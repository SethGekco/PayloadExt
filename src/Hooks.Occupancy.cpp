// Who may garrison a building — the three-class permission matrix.
// See docs/GARRISON.md §Occupancy.
//
// Vanilla has exactly one gate: InfantryTypeClass::Occupier=, which ships set on
// only E1/E2/INIT. That is a global whitelist expressed on the INFANTRY, so a
// building cannot say anything about who it accepts. PayloadExt moves the
// decision to the BUILDING and splits it by occupant CLASS:
//
//   class 0  Vanilla     the stock garrison            Occupier=
//   class 1  RA2         building's own garrison gun   Occupier.RA2=
//   class 2  OpenTopped  occupants fire their own      Occupier.OpenTopped=
//
// and gives each building, per class:
//     Occupier<.Class>.Allow=   admit this class at all   (default CanBeOccupied=)
//     Occupier<.Class>.Force=   list, or "all"            (default none)
//     Occupier<.Class>.Deny=    list                      (default none)
//
// Admission = for ANY class: Allow && (own flag || Force) && !Deny.
// `Force=all` + `Deny=<list>` is the plain blacklist.
//
// ---------------------------------------------------------------------------
// WHY THIS HOOKS 0x457D58 AND WHY LOAD ORDER MATTERS
//
// Antares does not merely *add* a check at 0x457D58 — it REPLACES the whole
// occupier block:
//
//     if(pInf->Type->Occupier) { ...its own AllowedOccupiers list... }
//     return can_occupy ? 0x457DD5 : 0x457DA3;      // never returns 0
//
// It re-tests Occupier itself, so an earlier hook that merely skips the VANILLA
// Occupier test still loses. And because a non-zero return ends the Syringe
// chain, whoever is listed first in the -i= list wins a shared address. That is
// why PayloadExt.dll must appear BEFORE Antares.dll in wine-game.sh /
// ClientDefinitions.ini for this feature to work at all.
//
// We keep the takeover as small as possible: unless the building actually
// declares a policy we return 0 and Antares handles everything exactly as
// before, including its own CanBeOccupiedBy= whitelist, raidable-bunker rules
// and ownership logic.

#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <InfantryClass.h>
#include <InfantryTypeClass.h>

#include <Utilities/Macro.h>

#include <Ext/TechnoType/Body.h>

// ---------------------------------------------------------------------------
// Step 1 — get non-Occupier infantry as far as the decision point.
//
// BuildingClass::CanBeOccupiedBy @0x457D48 — `mov eax,[edi+0x6C0]` (the
// infantry's Type), exactly 6 bytes. ESI = building, EDI = infantry. Vanilla
// reads InfantryTypeClass+0xEB4 (Occupier=) next and, if clear, diverts to the
// ASSAULT branch at 0x457DAD — so a non-Occupier type never even reaches the
// occupier logic. Returning 0x457D58 skips only that test (EAX is overwritten
// immediately there, so leaving it unset is safe).
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x457D48, BuildingClass_CanBeOccupiedBy_PayloadReach, 0x6)
{
	enum { SkipVanillaOccupierTest = 0x457D58 };

	GET(BuildingClass* const, pBuilding, ESI);
	GET(InfantryClass* const, pInfantry, EDI);

	if (!pBuilding || !pInfantry || !pBuilding->Type)
		return 0;

	const auto pBldExt = TechnoTypeExt::ExtMap.Find(pBuilding->Type);

	// No declared policy -> leave the vanilla/Antares path completely alone.
	if (!pBldExt || !pBldExt->HasOccupancyPolicy())
		return 0;

	return SkipVanillaOccupierTest;
}

// ---------------------------------------------------------------------------
// Step 2 — the decision itself.
//
// BuildingClass::CanBeOccupiedBy @0x457D58 — `mov eax,[esi+0x21C]`, exactly 6
// bytes; the same address Antares replaces. ESI = building, EDI = infantry.
// 0x457DD5 = "yes" (mov al,1), 0x457DA3 = "no" (xor al,al).
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x457D58, BuildingClass_CanBeOccupiedBy_PayloadPolicy, 0x6)
{
	enum { CanOccupy = 0x457DD5, CannotOccupy = 0x457DA3 };

	GET(BuildingClass* const, pBuilding, ESI);
	GET(InfantryClass* const, pInfantry, EDI);

	if (!pBuilding || !pInfantry || !pBuilding->Type || !pInfantry->Type)
		return 0;

	const auto pBldExt = TechnoTypeExt::ExtMap.Find(pBuilding->Type);

	// Not ours -> Antares (or vanilla) decides, untouched.
	if (!pBldExt || !pBldExt->HasOccupancyPolicy())
		return 0;

	if (!TechnoTypeExt::AdmitsOccupant(pBuilding, pInfantry))
		return CannotOccupy;

	// We admit it. If the infantry is a normal Occupier the downstream code would
	// admit it too, so hand back to Antares and let it apply its own extras
	// (capacity, raidable bunkers, ownership, mind-control). Only a FORCED
	// non-Occupier has to bypass, because Antares would reject it outright.
	if (pInfantry->Type->Occupier)
		return 0;

	// Bypassing means we owe the guards Antares would have applied. Replicate
	// the two that actually matter for a forced occupant.
	if (pBuilding->GetOccupantCount() >= pBuilding->Type->MaxNumberOccupants)
		return CannotOccupy;

	if (pInfantry->IsMindControlled())
		return CannotOccupy;

	return CanOccupy;
}

// ===========================================================================
// The OTHER Occupier gates — the ones that short-circuit before
// BuildingClass::CanBeOccupiedBy ever runs.
//
// 2026-09-08: GGI/SNIPE showed the "enter" cursor on an RA2-mode building and
// the order was accepted, but they never walked. Cause: `Occupier` is tested in
// SEVERAL places, and most of them bail *before* calling CanBeOccupiedBy
// (0x457CE0), which is where our policy lives. Found them all by searching the
// binary for reads of InfantryTypeClass+0xEB4:
//
//     mov cl,[Type+0xEB5]   ; Assaulter
//     jne proceed
//     mov cl,[Type+0xEB4]   ; Occupier
//     je  bail              ; <-- neither -> never reaches CanBeOccupiedBy
//   proceed:
//     ...
//     call 0x457CE0         ; CanBeOccupiedBy
//
// So each hook below only has to get PAST the short-circuit; the real decision
// still happens in CanBeOccupiedBy, where BuildingClass_CanBeOccupiedBy_
// PayloadPolicy applies the full matrix. The exception is GarrisonBuilding,
// which never calls CanBeOccupiedBy, so it decides there.
//
// Verified offsets: Occupier = InfantryTypeClass+0xEB4, Assaulter = +0xEB5.
// ===========================================================================

namespace
{
	// Would our per-building policy admit this infantry? False for any building
	// that declares no policy, so untagged buildings behave exactly as vanilla.
	bool PolicyAdmits(InfantryClass* pInfantry, TechnoClass* pCandidate)
	{
		if (!pInfantry || !pCandidate)
			return false;

		const auto pBuilding = abstract_cast<BuildingClass*>(pCandidate);

		if (!pBuilding || !pBuilding->Type)
			return false;

		const auto pExt = TechnoTypeExt::ExtMap.Find(pBuilding->Type);

		if (!pExt || !pExt->HasOccupancyPolicy())
			return false;

		return TechnoTypeExt::AdmitsOccupant(pBuilding, pInfantry);
	}

	// Reads a techno pointer stored at a raw offset on the infantry. Both call
	// sites below load the candidate building from such a field immediately
	// after the branch we are replacing, so the offsets come straight from the
	// surrounding disassembly.
	TechnoClass* TechnoAt(InfantryClass* pInfantry, int offset)
	{
		return *reinterpret_cast<TechnoClass**>(
			reinterpret_cast<BYTE*>(pInfantry) + offset);
	}
}

// InfantryClass::ActionOnObject @0x51F489 — deciding what the ORDER does.
// `mov cl,[eax+0xEB4]` = 6 bytes. ESI = infantry, EAX = its Type.
// 0x51F49D loads the building from [ESI+0x2B4] and calls CanBeOccupiedBy.
// This is the gate that made the cursor offer "enter" while the order did
// nothing: the cursor comes from a different function that lacks this check.
DEFINE_HOOK(0x51F489, InfantryClass_ActionOnObject_PayloadOccupierGate, 0x6)
{
	enum { Proceed = 0x51F49D };

	GET(InfantryClass* const, pInfantry, ESI);

	return PolicyAdmits(pInfantry, TechnoAt(pInfantry, 0x2B4)) ? Proceed : 0;
}

// InfantryClass::UpdatePosition @0x519698 — the arrival step.
// `mov cl,[eax+0xEB4]` = 6 bytes. ESI = infantry, EAX = its Type.
// 0x5196A6 loads the building from [ESI+0x5A4], confirms WhatAmI() == Building
// (cmp eax,6) and then calls CanBeOccupiedBy.
DEFINE_HOOK(0x519698, InfantryClass_UpdatePosition_PayloadOccupierGate, 0x6)
{
	enum { Proceed = 0x5196A6 };

	GET(InfantryClass* const, pInfantry, ESI);

	return PolicyAdmits(pInfantry, TechnoAt(pInfantry, 0x5A4)) ? Proceed : 0;
}

// InfantryClass::GarrisonBuilding @0x522920 — the actual entry.
// `cmp bl,[eax+0xEB4]` = 6 bytes; BL is 0 from the prologue's `xor ebx,ebx`.
// ESI = infantry. The building is the first stack argument: the prologue is
// `sub esp,0xC / push ebx / push esi / push edi`, so at this point it sits at
// [ESP+0x1C] (0xC + 3 pushes + return address). Confirmed by 0x522937 reading
// it at [ESP+0x20] after one further `push ebp`.
//
// Unlike the two above, this function never calls CanBeOccupiedBy, so the
// policy decision is made here.
DEFINE_HOOK(0x522920, InfantryClass_GarrisonBuilding_PayloadOccupierGate, 0x6)
{
	enum { Proceed = 0x52292C };

	GET(InfantryClass* const, pInfantry, ESI);
	GET_STACK(TechnoClass* const, pBuilding, 0x1C);

	return PolicyAdmits(pInfantry, pBuilding) ? Proceed : 0;
}
