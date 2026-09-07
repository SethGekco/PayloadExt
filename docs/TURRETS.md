# Turrets — index selectors and voxel drawing

Why turrets live in PayloadExt: the project already owns `Gunner=` (a Bay of
kind `Gunner`, DESIGN.md §2.3), and "which turret is showing" is the same
question as "which weapon slot is live." One selector drives both.

---

## 1. What the engine already gives us (verified)

`TechnoClass::CurrentTurretNumber` (`TechnoClass.h:600`, commented *"for
IFV/gattling/charge turrets"*) selects which turret voxel is drawn. Vanilla
varies it in only three situations:

| Situation | Who sets it |
|---|---|
| Gattling stage | engine (`IsGattling`) |
| Charge-turret animation frame (the **SREF** pattern) | engine (`IsChargeTurret`) |
| IFV gunner / passenger | engine `TechnoClass::SwitchGunner` (`0x70DC70`) |

**Vanilla already stores a per-weapon turret index.**
`TechnoTypeClass::GetWeaponTurretIndex(int)` at **`0x7178B0`** (name confirmed in
the Antares PDB map) disassembles to:

```asm
mov eax, [esp+4]                 ; weapon index
mov eax, [ecx + eax*4 + 0x814]   ; per-weapon turret index array on TechnoTypeClass
retn 4
```

So the array lives at `TechnoTypeClass + 0x814`. **Antares** exposes it as INI
(`WeaponTurretIndex<N>=`, and `<Name>TurretWeapon=` + `<Name>TurretIndex=` for
the IFV-style named list) and hooks `0x7178B0`/`0x717890` only to serve weapon
indices `>= 18` out of its own overflow vector — indices below 18 are pure
vanilla.

### The gap (this is exactly Rex's request #2)

Antares applies that mapping in **one** place — its `TechnoClass_SwitchGunner`
hook (`0x70DC70`), which the engine calls on the **gunner** path:

```cpp
if(!pType->IsChargeTurret) {
    pThis->CurrentTurretNumber = *pExt->GetWeaponTurretIndex(index);
    pThis->CurrentWeaponNumber = index;
}
```

An ordinary vehicle with `Primary=`/`Secondary=` never goes through
`SwitchGunner`, so `CurrentTurretNumber` is never updated when it switches
weapons — the turret does not change. The data and the mapping already exist;
only the *application* is missing. That is why this feature is small.

---

## 2. Implemented (v1, `src/Hooks.Turrets.cpp`)

Two opt-in selectors on any `TechnoType` with `TurretCount > 0`:

```ini
[HTNK]
TurretCount=3

; (a) turret follows the weapon being fired — reuses the engine/Antares
;     per-weapon mapping described above
Turret.FollowWeapon=yes
WeaponTurretIndex1=0        ; Antares spelling, unchanged
WeaponTurretIndex2=1

; (b) turret follows distance to the current target
Turret.RangeBands=4,8       ; ascending thresholds, in cells
Turret.RangeIndices=0,1,2   ; one MORE entry than bands
;   dist < 4        -> turret 0
;   4 <= dist < 8   -> turret 1
;   dist >= 8       -> turret 2
```

`Turret.RangeIndices` may be omitted, in which case band N maps to turret N.
When both selectors are set, **range wins while a target exists** (it is the
continuous one) and `FollowWeapon` applies at fire time otherwise.

### Hook points (both verified by disassembling a clean `gamemd.exe`)

| Address | Function | Stolen | Why this one |
|---|---|---|---|
| `0x6FDDC0` | `TechnoClass::FireAt` | 0x6 | has the weapon index (`[EBP+0xC]`); `ESI`=Techno, `EBX`=Weapon. `mov al,[ebx+0x144]` is exactly 6 bytes. Phobos also hooks it (`_BeforeTruelyFire`) — we always `return 0` so both chain. |
| `0x6F9E50` | `TechnoClass::Update` | 0x5 | per-frame, so a range selector tracks a moving target. Function start: `sub esp,0x68 / push ebx / push ebp` = exactly 5 bytes; `ECX` = `TechnoClass*`. **Antares, Ares, Kratos and Phobos all already hook this same entry** — the canonical benign shared call site. |

### Safety guards (all three matter)

1. `TurretCount <= 0` → skip. Not a multi-turret type.
2. `IsGattling || IsChargeTurret` → **skip**. The engine encodes gattling stage /
   charge animation frame in `CurrentTurretNumber`; writing it would fight the
   engine every frame. Antares' own `SwitchGunner` hook guards `IsChargeTurret`
   for exactly this reason. *(This is why Rex's "use the SREF charge system but
   drive it by distance" is implemented as a separate selector rather than by
   reusing the charge path — the charge path's index is owned by the animation.)*
3. Not opted in → skip. Untagged types are bit-for-bit vanilla.

Resolved indices are clamped to `TurretCount` before being written, because an
out-of-range value would index past `ChargerTurrets[]` during drawing.

---

## 3. Request #3 — voxel body should occlude the turret (NOT yet implemented)

Today turrets always draw on top regardless of HVA position. The relevant
address is **`0x73BA12`**, and there is a **major complication**:

> Phobos's `UnitClass_DrawAsVXL_RewriteTurretDrawing` hooks `0x73BA12` and
> **replaces the entire voxel turret+barrel draw block wholesale**, returning
> `SkipGameCode = 0x73BEA4` past all of it.

Consequences:
- On any Phobos build, the vanilla turret draw code **does not run at all**. A
  PayloadExt draw-order fix must either beat Phobos to the address (load-order
  dependent, fragile) or be built *into* a rewrite of our own.
- Good news: Phobos's rewrite reads `currentTurretNumber` off the stack, so our
  §2 selectors **compose correctly** with Phobos's drawing.
- Antares separately owns `UnitClass_DrawVXL_Turrets` (`0x73BD15`) and
  `DrawVXL_Barrels1/2/3` for the multi-turret art.

**DECIDED (Rex, 2026-08-20): supersede.** PayloadExt ships its **own** rewrite of
the voxel turret/barrel draw block and wins over Phobos when both are loaded.
Rationale: stay independent — Phobos has a strict rule against AI-generated
contributions, so upstreaming is not an option, and depending on their rewrite
would couple us to code we cannot change.

Implementation shape for that phase (not yet built):
- Hook `0x73BA12` like Phobos does and return `SkipGameCode = 0x73BEA4`, so the
  vanilla block is skipped exactly once regardless of who else is present.
- **Load order matters.** Both DLLs hook the same address; whoever Syringe calls
  first and returns non-zero wins. We must verify empirically which order the
  game loads them in, and document it — this is the one fragile part.
- Draw body / turret / barrel with proper depth ordering derived from the HVA
  section transforms, instead of the unconditional body→turret→barrel sequence
  both vanilla and Phobos use. That ordering *is* the feature.
- Keep reading `CurrentTurretNumber` from the stack slot Phobos uses
  (`STACK_OFFSET(0x1C4, -0x1A8)`) so the §2 selectors keep working.
- Must also account for Antares' `UnitClass_DrawVXL_Turrets` (`0x73BD15`) and
  `_Barrels1/2/3`, which live *inside* the block we would be skipping.

---

## 4. Verification status

- `0x7178B0`, `0x6F9E50`, `0x70DC70`, `0x717890` — names confirmed against the
  **Antares PDB symbol map** (`~/Claude/Antares/gamemd_names_from_antares_pdb.txt`);
  instruction bytes at `0x7178B0` and `0x6F9E50` confirmed by `objdump` of a
  clean `gamemd.exe`.
- `0x6FDDC0` — instruction boundary confirmed by `objdump`; not present in the
  (partial) PDB map. Register convention taken from Phobos's hook at the same
  address.
- `0x73BA12` behaviour — read directly from Phobos `Ext/TechnoType/Hooks.MatrixOp.cpp`.
- **Not yet compiled or run in game.** No in-game verification of the selectors.

---

## 5. TODO — remove `RepairTurretIndex` if the rules-side fix is sufficient

**Rex's standing principle: the DLL should not meddle with things that already
work.** `RepairTurretIndex()` in `src/Hooks.Turrets.cpp` currently rewrites an
out-of-range `CurrentTurretNumber` to 0. That is a workaround for someone
else's value, and it is exactly the kind of silent repair that principle warns
about — it papers over a bad index instead of letting it be visible.

Background (diagnosed 2026-09-05): with `IsChargeTurret=no`, Antares'
`TechnoClass_SwitchGunner` (`0x70DC70`) assigns
`CurrentTurretNumber = *GetWeaponTurretIndex(0)`, i.e. `TurretWeapon[0]`, which
defaults to **-1** when the type sets no per-weapon turret mapping. A negative
index makes Phobos fall back to the (Antares-emptied, NULL) `TurretVoxel` and
skip the draw, and makes Antares read `&ChargerTurrets[-1]`. Net effect: the
turret is invisible until the first shot.

`[SREF]` now carries `WeaponTurretIndex1=0`, which fixes it in the rules with no
code involved. Antares even validates this key at parse time
(`"Weapon %d on [%s] has an invalid turret index of %d."`) — though that warning
appears to be suppressed in our build, which is why it never surfaced.

**To decide:**
1. With `WeaponTurretIndex1=` set, does `RepairTurretIndex` ever fire? It logs
   once per type when it does — if the log stays silent across a few games, the
   repair is dead code and should come out.
2. If it does still fire, find out who is writing the bad index and fix that,
   rather than continuing to correct it after the fact.
3. Either way, prefer surfacing the bad value (a one-line warning naming the
   type and the missing key) over silently substituting 0.


### 5.1 Status after the rules fix (2026-09-05)

`[SREF]` now sets `WeaponTurretIndex1=0` and **the turret renders correctly
before firing** — the user-visible bug is fixed.

But the repair **still fired once**:

    [PayloadExt] SREF: CurrentTurretNumber was -1, outside 0..3 — repaired to 0.

So the rules key did NOT eliminate the -1, and criterion (1) above is **not**
met — do not delete `RepairTurretIndex` yet.

Ruled out since:
- **Antares' `PassengerTurret` path** (`Hooks.cpp` ~128,
  `CurrentTurretNumber = min(NumPassengers, TurretCount-1)`, which yields -1
  with no passengers) — `[SREF]` does not set `PassengerTurret`.
- **Any immediate-form write of -1** to `[reg+0x124]`: zero matches across
  gamemd-spawn.exe, Antares.dll, Phobos.dll and our DLLs. So it arrives via a
  register, not a constant store.
- The edit itself: `rulesmd.ini` still holds the key, there is exactly one
  `[SREF]` section, and no map overrides it.

Open question, and what the next game answers: the repair now also logs
`TurretWeapon[0]` (type+0x814, confirmed against the game's own setter at
`0x717890` — `mov [ecx+eax*4+0x814],edx`). That is both what
`WeaponTurretIndex1=` sets and what `SwitchGunner` copies, so:

- `TurretWeapon[0]=0` → the key landed, and the -1 has a **different** source
  still to find (most likely the value present at unit creation, before
  `SwitchGunner` first runs).
- `TurretWeapon[0]=-1` → the key is not reaching `ReadWeapons`, and the real fix
  is in the rules/parse path, after which the repair can be deleted.


### 5.2 RESOLVED — there is no rules-side fix; the repair stays (2026-09-06)

`TurretWeapon[0]=-1` in game **after** `WeaponTurretIndex1=0` was added, so the
key never landed. Cause:

    DEFINE_HOOK(0x747BCF, UnitTypeClass_LoadFromINI_Turrets, 0x5)
    {
        if(pThis->Gunner) {                                   // <-- gated
            TechnoTypeExt::ExtMap.Find(pThis)->LoadTurrets(pINI);
        }

`LoadTurrets()` is the **only** caller of the `WeaponTurretIndex%u` /
`<Name>TurretWeapon` parse loops, and it runs only for `Gunner=yes` units (its
`TurretNames` list — Normal, Repair, MachineGun, Flak, … — is the IFV set).
`[SREF]` is not a gunner, so the key is never read and `TurretWeapon[]` keeps
its -1 default.

**But `SwitchGunner` (`0x70DC70`) consumes it regardless of `Gunner`:**

    if(!pType->IsChargeTurret) {
        pThis->CurrentTurretNumber = *pExt->GetWeaponTurretIndex(index);
    }

So Antares reads a field it only ever populates for gunner units, and applies it
to every non-`IsChargeTurret` type. A non-gunner multi-turret unit therefore
always receives -1. **That is an Antares gap, not a mod misconfiguration**, and
it cannot be fixed from the rules — `WeaponTurretIndex1=` has been commented out
in `rulesmd.ini` with this explanation recorded in place.

**Conclusion: `RepairTurretIndex` stays.** Criteria (1) and (2) in §5 are both
resolved — it does fire, and the writer is Antares' own path with no INI
override available. The remaining option is upstream: report to the
Antares/Phobos channel that `SwitchGunner` should skip the assignment (or clamp
it) when the index is negative / when `LoadTurrets` never ran.

Related: the seam fix at `0x6FA5BE` (§2) is a *different* issue and should stay
— that one stops the engine overwriting a value we legitimately own, rather than
overriding a value we do not.
