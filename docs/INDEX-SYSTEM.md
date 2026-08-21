# The Index System — organizing the visual/profile tags

Rex dumped a large tag list (turret indices, `Image.*` overrides per
veterancy/observer, `Transparent.*.Level`, plus the existing OpenTopped index and
Veteran/Elite tags) and asked for it to be **organized into one flexible index
system**. This is that organization.

---

## 1. The observation

Every one of those asks has the same three-part shape:

> **Selector** (what state we key on) → **Index** (a number) → **Profile**
> (a named block of overrides to apply)

| Feature | Selector | Profile contents |
|---|---|---|
| OpenTopped index (DESIGN §2.2) | veterancy | range bonus, damage mult, FLH… |
| Turret index by weapon | firing weapon slot | which turret voxel |
| Turret index by range | distance to target | which turret voxel |
| `Image.Vet.Tur=` / `Image.Elite.Base=` | veterancy | which voxel sections |
| `Image.Owner=` / `.Ally=` / `.Enemy=` | **observer relationship** | whole image |
| `Transparent.Enemy.Level=` | observer relationship | translucency |
| `Image.Enemy.Index=EnemyImage` | explicit named profile | everything |

So there is **one primitive** here, not seven features:

> A **Profile** is a named set of overrides. A **Selector** resolves game state to
> a profile index. Each *domain* (turret, image, transparency, open-topped)
> consumes profiles of its own kind.

This is the same "one primitive, many unhardcodings" shape as the Bay / Deploy
gesture / Launched Object in DESIGN.md — it becomes **Primitive D: the Profile**.

## 2. The selector vocabulary (shared by every domain)

One vocabulary, reused everywhere, so learning it once covers all the tags:

| Selector | Values | Notes |
|---|---|---|
| `Veterancy` | `Rookie`/`Veteran`/`Elite` | already needed by OpenTopped index |
| `Observer` | `Owner`/`Ally`/`Team`/`Enemy` | **per-viewer**, see §4 — cosmetic only |
| `Weapon` | weapon slot index | reuses the engine's `+0x814` map (TURRETS.md) |
| `Range` | distance bands, in cells | ascending thresholds |
| `Explicit` | a named section | the `Image.Enemy.Index=EnemyImage` escape hatch |

## 3. Proposed unified spelling

Rex's flat tags stay valid as **shorthand**; the general form is the escape hatch
for anything more complex. Two rules keep it predictable:

- **Shorthand** = `<Domain>.<Selector><.Sub>=` — good for the common case.
- **General** = `<Domain>.Profile.<Selector>=<SectionName>` — a full profile block.

```ini
[HTNK]
; --- shorthand (Rex's original spellings, kept) ---
Image.Tur=HTNKTUR              ; none/inviso for no turret
Image.Base=HTNK
Image.Barl=HTNKBARL
Image.Elite.Tur=HTNKTUR_E
Image.Enemy=HTNK_SPOOKY
Transparent.Enemy.Level=100    ; 0 = opaque (default), 100 = invisible
Transparent.Ally.Level=25

; --- general form: hand the whole thing to a named profile ---
Image.Profile.Enemy=EnemyImage

[EnemyImage]
Image=HTNK_FAKE
Image.Elite.Tur=HTNKTUR_E
Transparent.Level=60
```

`Image.Enemy.Index=EnemyImage` from Rex's list becomes
`Image.Profile.Enemy=EnemyImage` — same capability, and it reads consistently
with every other domain instead of overloading the word "Index" for two different
things (a *number* for turrets, a *section name* for images).

### Naming decision to make (open)
`Index` currently means both "an integer slot" (turret/open-topped) and "a named
section" (`Image.Enemy.Index=`). Recommend reserving **`Index`** for integers and
**`Profile`** for named sections. Rex to confirm.

## 4. The hard constraint: cosmetic vs logical (sync safety)

This is the most important thing on this page.

The `Observer` selector (`Owner`/`Ally`/`Team`/`Enemy`) means **different machines
render the same unit differently** — that is *correct and required* for
"transparent to the enemy but not to me." But it means anything the Observer
selector touches **must be strictly cosmetic**:

- ✅ Safe: which voxel/SHP section is drawn, translucency level, tint, whether a
  section is drawn at all.
- ❌ **Never**: range, damage, ROF, targetability, armor, speed, weapon choice, or
  anything that feeds `CurrentTurretNumber` *when the turret index has gameplay
  meaning*.

Concretely: **`Transparent.*.Level` must not affect targetability.** "Physically
transparent or invisible at all times to both friendlies and enemies" is a
*rendering* property; if Rex also wants it untargetable, that is a **separate,
non-observer** tag applied identically on every machine (otherwise the game
desyncs the moment two players disagree about whether a unit can be shot).

This mirrors the classification rule already used in [[traitext-project]]: every
overridable key is tagged Cosmetic or Logical by the DLL, not by the modder.

**Therefore the domains split:**

| Domain | Selectors allowed | Class |
|---|---|---|
| Turret index | Veterancy, Weapon, Range | **Logical** (identical on all machines) |
| OpenTopped index | Veterancy | **Logical** |
| Image / voxel sections | Veterancy, Observer, Explicit | **Cosmetic** |
| Transparency | Veterancy, Observer, Explicit | **Cosmetic** |

Turret index is deliberately **not** observer-selectable: the turret can imply
which weapon is live, and players must agree on that.

## 5. Voxel section control (`Image.Tur/Base/Barl`)

Rex's per-section image tags map onto the engine's three voxel sections
(body / turret / barrel), which are loaded by
`ObjectTypeClass::Load3DArt_Turrets` (`0x5F865F`) and `_Barrels` (`0x5F887B`)
and drawn via `UnitClass::DrawVXL_Turrets` (`0x73BD15`) / `_Barrels1-3` — all
**Antares-owned** (see TURRETS.md).

`none`/`inviso` for a section = do not draw it. Phobos already has a related hook,
`TechnoClass_RenderVoxelObject_SkipInvisibleSections` (`0x706F64`), which is the
natural place to suppress a section — worth reusing rather than reinventing.

⚠ All of §5 is **design only** — no addresses re-verified for this purpose yet,
and the Phobos turret-draw rewrite conflict from TURRETS.md §3 applies here too.

## 6. Status

- ✅ **Implemented:** turret index by weapon + by range (TURRETS.md §2).
- 📋 **Designed, not built:** everything in §3–§5.
- ❓ **Needs Rex:** the `Index` vs `Profile` naming call (§3), and confirmation
  that invisible-to-enemy is cosmetic-only or needs a separate logical tag (§4).
