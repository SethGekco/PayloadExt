# PayloadExt — in-game test plan (build de3f6bc)

Everything below is **already applied** to the live game files. All rules edits
are marked `;PLXTEST`.

```bash
# see every change
grep -n PLXTEST ~/snap/cncra2yr/common/.wine/drive_c/Westwood/RA2/rulesmd.ini
# full revert
cp rulesmd.ini.payloadext-bak-20260821-221632 rulesmd.ini
```

| File | Change | Backup |
|---|---|---|
| `PayloadExt.dll` | deployed, 181,248 bytes | — (first deploy) |
| `Resources/Compatibility/Unix/wine-game.sh` | `+ -i=PayloadExt.dll` | `.payloadext-bak` |
| `Resources/ClientDefinitions.ini` | `+ -i=PayloadExt.dll` | `.payloadext-bak` |
| `rulesmd.ini` | tests A–D | `.payloadext-bak-20260821-221632` |

> `wine-game.sh` is the one that actually matters on Linux — it is what
> `UnixGameExecutableName` points at. `ClientDefinitions.ini` was updated too so
> the two stay consistent.

---

## TEST 0 — the DLL loads at all (do this first)

**Do:** launch the game, start any skirmish, quit.

**Look for** in `debug.log` (game folder, or `Debug/`):
```
[PayloadExt] Module base: 0x........
[PayloadExt] Build: Aug 21 2026 ...
```

- ✅ Both lines → Syringe injected it and the launcher edits worked.
- ❌ No lines → it is not being loaded. Check `syringe.log` for `PayloadExt.dll`.
- ❌ Game won't start → remove `-i=PayloadExt.dll` from `wine-game.sh` and report.

**Also confirm the "does nothing by default" promise:** everything is opt-in, so
apart from the four tagged sections the game should play *exactly* as before.

---

## TEST A — turret index follows **distance to target** (Prism Tank)

Rex's "SREF charge turrets, but driven by distance."

Applied to `[SREF]`: `IsChargeTurret=no` (this is required — the engine owns
`CurrentTurretNumber` on charge-turret types and PayloadExt deliberately skips
them), plus:
```ini
Turret.RangeBands=3,6
Turret.RangeIndices=0,1,3
```

**Do:** build a Prism Tank. Attack a stationary target (a wall or building) from
**far away (>6 cells)**, then order it closer so it fires from **3–6 cells**, then
**closer than 3 cells**.

**Look for:** the turret *voxel visibly changes shape* at each band —
turret 3 far → turret 1 mid → turret 0 close. SREF ships 4 turret voxels, so the
art already exists; they are visually distinct (they are the charge-up stages).

- ✅ Turret changes at the thresholds → range selector works.
- ⚠️ Turret never changes → selector not firing. Note whether the tank still
  shoots normally.
- ⚠️ Prism Tank no longer charges up visually → **expected**, that is
  `IsChargeTurret=no` doing its job, not a bug.

> The Prism Tank is also still carrying an **IntelExt** test tag
> (`Prerequisite.StolenTechs=130`) — it stays greyed out until you spy the Soviet
> Battle Lab. Either spy one, or temporarily comment those two lines out.

## TEST B — turret index follows **which weapon fires**

**Only after Test A.** Range wins while a target exists, so they must not both be
active.

**Do:** in `[SREF]`, comment out `Turret.RangeBands` and `Turret.RangeIndices`,
uncomment `Turret.FollowWeapon=yes`. SREF uses Antares' multi-weapon form
(`WeaponCount=1` / `Weapon1=Comet`), so to see a switch you also need a second
weapon and a turret mapping:
```ini
WeaponCount=2
Weapon2=<some AA weapon, e.g. a copy of an AA gun>
WeaponTurretIndex1=0     ; Antares spelling, unchanged
WeaponTurretIndex2=2
```
Then attack a **ground** target vs an **air** target.

**Look for:** turret voxel 0 when firing weapon 1, voxel 2 when firing weapon 2.

This is the gap the DLL closes: Antares already stores that mapping but only
applies it on the gunner/IFV path, so a normal vehicle never switched turrets.

---

## TESTS C & D — RA2 mode vs YR mode, side by side ★ main event
### (rewritten 2026-08-22 for the RA2Mode build — supersedes the old Crew.* text below)

The point of the pair is that **both systems now exist and are chosen per
building**.

**`[GAPILL]` Allied Pill Box = RA2 mode** (`CanOccupyFire=no` +
`CanOccupyFire.RA2Mode=yes`): the *building* owns the gun, infantry crew it.
```ini
GarrisonWeapon[0]=PXCrewGun        ; catch-all
GarrisonWeapon[0].Exclude=E1       ; ...but not GI
GarrisonWeapon[1]=PXCrewGunHeavy   ; GI only  (loud cannon, easy to hear)
GarrisonWeapon[1].Infantry=E1
GarrisonWeapon.ROFPerOccupant=yes
```

**`[NABNKR]` Soviet Battle Bunker = YR mode** (untouched, `CanOccupyFire=yes`):
fires the *occupant's* weapon. It carries a `Primary=` purely to demonstrate that
YR mode ignores it.

**Do, in order:**

1. Build a Pill Box, leave it **empty**, put a target in range.
   - **Look for:** it does not fire. (Vanilla would refuse anyway with
     `CanOccupyFire=no`; RA2 mode re-opens that gate only when crewed.)
2. Put **one Conscript** (or any non-GI infantry) inside.
   - **Look for:** it fires `PXCrewGun` — the light machine-gun report, ~1 shot
     per 2.5 s. This is the headline result: **a building firing its own weapon
     because infantry are inside it.**
3. Now use a **GI (E1)** instead.
   - **Look for:** a visibly/audibly different shot — `PXCrewGunHeavy`, a loud
     cannon with a shell projectile. That proves `GarrisonWeapon[N].Infantry` /
     `.Exclude` selection works.
4. Fill it to **4** occupants.
   - **Look for:** the fire rate climbing ~2.5 s → ~0.63 s. Count shots per 10 s
     (1 crew ≈ 4, 4 crew ≈ 16) rather than eyeballing.
5. Empty it again → it stops firing.
6. **Now the Battle Bunker.** Garrison it and watch.
   - **Look for:** it fires the *infantry's* weapon (ordinary GI/Conscript sounds),
     **not** `PXCrewGun`, and still speeds up with more men. That is YR mode
     working untouched, next to RA2 mode.

**If the pillbox still will not fire at all**, the RA2Mode gate at `0x447F25` is
not engaging — send `debug.log` and say which infantry you used.

---

## OLD TEST C text (superseded — the `Crew.*` tags no longer exist)

Applied to `[GAPILL]` (Allied Pill Box): it now uses a deliberately slow gun
(`PXCrewGun`, ROF=150 ≈ 2.5 s) and:
```ini
CanBeOccupied=yes / MaxNumberOccupants=4 / CanOccupyFire=no
Crew.Required=yes / Crew.MinOccupants=1
Crew.ROFPerOccupant=yes / Crew.ROFMaxOccupants=4
```
`CanOccupyFire=no` deliberately keeps vanilla's occupy-fire path out of the way,
so anything you see is PayloadExt's own gate and divisor.

**Do, in order:**

1. Build a Pill Box. Send an enemy unit (or use a skirmish opponent) into range
   while the pillbox is **empty**.
   - **Look for:** it does **not shoot at all**. ← the `Crew.Required` gate.
   - ❌ If it shoots while empty, the CanFire gate is not working.
2. Walk **one** infantry into it (right-click the pillbox with infantry selected).
   - **Look for:** it now fires, slowly — roughly **one shot every 2.5 s**.
   - Entry is gated by `CanBeOccupied` and `MaxNumberOccupants` only — **not** by
     `CanOccupyFire` (verified, see below), so `CanOccupyFire=no` does not block
     infantry from entering.
3. Add a **second**, then **third**, then **fourth** infantry.
   - **Look for:** fire rate visibly speeds up each time —
     ~2.5 s → ~1.25 s → ~0.83 s → ~0.63 s.
   - ❌ Rate never changes → the `0x6FD1B1` divisor hook isn't taking effect.
4. Kill/remove the occupants (sell or let them die).
   - **Look for:** it stops firing again once empty.

**Timing tip:** count shots over 10 seconds rather than eyeballing the gap —
1 crew ≈ 4 shots, 4 crew ≈ 16 shots.

## TEST D — control: does **vanilla** already do this? (no DLL tags)

Applied to `[NABNKR]` (Soviet Battle Bunker): it keeps its vanilla
`CanOccupyFire=yes` and simply gains `Primary=PXCrewGun`. **No `Crew.*` tags.**

**Do:** build a Battle Bunker, garrison it with 1 → 4 infantry, same observation
as Test C.

**Look for:**
- If it fires `PXCrewGun` **and** speeds up with more men → the vanilla occupy
  path already does this, and `Crew.ROFPerOccupant` is redundant for buildings
  configured this way. That is a genuinely useful negative result.
- If it fires the infantry's own `OccupyWeapon` instead (normal garrison sounds,
  not the pillbox report) → vanilla ignores the building's own weapon, which is
  exactly why the DLL feature is needed.

This is the cheap experiment I flagged earlier; **Test D is arguably the most
valuable test here**, because a positive result would simplify the whole design.

---

## What to send back

For each test: ✅/❌, plus anything odd. Most useful of all:
- `debug.log` and `syringe.log` after a session (especially any crash address).
- For Test C, the shot-count-per-10s at 1 / 2 / 3 / 4 occupants.

**Known unrelated noise:** Antares + Phobos are both loaded, and there is a
recorded EBolt crash where every Tesla shot can crash (both hook `0x4C1F33`).
`syringe.log` already shows a `0xC0000005` from a previous session. If you hit a
crash, check whether it involves Tesla weapons before blaming PayloadExt — and
either way send the address.

## Reverting everything

```bash
cd ~/snap/cncra2yr/common/.wine/drive_c/Westwood/RA2
cp rulesmd.ini.payloadext-bak-20260821-221632 rulesmd.ini
cp Resources/Compatibility/Unix/wine-game.sh.payloadext-bak \
   Resources/Compatibility/Unix/wine-game.sh
cp Resources/ClientDefinitions.ini.payloadext-bak Resources/ClientDefinitions.ini
rm PayloadExt.dll
```
