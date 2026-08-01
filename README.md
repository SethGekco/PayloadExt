# PayloadExt

A standalone [Syringe](https://github.com/Ares-Developers/Syringe) DLL for
**Red Alert 2: Yuri's Revenge** that generalizes what a unit or building
**carries and releases** — open-topped cargo, ammo, spawners, gunner weapons —
and what it **launches** — tracking / off-map missiles and airstrikes.

Designed to *layer on top of* **Antares** (the open-source Ares reimplementation)
and **Phobos**, never to fork their single-slot systems. Built against the
Phobos [YRpp](https://github.com/Phobos-developers/YRpp) headers.

- **Design:** [`docs/DESIGN.md`](docs/DESIGN.md)
- **Current work:** Feature 1 — OpenTopped for `BuildingType` (and later
  `InfantryType`). Research + hook plan in
  [`docs/OPENTOPPED_RESEARCH.md`](docs/OPENTOPPED_RESEARCH.md).

## Build

Windows-only, x86, MSVC (`v142`, C++20). Built in CI
(`.github/workflows/build.yml`, `DevBuild|Win32`); the resulting
`PayloadExt.dll` is copied next to `gamemd.exe` alongside `Syringe.exe`,
`Ares`/`Antares`, and `Phobos`.

```
git submodule update --init --recursive   # YRpp + Phobos
msbuild PayloadExt.sln /p:Configuration=DevBuild /p:Platform=x86
```

## Status

Phase 0 — buildable skeleton: a single startup build-stamp hook (`0x6BD68D`)
that confirms Syringe injection and identifies the loaded build in `debug.log`.
No gameplay features wired yet.
