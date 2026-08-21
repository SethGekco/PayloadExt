// Crewed weapon — the RA2-style garrison: a building that has its OWN weapon,
// fires only while infantry are inside, and fires faster the more of them there
// are. See docs/GARRISON.md.
//
// Vanilla already implements "more occupants = faster" inside
// TechnoClass::RearmDelay, but only for the true occupy path:
//
//   0x6FD150  call [vtable+0x400]   ; TechnoClass::CanOccupyFire()
//   0x6FD15C  je   0x6FD1B1         ; not occupy-firing -> no bonus at all
//   0x6FD162  call [vtable+0x408]   ; TechnoClass::GetOccupantCount()
//   0x6FD16A  jle  0x6FD183         ; count <= 0 -> skip
//   0x6FD17B  idiv ecx              ; rof /= occupantCount
//   0x6FD183  ...                   ; then flat RulesClass::OccupyROFMultiplier
//
// A building firing its own Primary= does not take that path, so we reproduce
// the divisor ourselves — after vanilla's whole occupy/multiplier block, so we
// never double-apply and never fight Phobos's hooks at 0x6FD183 / 0x6FD1C7.

#include <TechnoClass.h>
#include <TechnoTypeClass.h>

#include <Utilities/Macro.h>

#include <Ext/TechnoType/Body.h>

namespace
{
	// GetOccupantCount() is a TechnoClass virtual (vtable +0x408). It is
	// declared R0 in YRpp, but we call it *virtually* through the object, so
	// dispatch lands on the game's real implementation — the R0 footgun only
	// applies to qualified, vtable-bypassing calls. YRpp's own BuildingClass.h
	// calls it exactly this way.
	int OccupantsOf(TechnoClass* pThis)
	{
		return pThis->GetOccupantCount();
	}

	TechnoTypeExt::ExtData* CrewExt(TechnoClass* pThis)
	{
		const auto pType = pThis->GetTechnoType();
		const auto pExt = TechnoTypeExt::ExtMap.Find(pType);

		return (pExt && pExt->HasCrewLogic()) ? pExt : nullptr;
	}
}

// TechnoClass::CanFire — 0x6FC339. ESI = TechnoClass*.
// CannotFire = 0x6FCB7E (the same exit Phobos uses from this address).
// Antares, Ares, Kratos and Phobos all hook here; we only ever redirect for
// types that explicitly opted in, and otherwise return 0 so they all chain.
//
// This is the gate: "a building weapon that requires infantry inside".
DEFINE_HOOK(0x6FC339, TechnoClass_CanFire_PayloadCrewGate, 0x6)
{
	enum { CannotFire = 0x6FCB7E };

	GET(TechnoClass* const, pThis, ESI);

	const auto pExt = CrewExt(pThis);

	if (!pExt || !pExt->Crew_Required.Get())
		return 0;

	// Fewer bodies inside than required -> the weapon is simply unmanned.
	if (OccupantsOf(pThis) < pExt->Crew_MinOccupants.Get())
		return CannotFire;

	return 0;
}

// TechnoClass::RearmDelay — 0x6FD1B1, the join point AFTER vanilla's entire
// occupy-ROF block (both the occupant-count divisor and the flat
// OccupyROFMultiplier) and BEFORE the bunker block at 0x6FD1C7.
//
// Verified in a clean gamemd.exe: the 6 stolen bytes are exactly
//   8B 86 E4 02 00 00   mov eax, [esi+0x2E4]
// ESI = TechnoClass*. On every path reaching here EBP holds the current rearm
// delay, mirrored at [ESP+0x14] (vanilla writes `mov [esp+0x14], ebp` on each
// branch, and Phobos's bunker hook reads it from that stack slot). We therefore
// update BOTH so whoever reads next sees the same value.
//
// Not hooked by any framework in the registry — chosen deliberately to avoid
// arbitrating Phobos at 0x6FD183 / 0x6FD1C7.
DEFINE_HOOK(0x6FD1B1, TechnoClass_RearmDelay_PayloadCrewROF, 0x6)
{
	GET(TechnoClass* const, pThis, ESI);
	GET(int const, rof, EBP);

	const auto pExt = CrewExt(pThis);

	if (!pExt || !pExt->Crew_ROFPerOccupant.Get())
		return 0;

	int occupants = OccupantsOf(pThis);

	if (occupants <= 1)
		return 0;

	// Optional cap so a packed building isn't absurdly fast.
	const int cap = pExt->Crew_ROFMaxOccupants.Get();
	if (cap > 0 && occupants > cap)
		occupants = cap;

	// Integer division, matching vanilla's `idiv` at 0x6FD17B — keeps this
	// deterministic across machines (no floating point in logical state).
	int scaled = rof / occupants;

	// Never let the delay reach zero: a 0 rearm delay means "fire every frame".
	if (scaled < 1)
		scaled = 1;

	R->EBP(scaled);
	R->Stack(0x14, scaled);

	return 0;
}
