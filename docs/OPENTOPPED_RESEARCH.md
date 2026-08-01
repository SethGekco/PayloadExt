# Feature 1 — OpenTopped for BuildingTypes (and InfantryTypes)

Goal (Rex): make `OpenTopped` usable on **BuildingTypes** (priority) and
**InfantryTypes**. Vanilla `OpenTopped` works only for `VehicleType` /
`AircraftType`. Buildings *already* let infantry inside — but via the **garrison
(Urban Combat) system**, which has unavoidable defects:

- **Internal Error** when occupants have different weapon **rate-of-fire** or
  **range**.
- Special weapons (e.g. **mind control**) break the garrison firing logic.

Rex wants garrisoned-style occupancy but with each occupant firing as *itself*
(its own weapon, ROF, range, target) — i.e. genuine open-topped behavior.

---

## Key finding: garrison and open-topped are TWO SEPARATE engine systems

Confirmed by reading YRpp + Phobos source (submodules).

### Open-topped passenger firing (what we want the building to use)
Each passenger fires **its own** weapon independently. Driven off the FootClass
cargo/passenger chain + the `ObjectClass::InOpenToppedTransport` flag
(`YRpp/ObjectClass.h:307`), set by `TechnoClass::EnteredOpenTopped` /
`ExitedOpenTopped` (`YRpp/TechnoClass.h:437,442`).

Phobos hook anchors on this path (all in `gamemd.exe`):
- `0x6FC5C7` — `TechnoClass::CanFire`, open-topped gate (Phobos
  `TechnoClass_CanFire_OpenTopped`). Checks `InOpenToppedTransport` + transport
  state before letting a passenger fire.
- `0x6FE43B` — `TechnoClass::FireAt`, applies `OpenToppedDamageMultiplier` when
  `pThis->InOpenToppedTransport`.
- `0x6FA33C` / `0x6F89F4` / `0x6F7EC2` / `0x6F8FD7` — threat-eval / target
  acquisition routed through the open-topped owner (Phobos
  `TechnoClass_ThreatEvals_OpenToppedOwner`).
- `RulesClass` globals: `OpenToppedDamageMultiplier` (+0x…), `OpenToppedRangeBonus`,
  `OpenToppedWarpDistance` (`YRpp/RulesClass.h:634-636`).
- Entry marking example: `Phobos/src/Ext/Team/Hooks.cpp:46`
  `pTransport->EnteredOpenTopped(pNext)` gated on `pType->OpenTopped`.

`OpenTopped` itself is a `TechnoTypeClass` bool (`YRpp/TechnoTypeClass.h:291`),
so the flag **already exists on `BuildingTypeClass`** (it derives TechnoType) —
the engine just never runs the open-topped firing loop for buildings.

### Garrison / occupant firing (what buildings use today — the buggy path)
Occupants live in a **different** container: `BuildingClass::Occupants`
(`DynamicVectorClass<InfantryClass*>`, `YRpp/BuildingClass.h:314`) with
`FiringOccupantIndex` (`:315`, comment cites `0x6FF074` — "which occupant gets
XP / which weapon fires"). Firing goes through a **single shared** occupy-weapon
slot; `MaxNumberOccupants` / `CanBeOccupied` on `BuildingTypeClass:215-217`.

Phobos anchors on this path:
- `BuildingType/Hooks.cpp:45` — `OccupierMuzzleFlashes[FiringOccupantIndex]`.
- `0x6FE3F1` `TechnoClass_FireAt_OccupyDamageBonus`, `0x6FE421`
  `TechnoClass_FireAt_BunkerDamageBonus`.
- `0x459069` `BuildingClass_UpdateTankBunker_CheckOccupants`,
  `0x458180`/`0x458060` occupant unload/clear.

**That single shared-weapon assumption is the root of the ROF/range Internal
Error and the mind-control break.** Because occupants are not in the passenger
chain and are not marked `InOpenToppedTransport`, the open-topped firing
machinery above never touches them.

---

## Two implementation routes

### Route A — occupants become real open-topped passengers (RECOMMENDED)
When infantry garrison a building flagged `OpenTopped=yes` (+ a PayloadExt
opt-in), register them into the building's **passenger cargo chain** and call
`EnteredOpenTopped` on each, instead of (or mirrored alongside) `Occupants`.
Then the existing open-topped machinery (`0x6FC5C7`, `0x6FE43B`, threat evals)
applies **for free** — each occupant fires its own weapon / ROF / range, and the
garrison shared-weapon path (with its Internal Errors + mind-control break) is
bypassed entirely.

- **Pro:** reuses the most engine code; directly delivers "each fires as itself";
  inherits all Phobos `OpenTopped.*` tuning keys.
- **Hard part:** buildings don't naturally run the passenger open-topped *update*
  (that loop is driven from FootClass/unit AI). Need to verify whether marking
  occupants as open-topped passengers is enough for the target-scan/fire loop to
  pick them up, or whether we must drive it from `BuildingClass::AI`
  (⚠ address TBD — Ghidra).
- **Entry mechanic decision:** keep the garrison enter command/animation (units
  still "occupy"), but back it with the passenger list. Need the occupant-add
  site (near `0x459069` / building occupy) to also do the passenger register +
  `EnteredOpenTopped`.

### Route B — graft open-topped firing onto the garrison loop
Keep `Occupants`; hook the garrison firing routine (near `0x6FF074` /
`FiringOccupantIndex`) so each occupant acquires its own target and fires its own
weapon with its own reload timer, instead of the shared occupy-weapon.

- **Pro:** keeps the existing garrison entry/unload path untouched.
- **Con:** reimplements per-occupant target acquisition + reload + mind-control
  handling by hand — more new code, more sync surface, easy to drift from vanilla.

**Recommendation: Route A.** It is closer to "enable OpenTopped for buildings"
literally, and reuses the battle-tested open-topped firing/targeting/damage
machinery rather than re-deriving it. Fall back to B only if the building update
cannot be made to drive the open-topped passenger loop.

---

## Open questions for Rex before feature code
1. **Route A vs B** — confirm A (make occupants open-topped passengers) is the
   intended direction.
2. **Opt-in tag** — reuse bare `OpenTopped=yes` on the building, or a distinct
   `PayloadExt` key (e.g. `OpenTopped.Building=yes`) so existing garrison
   buildings are untouched? (Lean: distinct key → zero risk to vanilla garrison.)
3. **Entry** — keep the garrison "occupy" command as the entry gesture, or a
   transport-style load? (Lean: keep garrison entry; only firing changes.)
4. **Capacity source** — `MaxNumberOccupants` (garrison) vs `Passengers`/
   `SizeLimit` (transport) for the cap.

## Next actions (once route confirmed)
- ⚠ Ghidra: dive `BuildingClass::AI` / occupant update to find where (and
  whether) an open-topped passenger firing loop would run for a building; record
  the address in this file + the Hook Encyclopedia (`Ext-Payload.md`).
- ⚠ Ghidra: dive the occupant-add site to place the passenger-register +
  `EnteredOpenTopped` call.
- Stand up `Ext/BuildingType` + `Ext/Building` containers (mirror the
  TechnoAttachmentExt Container<T> pattern) and the opt-in tag parse.

All addresses above are from Phobos source (release channel) and are ⚠ until
re-verified against `/home/rex/gamemd.exe` in Ghidra.
