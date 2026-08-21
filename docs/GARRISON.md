# Garrison — the RA2-style crewed building weapon

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

## 2. Implemented (v1, `src/Hooks.CrewedWeapon.cpp`)

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
