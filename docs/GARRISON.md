# Garrison — three in-building firing systems

PayloadExt now supports **three**, chosen per building. They are genuinely
different mechanics, not variants:

| Mode | Who owns the weapon | How it fires | INI |
|---|---|---|---|
| **YR garrison** (stock) | the **occupant** (`OccupyWeapon`) | building fires ONE shared weapon, round-robining the crew | `CanOccupyFire=yes` |
| **RA2 mode** (§2) | the **building** | building fires its own weapon, faster per crewman | `CanOccupyFire=no` + `CanOccupyFire.RA2Mode=yes` |
| **OpenTopped** (§3) | each **occupant**, independently | every occupant fires its OWN weapon out simultaneously, like BFRT passengers | `CanOccupyFire=no` + `OpenTopped=yes` |

RA2 mode and OpenTopped are mirror images: one makes the structure the gun and the
infantry the crew, the other makes the structure a firing position and the
infantry the guns.

---

## Original: the RA2-style crewed building weapon

Rex's spec:

> Originally, in Red Alert 2, buildings had weapons on them. When infantry
> entered these buildings, they started to fire. The more infantry inside, the
> faster the building fired said weapon. […] it would be a great start to have a
> building weapon that requires infantry inside.

This is the **`Garrison` fire mode** from DESIGN.md §2.1a, and it is genuinely
different from open-topped (where each occupant fires its *own* weapon). Here the
**building** owns the weapon and the occupants are *crew*.

---

## 1. What vanilla already does (verified by disassembly)

Inside `TechnoClass::RearmDelay`, a clean `gamemd.exe` contains:

```asm
6FD150   call [vtable+0x400]   ; TechnoClass::CanOccupyFire()
6FD15A   test al, al
6FD15C   je   0x6FD1B1         ; not occupy-firing -> no ROF bonus at all
6FD15E   call [vtable+0x408]   ; TechnoClass::GetOccupantCount()
6FD168   test eax, eax
6FD16A   jle  0x6FD183         ; count <= 0 -> skip the divide
6FD170   call [vtable+0x408]   ; count again
6FD17B   idiv ecx              ; rof = rof / occupantCount     <-- "more = faster"
6FD183   ...                   ; then flat RulesClass::OccupyROFMultiplier (+0xF44)
```

So **"more infantry = faster firing" is vanilla**, implemented as an integer
divide of the rearm delay by the occupant count. Two `TechnoClass` virtuals drive
it, both declared in YRpp:

| vtable | YRpp declaration | Role |
|---|---|---|
| `+0x400` | `virtual bool CanOccupyFire() const` | gate: is this occupy-firing? |
| `+0x408` | `virtual int GetOccupantCount() const` | the divisor |

Related vanilla INI on `BuildingTypeClass`: `CanBeOccupied`, `CanOccupyFire`,
`MaxNumberOccupants`, `ShowOccupantPips`. The weapon actually fired on the vanilla
occupy path comes from the **occupant**, not the building —
`InfantryTypeClass::OccupyWeapon` / `EliteOccupyWeapon`, cycled via
`BuildingClass::FiringOccupantIndex`.

**Therefore the missing piece is narrow:** vanilla ties the ROF bonus to the
*occupy path* (which fires the infantry's `OccupyWeapon`). A building that simply
has its own `Primary=` never reaches that code. We supply the gate and the
divisor for that case.

> ⚠ **Worth testing before writing more code:** setting `CanOccupyFire=yes` on a
> building that also has its own `Primary=` may already route it through the
> vanilla block above and give the count-scaling for free. Nobody has tried it.
> If it works, `Crew.ROFPerOccupant` below is redundant for that configuration.

---

### 1a. Which INI tags a crewed building actually needs (verified by disassembly)

`BuildingClass::CanBeOccupiedBy` (`0x457CE0`) is the entry gate. Verified field
offsets on `BuildingTypeClass`: **`+0x157B` = `CanBeOccupied`**,
**`+0x157C` = `CanOccupyFire`**, **`+0x1580` = `MaxNumberOccupants`**.

```asm
457CF9  mov  cl, [Type+0x157B]     ; CanBeOccupied
457CFF  test cl, cl
457D01  je   fail                  ; -> REQUIRED
...
457D79  call [vtable+0x408]        ; eax = live GetOccupantCount()
457D85  cmp  eax, [Type+0x1580]    ; vs MaxNumberOccupants
457D8B  je   fail                  ; count == max -> full, reject
```

| Tag | Required? | Why |
|---|---|---|
| `CanBeOccupied=yes` | **YES** | first gate; without it nobody can ever enter |
| `MaxNumberOccupants=X` | **YES, > 0** | the compare is `count == max → reject`. Left at 0, an *empty* building has `0 == 0` and is rejected, so nobody can enter |
| `Passengers=X` | **NO** | different container entirely — that is the FootClass transport list (`Passengers.NumPassengers`). `GetOccupantCount` is `mov eax,[this+0x694]; ret`, i.e. `Occupants.Count` on the building instance |
| `CanOccupyFire=yes` | **NO** (for PayloadExt) | only gates *vanilla's* occupy-fire and the `0x6FD150` ROF block. Entry does not check it, and the `Crew.*` feature reads `GetOccupantCount()` directly — so `CanOccupyFire=no` still admits infantry and still lets the crewed weapon see them |

`BuildingClass::GetOccupantCount()` (vtable `+0x408` → `0x4581F0`) is literally
`mov eax,[ecx+0x694]; ret` — it reads the **live** count off the building
instance, not the Type, which is what makes the `0x6FD17B` divisor a genuine
"more men = faster" and not a flat per-type bonus.

`BuildingClass::CanOccupyFire()` (vtable `+0x400` → `0x458DD0`) =
`CanBeOccupied && CanOccupyFire && GetOccupantCount() > 0`.

### 1b. ⚠ CORRECTION (2026-08-22, from a failed in-game test)

The first test build produced a pillbox that **could not fire even when full**.
Root cause was the INI, not the DLL — and it invalidates part of §2 below.

**`BuildingClass::CanFire` (`0x447F10`) already gates on occupancy:**
```asm
447F15  mov  eax,[this+0x520]      ; ->Type
447F1B  mov  cl,[eax+0x157B]       ; CanBeOccupied
447F23  je   0x447F45              ; not occupiable -> normal path
447F25  mov  cl,[eax+0x157C]       ; CanOccupyFire
447F2D  je   0x44805A              ; occupiable + CanOccupyFire=no -> CANNOT FIRE
447F37  call [vtable+0x408]        ; GetOccupantCount()
447F3F  je   0x44805A              ; occupiable + empty      -> CANNOT FIRE
```

Two consequences:

1. **`CanBeOccupied=yes` + `CanOccupyFire=no` = a building that can never fire,
   period.** This is a hard vanilla block, hit before anything we do. My earlier
   claim that `CanOccupyFire` "only gates vanilla's occupy-firing" was **wrong**;
   it also gates the building's own `CanFire`.
2. With `CanOccupyFire=yes`, **vanilla already implements "requires infantry
   inside"** (`0x447F3F`) *and* "more infantry = faster" (`0x6FD17B`).

**So `Crew.Required` and `Crew.ROFPerOccupant` are both redundant** — and worse,
`Crew.ROFPerOccupant` would divide the rearm delay a *second* time on top of
vanilla's divisor. Both are disabled in the test rules pending the rewrite.

**The one thing vanilla genuinely will not do** is let the building fire its *own*
weapon while occupied. `BuildingClass::GetWeapon` (`0x4526F0`):
```asm
452738  call [vtable+0x400]        ; CanOccupyFire()  (= CanBeOccupied && CanOccupyFire && count>0)
452740  je   0x4527B4              ; false -> base TechnoClass::GetWeapon = building's OWN weapon
452742  mov  eax,[this+0x69C]      ; else FiringOccupantIndex
452752  mov  ecx,[this+0x688]      ; Occupants.Items
452770  mov  ecx,[occType+0xE20]   ; the OCCUPANT's OccupyWeapon
452785  call [occ_vtable+0x3F8]    ; (fallback) the occupant's own weapon
```
There is **no vanilla configuration** in which a garrisonable building fires its
own `Primary=`: `CanOccupyFire=no` makes it fire nothing at all, and
`CanOccupyFire=yes` makes it fire the occupant's weapon.

**Therefore the redesign:** require `CanOccupyFire=yes` (inherit vanilla's gate
and ROF divisor for free) and have PayloadExt override **only** weapon selection —
hook `0x452738`/`0x452740` so an opted-in building takes the `0x4527B4` branch and
uses its own weapon. That is a much smaller feature than v1, and it composes with
vanilla instead of duplicating it.

### 1c. Mixed crews — what actually fires? (answered from the engine)

`TechnoClass::Fire` at `0x6FF065` advances the crewman after **every shot**:

```asm
6FF06D  inc eax                 ; ++FiringOccupantIndex
6FF074  call [vtable+0x408]     ; GetOccupantCount()
6FF083  idiv ecx                ; index %= count
6FF085  mov [edi+0x69C],edx     ; wrap
```

So the building **round-robins through its crew, one shot each** — that is stock
engine behaviour, and RA2 mode inherits it because `GetWeapon` reads the same
`FiringOccupantIndex`.

**With an E1 and an E2 inside** (E1 → `GarrisonWeapon[1]`, E2 → `[0]`), the
building **alternates**: shot 1 heavy, shot 2 light, shot 3 heavy… Add a third
crewman and it cycles through all three. Kill one and the cycle shortens
immediately. This is also why `GarrisonWeapon[N].ROFMultiplier` is per-entry: the
delay after each shot belongs to whoever just fired it.

### 1d. Who may crew it — `Occupier.RA2Mode`

Vanilla gates garrison entry on `InfantryTypeClass::Occupier` (`+0xEB4`), read in
`CanBeOccupiedBy` at `0x457D4E`; if clear, the infantryman is diverted to the
*assault* branch and simply cannot enter. Vanilla only sets `Occupier=yes` on
**E1, E2 and INIT**, which would make RA2 mode useless for every other type.

```ini
[General]
Occupier.RA2Mode.Default=yes   ; global default (default: yes)

[SomeInfantry]
Occupier.RA2Mode=no            ; keep this type out
```

Only applies to buildings in RA2 mode; YR-mode garrisons keep vanilla's
`Occupier=` rule untouched.

## 2. Implemented (v2, `src/Hooks.CrewedWeapon.cpp`)

```ini
[Building]
CanBeOccupied=yes
MaxNumberOccupants=4
CanOccupyFire=no                      ; YR's occupant-weapon system OFF
CanOccupyFire.RA2Mode=yes             ; RA2's building-weapon system ON
CanOccupyFire.RA2Mode.MinOccupants=1  ; 0 = also fires while empty, at base rate
GarrisonWeapon.ROFPerOccupant=yes     ; default yes -- the RA2 feel
GarrisonWeapon.ROFMaxOccupants=0      ; 0 = uncapped

GarrisonWeapon[0]=LightGun            ; catch-all (empty .Infantry)
GarrisonWeapon[0].Exclude=E1
GarrisonWeapon[0].ROFMultiplier=1.0
GarrisonWeapon[1]=HeavyGun
GarrisonWeapon[1].Infantry=E1         ; GI only
GarrisonWeapon[1].ROFMultiplier=2.0   ; >1 slower, <1 faster
```

An empty `GarrisonWeapon` list falls through to plain `Primary=`/`Secondary=` —
literal RA2 `CABUNK01`.

### Hook points

| Address | Function | Role |
|---|---|---|
| `0x457D48` | `BuildingClass::CanBeOccupiedBy` | let non-`Occupier=` infantry crew an RA2-mode garrison (rejoins at `0x457D58`, so Antares' hook still runs) |
| `0x447F25` | `BuildingClass::CanFire` | re-open the gate `CanOccupyFire=no` slams shut at `0x447F2D`; fire iff crew ≥ MinOccupants |
| `0x452738` | `BuildingClass::GetWeapon` | return the building's garrison weapon; `nullptr` → `0x4527B4` = own weapon |
| `0x6FD1B1` | `TechnoClass::RearmDelay` | occupant-count divisor + per-entry ROFMultiplier |
| `0x679A15` | rules load | `[General]` defaults |

### Still to do
- **Firepower / Range per entry.** Needs separate hooks (damage application and
  weapon-range calculation) keyed to the active entry; not yet written.
- **OpenTopped buildings** (the BFRT `OpenTopped=yes` pattern — infantry inside
  fire *their own* weapon out). That is the mirror image of RA2 mode and is
  tracked separately in OPENTOPPED_RESEARCH.md.

## OLD v1 notes — SUPERSEDED, see §1b

```ini
[GACNST]                    ; any BuildingType that has its own weapon
Primary=BuildingGun

Crew.Required=yes           ; can only fire while manned
Crew.MinOccupants=1         ; how many bodies are needed (default 1)
Crew.ROFPerOccupant=yes     ; divide rearm delay by occupant count (the RA2 feel)
Crew.ROFMaxOccupants=4      ; cap the divisor; 0 = uncapped (default)
```

Opt-in only — a type that sets none of these is bit-for-bit vanilla.

### Hook points

| Address | Function | Stolen | Role |
|---|---|---|---|
| `0x6FC339` | `TechnoClass::CanFire` | 0x6 | **the gate.** Returns `CannotFire = 0x6FCB7E` when `GetOccupantCount() < Crew.MinOccupants`. `ESI` = Techno. |
| `0x6FD1B1` | `TechnoClass::RearmDelay` | 0x6 | **the divisor.** Join point *after* vanilla's whole occupy block and *before* the bunker block. |

`0x6FC339` is hooked by Antares, Ares, Kratos **and** Phobos — we redirect only
for types that opted in, and return 0 otherwise so everyone chains.

`0x6FD1B1` is hooked by **nobody** in the registry. It was chosen deliberately:
Phobos owns `0x6FD183` (occupy ROF mult) and `0x6FD1C7` (bunker ROF mult) and
*redirects* from both, so hooking either would mean arbitrating Phobos. Sitting
at the join point after them avoids that entirely and cannot double-apply
vanilla's own divisor. Verified stolen bytes: `8B 86 E4 02 00 00`
(`mov eax,[esi+0x2E4]`) — exact 6-byte boundary. On every path reaching it `EBP`
holds the current rearm delay, mirrored at `[ESP+0x14]`; we write **both**,
because Phobos's bunker hook reads that stack slot.

### Design notes
- Integer division throughout, matching vanilla's `idiv` — no floating point in
  logical state, so it stays deterministic across machines.
- The result is floored at 1: a rearm delay of 0 means "fire every frame".
- `GetOccupantCount()` is called **virtually**, so dispatch reaches the game's
  real implementation. (It is `R0` in YRpp, but that only bites on qualified,
  vtable-bypassing calls — YRpp's own `BuildingClass.h` calls it this way.)

---

## 3. Not yet done — the customization Rex wants beyond the start

- Per-occupant-**type** contribution (a Conscript worth less than a Guardian GI).
- Which weapon: currently the building's own `Primary=`. A `Crew.Weapon=` override
  and/or "strongest occupant picks the weapon" is a natural next step.
- Veterancy of occupants feeding the Index/Profile system (INDEX-SYSTEM.md).
- Damage/range scaling by crew, not just ROF.
- The `Payload.BuildingFireMode=None|Garrison|OpenTopped` selector itself
  (DESIGN.md §2.1a) is still **unimplemented** — v1 above is reachable directly
  via the `Crew.*` tags without it. Wiring it in is a cleanup step once the
  OpenTopped mode also exists.

---

## 4. Verification status

- `0x6FD150`–`0x6FD183` vanilla logic, and the `0x6FD1B1` / `0x6FC339` stolen
  bytes: confirmed by `objdump -d` of a clean `gamemd.exe`.
- `CanOccupyFire` / `GetOccupantCount` vtable slots: inferred from the call sites
  above (`+0x400`, `+0x408`) cross-referenced with YRpp's declaration order —
  **not** independently confirmed against a vtable dump.
- `0x6FC339` register convention + `CannotFire` exit: from Phobos's own hook at
  that address.
- **Not compiled, not run in game.** No behaviour here has been observed.

---

## 3. OpenTopped buildings (`src/Hooks.OpenToppedBuilding.cpp`)

The BFRT pattern for structures: each occupant fires **its own** weapon out,
independently and simultaneously — rather than the building round-robining one
shared occupy-weapon.

```ini
[Building]
CanBeOccupied=yes
MaxNumberOccupants=6
OpenTopped=yes        ; occupants shoot out, exactly as BFRT does
CanOccupyFire=no      ; keep the BUILDING itself silent (0x447F2D)
```

`OpenTopped` is a `TechnoTypeClass` field, so it **already exists on every
BuildingType** and the engine simply never acts on it there — setting it on a
building is a vanilla no-op. That makes it safe to reuse as the opt-in, and keeps
the spelling identical to BFRT.

### Why this needs code at all
`TechnoClass::EnteredOpenTopped` (`0x710470`) sets `InOpenToppedTransport`
(`+0x82`) and — the essential part — adds the passenger to
`LogicClass::Instance` (`0x87F778`) so it keeps receiving `Update()` ticks while
limboed. From there all the ordinary open-topped machinery applies. Buildings
hold their infantry in `Occupants` rather than `Passengers`, so this never runs
for them; we submit each occupant on entry and withdraw it on exit.

### Hook points
| Address | Role |
|---|---|
| `0x52297F` `InfantryClass::GarrisonBuilding` | entry: set `Transporter`, call `EnteredOpenTopped`. `EBP` = building, `ESI` = the infantry just appended. Antares hooks the same address; we return 0 so both chain. |
| `0x4580BD` unload loop | exit: `ExitedOpenTopped` + clear `Transporter`. `EDI` = `Occupants.Items[EBP]` (loaded at `0x4580B1`). Driven from the occupant alone, since `ExitedOpenTopped` ignores its `this`. |
| `0x457D48` `CanBeOccupiedBy` | the `Occupier.RA2Mode` permission now covers OpenTopped buildings too. |

### Known gaps / risks (untested)
- **Occupant death inside the building** is not explicitly withdrawn from the
  logic layer; the object is being destroyed anyway, but `KillOccupiers`
  (`0x4586D6`) may deserve a hook if anything misbehaves.
- Each occupant targets independently, so a building can engage several targets
  at once. That is the point, but it is a real balance change.
- `Transporter` pointing at a *building* is a configuration the engine never
  produces on its own. If something asserts on it, this is the first suspect.
