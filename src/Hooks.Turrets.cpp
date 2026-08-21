// Turret index selectors — see docs/TURRETS.md.
//
// The engine picks which turret voxel to draw from TechnoClass::CurrentTurretNumber.
// Vanilla only ever varies it for gattling stages, charge-turret animation
// frames (the SREF pattern) and the IFV gunner. Antares adds a per-weapon turret
// index mapping (WeaponTurretIndex<N>= / <Name>TurretIndex=) but only applies it
// inside TechnoClass::SwitchGunner (0x70DC70), which the engine calls on the
// GUNNER path — so an ordinary Primary/Secondary vehicle never changes turret
// when it switches weapons. These two hooks close that gap and add a
// range-driven selector on top.

#include <TechnoClass.h>
#include <TechnoTypeClass.h>

#include <Utilities/Macro.h>

#include <Ext/TechnoType/Body.h>

namespace
{
	// Applies a resolved turret index, with every safety guard in one place.
	void ApplyTurretIndex(TechnoClass* pThis, int weaponIndex)
	{
		const auto pType = pThis->GetTechnoType();

		// Multi-turret types only.
		if (pType->TurretCount <= 0)
			return;

		// Never fight the engine where it owns CurrentTurretNumber.
		if (TechnoTypeExt::EngineOwnsTurretNumber(pType))
			return;

		const auto pTypeExt = TechnoTypeExt::ExtMap.Find(pType);

		// Opt-in only: untagged types are left exactly as vanilla.
		if (!pTypeExt || !pTypeExt->HasTurretSelector())
			return;

		const int index = pTypeExt->ResolveTurretIndex(pThis, weaponIndex);

		// Out-of-range indices would index past the ChargerTurrets array during
		// drawing, so clamp to the declared turret count instead of trusting INI.
		if (index >= 0 && index < pType->TurretCount)
			pThis->CurrentTurretNumber = index;
	}
}

// TechnoClass::FireAt — 0x6FDDC0. Verified instruction boundary in a clean
// gamemd.exe: `mov al, [ebx+0x144]` is exactly the 6 stolen bytes.
// ESI = TechnoClass*, EBX = WeaponTypeClass*, [EBP+0xC] = weapon index.
// Phobos also hooks this address (TechnoClass_FireAt_BeforeTruelyFire); we
// always return 0 so both hooks chain and Phobos keeps its own control flow.
// This is the "turret follows the weapon being fired" path.
DEFINE_HOOK(0x6FDDC0, TechnoClass_FireAt_PayloadTurretIndex, 0x6)
{
	GET(TechnoClass* const, pThis, ESI);
	GET_BASE(const int, weaponIndex, 0xC);

	ApplyTurretIndex(pThis, weaponIndex);

	return 0;
}

// TechnoClass::Update — 0x6F9E50. Verified function start in a clean
// gamemd.exe: `sub esp,0x68 / push ebx / push ebp` is exactly the 5 stolen
// bytes, and ECX holds the TechnoClass* (`mov esi, ecx` follows).
// Antares, Ares, Kratos and Phobos all hook this same entry point already —
// it is the canonical benign shared call site, and we return 0 like they do.
// This drives the range-based selector, which must track a moving target
// continuously rather than only updating when the unit fires.
DEFINE_HOOK(0x6F9E50, TechnoClass_Update_PayloadTurretIndex, 0x5)
{
	GET(TechnoClass* const, pThis, ECX);

	// -1 = "not firing"; the range selector does not need a weapon index.
	ApplyTurretIndex(pThis, -1);

	return 0;
}
