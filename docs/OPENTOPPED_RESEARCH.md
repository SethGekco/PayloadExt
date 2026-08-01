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

## UPDATE (2026-07-31): both systems are in scope, as a per-building toggle

Rex's direction: **don't pick garrison *or* open-topped — offer both**, toggleable
per building. They are "similar but not identical" and each has its uses:
- **Improved garrison** (RA2-style): keep the shared occupy-weapon feel but fix
  its defects (ROF/range Internal Error, mind-control break).
- **Open-topped building**: each occupant fires its own weapon (Route A below).

This fits the PayloadExt **Bay** primitive cleanly: a building's infantry hold is
a `Bay(Cargo)` with a `FireMode = Garrison | OpenTopped` policy. Same entry
gesture (garrison "occupy"), different firing path.

> ⚠ Open for Rex: what specifically makes the intended garrison mode "**RA2**-style"
> vs the current YR one? The engine has *three* separate in-building systems
> (Garrison/UC, Battle-/Tank-Bunker, Open-Topped — see Encyclopedia page below);
> need Rex to name the exact behavioural target for "improved RA2 garrison."

### Prior art found (Encyclopedia registry) — this reshapes build-vs-reuse

- **Phobos PR #1879** — *"Implement open-topped building support and fix building
  self-attack issue"* (@Metadorius), **open/unmerged**. Hooks infantry/unit
  `PerCellProcess` at building-enter (`0x51A320` / `0x73A2F4`,
  `SubmitToOpenToppedOnBuildingEnter`) + disable-on-unload/debris
  (`0x44DBA9` / `0x442D97`). This is exactly Route A. **PayloadExt cannot depend
  on an unmerged PR**, so implement our own Route A using these addresses, and
  stay compatible if #1879 later merges (don't double-hook the enter point).
- **Antares "Trenches"** (`Ext/Building/Hooks.Trenches.cpp`) — the garrison
  extension surface: `GarrisonBuilding_OccupierEntered` (`0x52297F`),
  `CanBeOccupied_SpecificOccupiers/Assaulters`, `UnloadOccupants`,
  `KillOccupiers`. The **improved-garrison** mode should layer on / coexist with
  Trenches, not fork it.

### Confirmed Route-A mechanism (release Phobos source)

`Phobos/src/Ext/Techno/Body.Update.cpp` (~1234): `EnteredOpenTopped(passenger)`
**adds the passenger to the logic layer** (`LogicClass::Instance`) and sets
`passenger->Transporter`, so it receives update ticks and fires; `ExitedOpenTopped`
reverses it. This runs only for units in `Passengers`. Buildings keep infantry in
`Occupants`, so it never runs — the comment there literally says *"OpenTopped does
not work properly with buildings to begin with."* So Route A = at building-enter,
put the occupier into the passenger/logic-layer path + `EnteredOpenTopped`.

Full hook surface + verification status: **Encyclopedia
`encyclopedia/Ext-Building-Occupancy.md`** (added this session).

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

**Decision (2026-07-31): ship BOTH as a per-building `FireMode` toggle.** Route A
is the **OpenTopped** mode (validated by PR#1879 + the release-Phobos mechanism
above). Route B's per-occupant work becomes the **improved Garrison** mode — but
scoped to *fixing* the shared-weapon path (ROF/range Internal Error, mind-control)
while keeping garrison semantics, layered on Antares Trenches. A building selects
one mode; the entry gesture (garrison "occupy") is shared.

---

## Open questions for Rex before feature code
1. ~~Route A vs B~~ — **resolved**: ship both as a `FireMode` toggle (see Decision).
2. **"RA2-style" garrison** — what specifically should the improved-garrison mode
   do differently from the current YR garrison? (Engine has 3 separate systems:
   Garrison/UC, Battle-/Tank-Bunker, Open-Topped — need the exact target.)
3. **Opt-in / mode tag** — one tri-state key on the building selecting the hold's
   fire mode, e.g. `Payload.BuildingFireMode=none|garrison|opentopped` (default
   `none` = vanilla, so existing garrison buildings are untouched). Confirm the
   spelling. (Lean: distinct PayloadExt key, never overload bare `OpenTopped=`.)
4. **Entry** — keep the garrison "occupy" command as the shared entry gesture for
   both modes. (Lean: yes; only the firing path differs.)
5. **Capacity source** — `MaxNumberOccupants` (garrison) vs `Passengers`/
   `SizeLimit` (transport) for the cap in open-topped mode.

## Next actions (route resolved; awaiting Q2/Q3 spellings)
- ⚠ Ghidra/PR#1879: confirm the building-enter submit point (`0x51A320` infantry
  / `0x73A2F4` unit) and how PR#1879 registers the occupier as an open-topped
  passenger; do NOT double-hook if #1879 is present. Record in
  Encyclopedia `Ext-Building-Occupancy.md`.
- ⚠ Ghidra: confirm where the open-topped passenger fire/target loop is driven so
  a building occupier put on that path actually fires (logic-layer membership via
  `EnteredOpenTopped` appears to be the key — verify).
- Improved-garrison mode: locate the ROF/range Internal-Error site + the
  mind-control interaction on the shared occupy-weapon path; layer on Antares
  Trenches (`0x52297F` `OccupierEntered`, etc.), don't fork.
- Stand up `Ext/BuildingType` + `Ext/Building` containers (mirror the
  TechnoAttachmentExt Container<T> pattern) + the `FireMode` tag parse.

All addresses above are from Phobos/Antares source (release + PR#1879 channel) and
are ⚠ until re-verified against `/home/rex/gamemd.exe` in Ghidra. Full hook
surface: Encyclopedia `encyclopedia/Ext-Building-Occupancy.md`.
