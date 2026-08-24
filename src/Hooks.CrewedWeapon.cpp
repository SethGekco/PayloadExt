// RA2-style occupy fire — the BUILDING owns the garrison weapon and infantry
// crew it. See docs/GARRISON.md.
//
// Yuri's Revenge replaced RA2's system: in YR a garrisoned building fires the
// OCCUPANT's InfantryTypeClass::OccupyWeapon. In RA2 the building itself carried
// the weapon (CABUNK01: Primary=AlliedOccupyW / Secondary=SovietOccupyW) and the
// infantry inside merely operated it, firing faster the more of them there were.
// Both are useful; this adds RA2's back WITHOUT removing YR's.
//
// The three engine facts this is built on (all objdump-verified):
//
//  1. BuildingClass::CanFire @0x447F10
//         mov cl,[Type+0x157B]   ; CanBeOccupied
//         je  0x447F45           ; not occupiable -> normal path
//         mov cl,[Type+0x157C]   ; CanOccupyFire
//         je  0x44805A           ; occupiable + CanOccupyFire=no -> CANNOT FIRE
//         call [vtable+0x408]    ; GetOccupantCount()
//         je  0x44805A           ; occupiable + empty        -> CANNOT FIRE
//     So CanOccupyFire=no does not merely disable YR's occupant firing, it stops
//     the building firing at all. RA2 mode has to re-open that gate itself.
//
//  2. BuildingClass::GetWeapon @0x4526F0
//         call [vtable+0x400]    ; CanOccupyFire()
//         je  0x4527B4           ; FALSE -> base TechnoClass::GetWeapon = OWN weapon
//         ...                    ; TRUE  -> the occupant's OccupyWeapon
//     With CanOccupyFire=no the engine already hands back the building's own
//     weapon, which is why a bare CABUNK01-style Primary=/Secondary= setup needs
//     no weapon hook at all — only the GarrisonWeapon[] list does.
//
//  3. TechnoClass::RearmDelay @0x6FD150 divides the delay by the occupant count,
//     but only when CanOccupyFire() is true — which it is not in RA2 mode, so we
//     supply that divisor ourselves.

#include <BuildingClass.h>
#include <InfantryClass.h>
#include <InfantryTypeClass.h>
#include <TechnoClass.h>

#include <Utilities/Macro.h>

#include <Ext/TechnoType/Body.h>

namespace
{
	// GetOccupantCount() is a TechnoClass virtual (vtable +0x408). It is R0 in
	// YRpp, but we call it *virtually*, so dispatch reaches the game's real
	// implementation (`mov eax,[this+0x694]; ret`). The R0 footgun only applies
	// to qualified, vtable-bypassing calls.
	int OccupantsOf(TechnoClass* pThis)
	{
		return pThis->GetOccupantCount();
	}

	TechnoTypeExt::ExtData* RA2GarrisonExt(TechnoClass* pThis)
	{
		const auto pExt = TechnoTypeExt::Fetch(pThis);
		return (pExt && pExt->UsesRA2Garrison()) ? pExt : nullptr;
	}
}

// ---------------------------------------------------------------------------
// 0. Let ordinary infantry crew an RA2-mode garrison.
//
// BuildingClass::CanBeOccupiedBy @0x457D48 — `mov eax,[edi+0x6C0]` (the
// infantry's Type), exactly 6 bytes (8B 87 C0 06 00 00). ESI = building,
// EDI = infantry. Vanilla then reads InfantryTypeClass+0xEB4 (`Occupier=`) and,
// if clear, diverts to the ASSAULT branch at 0x457DAD — so a non-Occupier type
// can never garrison. Vanilla only sets Occupier= on E1/E2/INIT, which would
// make RA2 mode unusable for every other infantry type.
//
// Returning 0x457D58 skips only the Occupier test and rejoins the normal
// occupier path (EAX is immediately overwritten there, so not setting it is
// safe). Antares' own hook at 0x457D58 still runs.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x457D48, BuildingClass_CanBeOccupiedBy_PayloadRA2Occupier, 0x6)
{
	enum { SkipOccupierCheck = 0x457D58 };

	GET(TechnoClass* const, pThis, ESI);
	GET(InfantryClass* const, pInfantry, EDI);

	if (!RA2GarrisonExt(pThis) || !pInfantry)
		return 0;

	const auto pInfExt = TechnoTypeExt::ExtMap.Find(pInfantry->Type);

	// Defaults to [General]->Occupier.RA2Mode.Default (yes), so RA2 mode works
	// out of the box; set Occupier.RA2Mode=no to keep a type out.
	if (pInfExt && pInfExt->AllowsRA2Occupy())
		return SkipOccupierCheck;

	return 0;
}

// ---------------------------------------------------------------------------
// 1. Re-open the fire gate.
//
// BuildingClass::CanFire @0x447F25 — the `mov cl,[eax+0x157C]` that reads
// CanOccupyFire; exactly 6 bytes (8A 88 7C 15 00 00). EAX = BuildingTypeClass*,
// ESI = the building. Unhooked by any framework (Antares' PrismForward is later
// in the same function at 0x447FAE, Phobos' OmniFire at 0x447FED).
//
// In RA2 mode we replace vanilla's two checks with one: the building may fire
// while it is crewed, regardless of CanOccupyFire.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x447F25, BuildingClass_CanFire_PayloadRA2Garrison, 0x6)
{
	enum { ContinueChecks = 0x447F45, CannotFire = 0x44805A };

	GET(TechnoClass* const, pThis, ESI);

	const auto pExt = RA2GarrisonExt(pThis);

	if (!pExt)
		return 0;

	// MinOccupants=0 lets the building fire while empty at its base rate.
	return OccupantsOf(pThis) >= pExt->Garrison_MinOccupants.Get()
		? ContinueChecks : CannotFire;
}

// ---------------------------------------------------------------------------
// 2. Pick the building's own garrison weapon.
//
// BuildingClass::GetWeapon @0x452738 — the `call [edx+0x400]` (CanOccupyFire());
// exactly 6 bytes (FF 92 00 04 00 00). ESI = the building, EBP = weapon index.
// 0x4527B4 is the "use the building's own weapon" branch; 0x4527BC is the
// balanced epilogue that returns EAX (prologue pushes ebx/ebp/esi/edi, and we
// push nothing).
//
// Returning nullptr from PickGarrisonWeapon falls through to 0x4527B4, i.e. the
// building's plain Primary=/Secondary= — which is precisely RA2's CABUNK01.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x452738, BuildingClass_GetWeapon_PayloadRA2Garrison, 0x6)
{
	enum { OwnWeapon = 0x4527B4, ReturnEAX = 0x4527BC };

	GET(BuildingClass* const, pThis, ESI);

	const auto pExt = RA2GarrisonExt(pThis);

	if (!pExt)
		return 0;

	// Which occupant is currently manning it — the same index vanilla uses to
	// pick the occupant's weapon and award XP.
	InfantryTypeClass* pOccupantType = nullptr;
	const int index = pThis->FiringOccupantIndex;

	if (index >= 0 && index < pThis->Occupants.Count)
	{
		if (const auto pOccupant = pThis->Occupants[index])
			pOccupantType = pOccupant->Type;
	}

	if (const auto pWeapon = pExt->PickGarrisonWeapon(pOccupantType))
	{
		R->EAX(pWeapon);
		return ReturnEAX;
	}

	return OwnWeapon;
}

// ---------------------------------------------------------------------------
// 3. "The more infantry inside, the faster it fires."
//
// TechnoClass::RearmDelay @0x6FD1B1 — the join point AFTER vanilla's whole
// occupy block and BEFORE the bunker block at 0x6FD1C7. Stolen bytes are exactly
// 8B 86 E4 02 00 00 (mov eax,[esi+0x2E4]); ESI = TechnoClass*. Unhooked by any
// framework — chosen so we never arbitrate Phobos's REDIRECTING hooks at
// 0x6FD183 / 0x6FD1C7.
//
// Vanilla's own divisor at 0x6FD17B is gated on CanOccupyFire(), which is false
// in RA2 mode, so this cannot double-apply.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x6FD1B1, TechnoClass_RearmDelay_PayloadRA2Garrison, 0x6)
{
	GET(TechnoClass* const, pThis, ESI);
	GET(int const, rof, EBP);

	const auto pExt = RA2GarrisonExt(pThis);

	if (!pExt || !pExt->Garrison_ROFPerOccupant.Get())
		return 0;

	// Per-entry ROFMultiplier for whoever is currently manning the weapon.
	double perEntry = 1.0;

	if (const auto pBuilding = abstract_cast<BuildingClass*>(pThis))
	{
		const int index = pBuilding->FiringOccupantIndex;

		if (index >= 0 && index < pBuilding->Occupants.Count)
		{
			if (const auto pOccupant = pBuilding->Occupants[index])
			{
				if (const auto pEntry = pExt->PickGarrisonEntry(pOccupant->Type))
					perEntry = pEntry->ROFMultiplier.Get();
			}
		}
	}

	int occupants = OccupantsOf(pThis);

	if (occupants <= 1 && perEntry == 1.0)
		return 0;

	if (occupants < 1)
		occupants = 1;

	const int cap = pExt->Garrison_ROFMaxOccupants.Get();
	if (cap > 0 && occupants > cap)
		occupants = cap;

	// Integer division, matching vanilla's idiv at 0x6FD17B — deterministic
	// across machines, no floating point in logical state.
	int scaled = static_cast<int>((rof / occupants) * perEntry);

	// A rearm delay of 0 means "fire every frame".
	if (scaled < 1)
		scaled = 1;

	R->EBP(scaled);
	R->Stack(0x14, scaled);

	return 0;
}
