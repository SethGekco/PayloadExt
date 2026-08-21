# PayloadExt — Design

A standalone Syringe DLL for RA2/Yuri's Revenge that generalizes **what a unit or building
carries and releases** — passengers, ammo, spawn-craft, gunner weapons, and the missiles /
airstrikes it (or an off-map superweapon) launches. Everything is driven by three reusable
primitives: the **Bay**, the **Deploy gesture**, and the **Launched Object**. Designed to
*layer on top of* **Antares** and **Phobos**, never to fork their single-slot systems.

> Name: **PayloadExt** (settled). Scope is "carried & launched objects."
>
> Toolchain note: this project targets **Antares** — the open-source Ares reimplementation with
> full feature parity — not closed-source Ares (see [[antares-replaces-ares]]). Being open source,
> Antares' behavior can be read and built against, which matters here because several bays *wrap*
> Antares' own slots (Gunner/IFV, Spawns, Ammo). "Ares" in older family notes = Antares going forward.

---

## 1. The one idea — three primitives

Vanilla (and even Antares/Phobos) treat each of these as a *single hardcoded slot*:
one passenger list, one ammo counter, one spawner, one gunner weapon, one deploy action,
one projectile guidance model. Every feature Rex listed is "give me **N** of that slot,
with **open/closed** state, **ordering**, **timing**, and **veterancy-indexed** behavior,
and let **off-map** be a valid origin."

So the whole list collapses to:

### Primitive A — the **Bay**
> A Bay is *an ordered queue of held items* + *a policy*. Policy = `{ kind, open?, active-index,
> load-order, release-order, per-slot timing, veterancy→index map }`.

`kind ∈ { Cargo, Ammo, Spawner, Gunner }`. A Techno may own **several** Bays. This single
container unifies:
- **Cargo** holds (passengers) → open-topped, unload ordering.
- **Ammo** queues → multiple ammo pools, switchable.
- **Spawner** tags → multiple spawn types, switchable.
- **Gunner** weapon slots → IFV-style weapon-per-passenger, but multiple.

### Primitive B — the **Deploy gesture**
> A gesture is *an input* + *preconditions* + *an action on a Bay or weapon*.

Generalizes the single hardcoded "deploy" into a set of named gestures: fire a deploy weapon,
unload a Bay in order, switch the active spawner/ammo/fire-mode, transform — each gated by
preconditions (cargo must be empty, must be a quick double-tap, must be off cooldown) and bound
to an input (the deploy key **or a new alternate key**).

### Primitive C — the **Launched Object**
> A launched object is *a released entity* + *a guidance profile* + *an origin*.

`guidance = { homing?, precision, speed, turn-rate, retarget? }`; `origin ∈ { Bay (on-unit),
OffMap-SW, OffMap-Designator }`; plus `targetable?`. Unifies spawned tracking missiles, off-map
missiles you can shoot down, and off-map airstrike/superweapon deliveries — the *only* thing
that differs between them is origin and guidance, not the object.

**Mapping the request to the primitives** (every bullet lands somewhere):

| Rex's ask | Primitive |
|---|---|
| Open-topped **bunkers** (buildings) | Bay(Cargo).open on BuildingType |
| **OpenTopped index** | Bay(Cargo).fire-index |
| **Veteran / Elite** tags → which index | Bay.veterancy→index map (§2.2) |
| Multiple cargo queues; which are open; deploy/unload order | Multiple Bay(Cargo) + open + order (§2) |
| Control deploy weapons, unloading, order, timing, delays | Deploy gesture release schedule (§3) |
| Custom deploy abilities (require empty, double-deploy) | Deploy gesture preconditions (§3.2) |
| Multiple **Gunner** weapons | Bay(Gunner) (§2.3) |
| Multiple **ammo** queues | Bay(Ammo) (§2.4) |
| Multiple **Spawner** tags; control when you switch | Bay(Spawner) + switch policy (§2.5) |
| Weapon **fire modes** via Deploy | Deploy gesture → switch active-index (§3.3) |
| New alternate Deploy key | Deploy gesture input binding (§3.4) |
| Tracking spawned missiles (varied precision/speed) | Launched Object guidance (§4) |
| Off-map missiles (SW / laser designator), destroyable | Launched Object origin=OffMap + targetable (§5) |
| Weapons fired from off-map (SW, new airstrike type) | Launched Object origin=OffMap (§5) |

---

## 2. The Bay (Primitive A)

### 2.1 Declaration

A Techno declares its bays by name; each bay is a self-contained section. Bay index 0 with no
declaration = **vanilla behavior** (the single Passengers list / single Ammo / single Spawn),
so untagged units are untouched.

```ini
[HTNK]                          ; or a BuildingType, for open-topped bunkers
Bays=HOLDA,HOLDB,MISSILEPOD

[HOLDA]
Kind=Cargo
Open=yes                        ; open-topped: passengers can fire out
Open.Index=0                    ; which fire-out behavior profile to use (§2.2)
Size=5                          ; capacity (passenger "slots" / SizeLimit)
UnloadOrder=LIFO                ; FIFO | LIFO | Explicit
LoadOrder=FIFO
Unload.Delay=15                 ; frames between ejecting each occupant
Unload.Weapon=                  ; optional weapon fired per ejected occupant (§3.1)

[HOLDB]
Kind=Cargo
Open=no                         ; a *closed* second hold on the same transport
Size=3

[MISSILEPOD]
Kind=Spawner
Spawns=DMISL                    ; reuse vanilla/Antares Spawner keys inside a bay
SpawnCount=4
```

**Open-topped for buildings** is just `Kind=Cargo` + `Open=yes` on a BuildingType — the feature
"open-topped bunkers" is the Cargo bay unhardcoded from vehicles-only.

### 2.1a Building fire mode — garrison vs open-topped (both supported, toggleable)

The engine has **three separate in-building firing systems** (garrison/UC,
Battle/Tank-Bunker, open-topped) — see the Encyclopedia page
`Ext-Building-Occupancy.md`. RA2-style **garrison** and **open-topped** are *both*
desirable and *similar but not identical*, so a Cargo bay on a BuildingType picks
one via a fire-mode policy (shared "occupy" entry gesture, different firing path):

```ini
[GATECH]                 ; a BuildingType with an infantry hold
Bays=BUNKERHOLD
[BUNKERHOLD]
Kind=Cargo
FireMode=OpenTopped      ; None (vanilla) | Garrison | OpenTopped
Open=yes                 ; (OpenTopped mode) each occupant fires its own weapon/ROF/range
```

- **`Garrison`** — the RA2-style **crewed building weapon**: the *building* owns
  the weapon, it only fires while infantry are inside, and it fires faster the
  more occupants there are. This sidesteps the YR garrison's defects (Internal
  Error on mismatched occupant ROF/range, mind-control breaking the shared-weapon
  arbitration) because there is only ever **one** weapon — the building's.
  Implemented via the `Crew.*` tags; see docs/GARRISON.md.
- **`OpenTopped`** — each occupant is submitted to the open-topped system
  (`EnteredOpenTopped` → logic layer) and fires its **own** weapon independently,
  sidestepping the shared-weapon defects entirely. (This is what Phobos PR#1879
  does; PayloadExt implements it standalone since #1879 is unmerged.)
- **`None`** — vanilla garrison, untouched. Default, so existing buildings are safe.

Deferred/related: InfantryType open-topped (transport-infantry that themselves hold
open-topped passengers) rides the same Cargo-bay mechanism, lower priority.

### 2.2 Veterancy → OpenTopped index (the clarified feature)

The **OpenTopped index** selects a *fire-out behavior profile*; Veteran/Elite pick a different
index. Per Rex's clarification, the tags exist so a bunker/transport fires differently when
**veteran or elite**:

```ini
[HOLDA]
Kind=Cargo
Open=yes
Open.Index=0                    ; rookie / base
Open.Index.Veteran=1
Open.Index.Elite=2

[OpenToppedProfiles]            ; named, reusable fire-out profiles
0=OT_BASE
1=OT_VET
2=OT_ELITE

[OT_ELITE]
RangeBonus=2                    ; superset of Phobos OpenTopped.RangeBonus et al.
DamageMultiplier=1.5
WarpDistance=0
ShareTransportTarget=yes        ; honor Phobos' key name
FireFX=                         ; optional muzzle/anim override
```

- **Open decision (§12):** is veterancy read from the **transport** (the bunker's rank) or the
  **passenger** firing out? Rex's phrasing ("Veteran/Elite tags … specify an OpenTopped index
  based on being vet or elite") reads as the *transport's* rank selecting the index — that's the
  default; a `Open.VeterancySource=Transport|Passenger` key covers the other reading.

### 2.3 Gunner bays (multiple gunner weapons)

Antares `Gunner=yes` already swaps the vehicle's weapon based on its single passenger. A
`Kind=Gunner` bay generalizes it to **N** gunner slots, each mapping passenger → weapon:

```ini
[GUNBAY]
Kind=Gunner
Weapon.Occupant.E1=Gun_Rifle    ; per-passenger-type weapon
Weapon.Occupant.E2=Gun_Rocket
Weapon.Default=Gun_Empty
Switch=OnLoad                   ; when to re-evaluate the active weapon
```

Layers on Antares' IFV logic: we choose which weapon the vehicle's slot points at; Antares still
fires it. Multiple gunner bays = multiple simultaneous weapon barrels driven by different
occupants (the "multiple gunner weapons" ask).

### 2.4 Ammo bays (multiple ammo queues)

```ini
[AMMOA]
Kind=Ammo
Ammo=6
Reload.Rate=30
Weapon=Weap_Shell               ; weapon this ammo pool feeds
[AMMOB]
Kind=Ammo
Ammo=2
Reload.Rate=120
Weapon=Weap_Nuke
```

Switching between ammo pools is a Deploy gesture (§3.3) or automatic-on-empty (`Switch=OnEmpty`).

### 2.5 Spawner bays (multiple spawner tags + switching)

```ini
[PODA]
Kind=Spawner
Spawns=HORNET
SpawnCount=4
[PODB]
Kind=Spawner
Spawns=CMISL
SpawnCount=2

; on the owning Techno:
Bay.Switch.Auto=OnTargetType    ; Manual | OnEmpty | OnTargetType | OnRange
Bay.Switch.OnTargetType.Air=PODA
Bay.Switch.OnTargetType.Ground=PODB
```

"Control when you switch" = the `Bay.Switch.*` policy: manual (a Deploy gesture), or automatic
by target class / range / empty.

---

## 3. The Deploy gesture (Primitive B)

Generalizes the one hardcoded deploy (`DeploysInto` / `IsSimpleDeployer` / `DeployFire`) into a
list of named gestures. Vanilla deploy = the default gesture with no preconditions.

```ini
[HTNK]
DeployGestures=UNLOADA,SWITCHMODE

[UNLOADA]
Input=Deploy                    ; Deploy | AltDeploy | Auto
Action=UnloadBay
Bay=HOLDA
Require.Cargo=NonEmpty           ; precondition (§3.2)
Cooldown=45

[SWITCHMODE]
Input=AltDeploy
Action=SwitchActiveIndex
Bay=MISSILEPOD
```

### 3.1 Release schedule (deploy weapons / unloading order / timing / delays)

`Action=UnloadBay` runs the bay's ordered release with per-step control:

```ini
[UNLOADA]
Action=UnloadBay
Bay=HOLDA
Order=LIFO                       ; overrides the bay default
Step.Delay=15                    ; frames between each ejected unit
Step.Weapon=Weap_DropFX          ; weapon/anim fired as each unit exits
Prefire.Delay=8                  ; delay before the first ejection
Postfire.Weapon=                 ; a weapon fired once the bay empties
```

This one schedule covers "control unit deploy weapons, unloading, the whole order and timing
and delays."

### 3.2 Preconditions (custom deploy abilities)

```ini
Require.Cargo=Empty | NonEmpty | Full     ; "require cargo empty before deploy"
Require.DoubleTap=6                        ; two deploy presses within N frames ("quick double deploy")
Require.Ammo=>=1                           ; needs ammo in the active bay
Require.Cooldown=45                        ; min frames since last gesture
Require.Veterancy=Elite                    ; rank-gated ability
Fail.FX=                                   ; feedback when preconditions unmet
```

Each precondition is a boolean the gesture ANDs before acting — a clean home for arbitrary
"customized deploy abilities."

### 3.3 Fire-mode switching (weapon fire modes via deploy)

`Action=SwitchActiveIndex` / `Action=CycleBay` retargets which Bay/weapon is live — this is how
"weapon fire modes using the deploy function" and "control when you switch" (spawners/ammo) are
the *same* action verb operating on different bay kinds.

### 3.4 Alternate deploy key (new input)

`Input=AltDeploy` binds a gesture to a **second** deploy input, distinct from the vanilla deploy
key. Requires a new keyboard command registered with the engine's command list.

- **Open decision (§12):** register a real hotkey command (rebindable in the keyboard config,
  cleanest, more hook surface) vs. piggy-back a modifier (Ctrl/Alt + deploy, less invasive).
  Lean: real command, so it appears in the keyboard UI like other commands.

---

## 4. Launched Objects on-unit (Primitive C) — tracking spawn-missiles

Spawned missiles with "varied precisions, speeds, etc." = a guidance profile attached to the
spawned projectile. This layers on **Phobos trajectories** (Straight/Missile/Bombard/Arcing)
rather than reinventing flight:

```ini
[DMISL]                          ; a spawned missile type
Guidance=HOMING_SLOPPY

[LaunchGuidance]
0=HOMING_SLOPPY
1=HOMING_PRECISE

[HOMING_SLOPPY]
Homing=yes
TurnRate=6                       ; ROT; low = wide, "imprecise" arcs
Precision=64                     ; aim jitter in leptons (synced RNG — §6)
Speed=40
Acceleration=2
Retarget=yes                     ; re-lock if the target dies/moves
Retarget.Precision=OnLoseLock
```

`Precision` jitter **must** draw from the synced game RNG (§6), never the render RNG — this is
exactly the [[kratos-rng-desync-rootcause]] trap.

---

## 5. Off-map origin (Primitive C, origin=OffMap)

The same Launched Object, but it originates off the playable map instead of from a bay.

### 5.1 Superweapon-launched
```ini
[NUKE-OFFMAP]                    ; a new SW
SW.Launches=CMISL_OFFMAP
[CMISL_OFFMAP]
Origin=OffMap-SW
EntryEdge=North                  ; where it flies in from
Guidance=HOMING_PRECISE
Targetable=yes                   ; can be shot down (§5.3)
Health=200
```

### 5.2 Laser-designator-launched
```ini
[LASERDES]                       ; a designator weapon/ability owned by a unit
Designator.Launches=CMISL_OFFMAP
Designator.Origin=OffMap-Designator
```
A ground unit "paints" a target; the missile arrives from off-map toward the painted cell
(the classic MO/Generals designator pattern).

### 5.3 Targetable / destroyable off-map ordnance

`Targetable=yes` makes the launched object a real, hittable entity (has Health, appears to AA)
rather than an invulnerable projectile — so "destroyable / target-able" incoming missiles are
just Launched Objects with a combat body.

- **Open decision (§12):** implement the targetable missile as a lightweight real Techno/aircraft
  (AA can lock it naturally, more overhead) vs. a projectile with an interception hook (cheaper,
  but AA targeting logic must be taught about it). Lean: real object for correctness.

### 5.4 Off-map weapons / new airstrike type

A pure "weapon fired from off-map" (no persistent missile to shoot down) is the same origin with
`Targetable=no` and an instantaneous or fast delivery — the "new airstrike weapon type." Where
Antares already covers a case (`AircraftDrop`, airstrike warheads), **defer to Antares** and only
add the off-map-*origin* + guidance layer.

---

## 6. Sync-safety model

All of these are **Logical game state** (cargo contents, ammo counts, spawn timers, missile
guidance, deploy cooldowns) — mismatches desync multiplayer. Rules (same discipline as
[[kratos-rng-desync-rootcause]] and the sibling docs):

1. **Every random draw** (`Precision` jitter, any "varied" guidance) uses the **synced** game
   RNG, never the unsynced render/animation RNG.
2. **No per-frame float divergence**: fixed-point / integer timers for delays and reload.
3. **Serialize** all new per-object state (bay contents beyond vanilla list, active-index,
   cooldown clocks, pending off-map arrivals) via the Phobos save stream, or accept mid-game
   save/load resets and document it (AITriggerTypeExt initially chose the latter — see
   [[aitriggertypeext-project]]).
4. Input gestures (double-tap, alt-key) resolve to a **queued command** processed in the synced
   command phase, not applied directly on keypress.

---

## 7. Coexistence — layer on, never fork

The biggest risk (as with [[buildqueueext-project]]) is collision, not the features. Antares and
Phobos actively own adjacent code; PayloadExt must **contain** their slots, not reimplement firing.
Antares being open source ([[antares-replaces-ares]]) is a real advantage here — we can read its
Gunner/Spawner/Ammo handling and hook precisely around it instead of guessing:

- **Antares** owns: `OpenTopped.*` bonuses, `Gunner`/IFV weapon swap, `Spawns`/spawner regen,
  `Ammo`, `DeploysInto`/`IsSimpleDeployer`/`DeployFire`, `AircraftDrop`. → A Bay *wraps* these
  keys; a Gunner bay *chooses* which weapon Antares' IFV logic points at; keep Antares key spellings.
- **Phobos** owns: `OpenTopped.ShareTransportTarget`/`RangeBonus`/`DamageMultiplier`,
  trajectories (missile homing), deploy-to-transform, Spawner extensions, LaserTrails. → Fire-out
  profiles reuse Phobos key names; Launched Object guidance sits on Phobos trajectories.
- **Rule:** chain-after / layer-on. Before hooking any address, check the Encyclopedia's
  `conflicts.md` ([[yr-hook-encyclopedia]]) for who already owns it.

---

## 8. Hook candidates (all ⚠ until Ghidra / Encyclopedia-verified)

Address-key everything against the Encyclopedia before writing. Candidate surfaces:

- ⚠ **Open-topped fire-out** path (passenger fires from transport) — for §2.2 index + building
  open-topped. Cross-ref Phobos OpenTopped hooks.
- ⚠ **Passenger add/remove / capacity** (`FootClass`/transport load) — for multiple Cargo bays.
- ⚠ **Unload / deploy-unload** sequence — for §3.1 release schedule.
- ⚠ **Deploy command dispatch** + keyboard command table — for gestures + AltDeploy (§3.4).
- ⚠ **Spawner update** (`SpawnManagerClass`) — for multiple spawner bays + switching.
- ⚠ **Ammo/reload update** — for ammo bays.
- ⚠ **IFV/Gunner weapon selection** — for gunner bays (layer on Antares).
- ⚠ **Projectile/BulletClass update + homing** — for Launched Object guidance (layer on Phobos).
- ⚠ **SuperWeapon activate** + **laser-designator/target-paint** — for off-map origin.
- ⚠ **AA target acquisition** — for targetable off-map missiles.

Vanilla `gamemd.exe` for RE work: `/home/rex/gamemd.exe`. Verified addresses from siblings live
in [[aitrigger-lifecycle-hook-addresses]]; write a `encyclopedia/Ext-Payload.md` Tier-2 page as
each surface is dived.

---

## 9. Build / repo layout (mirror sibling projects)

```
PayloadExt/
  DESIGN.md
  src/                 ; Ext containers (Techno/Building/Bullet/House), bay engine,
                       ;   gesture dispatcher, launched-object guidance, off-map delivery
  .github/workflows/   ; CI-only Windows MSVC build (Syringe), like TechnoAttachmentExt
```

Same Syringe/Phobos toolchain as the family: Container/ExtData + `DEFINE_HOOK`/`GET`,
`return 0` = fall through; CI via GitHub Actions/MSBuild driven with `gh`; DLL copied into
`/home/rex/snap/cncra2yr/common/.wine/drive_c/Westwood/RA2/` after each green build.

---

## 10. Phasing

- **P1 — OpenTopped index + veterancy** (§2.2, incl. building open-topped). Smallest, most
  isolated: extends the existing fire-out path with a lookup; no new container. Ship first.
- **P2 — Deploy gesture framework** (§3): input binding (AltDeploy), preconditions, fire-mode /
  bay-switch actions. Input + state machine, isolated from the container work.
- **P3 — Multiple Bays** (§2): the container abstraction over Cargo/Ammo/Spawner/Gunner. The big
  engine change and the main sync surface — index 0 stays vanilla for compatibility.
- **P4 — Launched Object guidance** (§4): tracking spawn-missiles, layered on Phobos trajectories.
- **P5 — Off-map origin** (§5): SW / designator launches, targetable missiles, off-map airstrike.
  Largest unknowns (SW + AA-targeting + new command); do last.

---

## 11. Relationship to the family

Part of the [[aitriggertypeext-project]] family of YR modding DLLs; same
one-primitive-many-unhardcodings ethos as [[traitext-project]], [[spawnext-project]],
[[buildqueueext-project]], [[prerequisiteext-project]]. Reuses the
[[yr-hook-encyclopedia]] for address-keyed conflict checks and the
[[kratos-rng-desync-rootcause]] sync discipline. Coexists with TechnoAttachmentExt's
map-mode container pattern ([[technoattachmentext-project]]).

---

## 12. Open decisions

1. **Veterancy source for OpenTopped index** — transport's rank (default) vs firing passenger's
   rank; `Open.VeterancySource=` covers both — confirm the default reading.
2. **Multiple-cargo vs vanilla Passengers list** — does bay 0 remain the literal vanilla list, or
   do all holds move into PayloadExt containers? (Lean: bay 0 = vanilla, extra bays = ours, for
   drop-in compatibility.)
3. **AltDeploy input** — real rebindable keyboard command vs modifier+deploy (§3.4).
4. **Targetable off-map missile** — real Techno/aircraft body vs projectile+interception hook
   (§5.3).
5. **Gunner bays vs Antares IFV** — require Antares, or reimplement the weapon swap if Antares
   absent? (Antares being open source makes wrapping it the clear default.)
6. **Save/load** — serialize all new bay/guidance/off-map state now, or accept resets first (as
   AITriggerTypeExt did) and serialize in v2?
7. **Time units** — frames everywhere (pick one; sibling docs flagged the same).
