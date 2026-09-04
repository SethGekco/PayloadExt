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
#include <Utilities/Debug.h>

#include <cstdio>
#include <set>
#include <utility>

#include <Ext/TechnoType/Body.h>

namespace
{
	// ---- TEMPORARY DIAGNOSTIC: "turret invisible until the unit first fires" --
	//
	// Established so far: for [SREF] all four ChargerTurrets slots ARE loaded
	// (read out of a crash dump: VXL+HVA non-null for 0..3), while the vanilla
	// TurretVoxel slot is NULL. So the art is not missing.
	//
	// Phobos picks the voxel with:
	//     if (TurretCount == 0 || IsGattling || idx < 0) return &TurretVoxel;
	//     if (idx < 18) return &ChargerTurrets[idx];
	//   ... if (!(tur && tur->VXL && tur->HVA)) return SkipDrawing;
	//
	// so a NEGATIVE CurrentTurretNumber lands on the null TurretVoxel and the
	// turret is silently not drawn -- which matches "invisible" exactly. This
	// logs what the index actually is, and which slots are populated, so we can
	// tell "index went negative" from "index is fine, art is not drawn".
	//
	// ChargerTurrets is not declared in YRpp: it is VoxelStruct[18] at type+0xC8,
	// each entry {VXL*, HVA*} (derived from the vanilla loader at 0x5F7A90,
	// which stores to [type + esi*8 + 0xC8] / [+0xCC]).
	constexpr int ChargerTurretsOffset = 0xC8;

	void DiagTurretSlots(TechnoTypeClass* pType)
	{
		static std::set<TechnoTypeClass*> reported;
		if (!reported.insert(pType).second)
			return;

		auto const base = reinterpret_cast<DWORD*>(
			reinterpret_cast<BYTE*>(pType) + ChargerTurretsOffset);

		char slots[128];
		int n = 0;
		for (int i = 0; i < pType->TurretCount && i < 18; ++i)
			n += _snprintf_s(slots + n, sizeof(slots) - n, _TRUNCATE,
				"%s%d=%s", i ? " " : "", i,
				(base[i * 2] && base[i * 2 + 1]) ? "ok" : "EMPTY");

		auto const pMain = reinterpret_cast<DWORD*>(
			reinterpret_cast<BYTE*>(pType) + 0xB8); // vanilla TurretVoxel
		Debug::Log("[PayloadExt-diag] %s: TurretCount=%d TurretVoxel=%s slots[%s]\n",
			pType->ID, pType->TurretCount,
			(pMain[0] && pMain[1]) ? "ok" : "NULL", slots);
	}

	// Report each distinct index this type is given, plus the "we left it alone"
	// case -- that one carries the engine's own value, which is the number we
	// actually need to see.
	void DiagTurretIndex(TechnoClass* pThis, TechnoTypeClass* pType, int resolved)
	{
		static std::set<std::pair<TechnoTypeClass*, int>> reported;
		int const current = pThis->CurrentTurretNumber;
		int const key = (resolved >= 0) ? resolved : (-1000 - current);
		if (!reported.emplace(pType, key).second)
			return;

		if (resolved >= 0)
			Debug::Log("[PayloadExt-diag] %s: applying turret %d (was %d)\n",
				pType->ID, resolved, current);
		else
			Debug::Log("[PayloadExt-diag] %s: no selector result (no target); "
				"engine left CurrentTurretNumber=%d%s\n",
				pType->ID, current,
				current < 0 ? "   <<< NEGATIVE: Phobos draws the null TurretVoxel"
							  " and skips the turret" : "");
	}
	// ---- end diagnostic ------------------------------------------------------

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

		DiagTurretSlots(pType);

		const int index = pTypeExt->ResolveTurretIndex(pThis, weaponIndex);

		DiagTurretIndex(pThis, pType, index);

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

// The per-frame selector must run AFTER the engine's charge-turret block, not
// at the top of TechnoClass::AI.
//
// This used to hook the function entry (0x6F9E50). That looked right and was
// wrong: the charge-turret code at 0x6FA540-0x6FA5B8 lives INSIDE the same
// function (TechnoClass::AI runs 0x6F9E50-0x6FAF81), and it ends with
// `mov [esi+0x124], edi` — an unconditional store to CurrentTurretNumber. So
// our value was overwritten later in the very same tick, every frame.
//
// Crucially that block is NOT gated on IsChargeTurret. Its only guards are:
//     0x6FA51F  call 0x717880   -> returns pType->TurretCount > 0
//     0x6FA536  test [type+0xCD5] -> IsGattling, skip if set
// so every multi-turret non-gattling unit reaches it, and when
// ChargeTurretDelay <= 0 (i.e. it has not fired yet) the store writes 0.
// That is the whole bug: a Prism Tank showed turret 0 until it first fired,
// because only the FireAt hook below could get a value in edgewise.
//
// 0x6FA5BE is the convergence point immediately after that store — both guard
// branches (`je`/`jne` at 0x6FA526/0x6FA53E) jump here too, and it is also the
// address Phobos's own replacement of the block returns to, so this runs
// whether or not Phobos owns 0x6FA540. Stolen bytes: `lea edi,[esi+0x350]`
// (6 bytes, resumes cleanly at 0x6FA5C4). ESI = TechnoClass* on every path in.
DEFINE_HOOK(0x6FA5BE, TechnoClass_AI_AfterChargeTurret_PayloadTurretIndex, 0x6)
{
	GET(TechnoClass* const, pThis, ESI);

	// -1 = "not firing"; the range selector does not need a weapon index.
	ApplyTurretIndex(pThis, -1);

	return 0;
}
