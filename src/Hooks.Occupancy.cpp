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
#include <Utilities/Debug.h>

#include <set>
#include <utility>

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
	bool PolicyAdmits(InfantryClass* pInfantry, TechnoClass* pCandidate,
		const char* pGate)
	{
		if (!pInfantry || !pInfantry->Type)
			return false;

		const auto pBuilding = abstract_cast<BuildingClass*>(pCandidate);
		const auto pExt = (pBuilding && pBuilding->Type)
			? TechnoTypeExt::ExtMap.Find(pBuilding->Type) : nullptr;
		const bool hasPolicy = pExt && pExt->HasOccupancyPolicy();
		const bool admits = hasPolicy
			&& TechnoTypeExt::AdmitsOccupant(pBuilding, pInfantry);

		// TEMPORARY DIAGNOSTIC: one line per (gate, infantry, building) so a
		// single run shows, for every type Rex tries, whether the gate is even
		// reached, whether the building's policy was seen, what the type's
		// vanilla Occupier flag actually is, and the final verdict. Guessing at
		// INI defaults from the disassembly was not converging.
		static std::set<std::pair<const void*, const void*>> reported;
		const void* key2 = pBuilding ? (const void*)pBuilding->Type : nullptr;
		if (reported.emplace((const void*)pGate, key2).second
			|| reported.emplace((const void*)pInfantry->Type, key2).second)
		{
			Debug::Log("[PayloadExt-diag] gate %s: %s -> %s "
				"(Occupier=%d hasPolicy=%d admits=%d)\n",
				pGate, pInfantry->Type->ID,
				(pBuilding && pBuilding->Type) ? pBuilding->Type->ID : "<not-a-building>",
				(int)pInfantry->Type->Occupier, (int)hasPolicy, (int)admits);
		}

		return admits;
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

	return PolicyAdmits(pInfantry, TechnoAt(pInfantry, 0x2B4), "ActionOnObject") ? Proceed : 0;
}

// InfantryClass::UpdatePosition @0x519698 — the arrival step.
// `mov cl,[eax+0xEB4]` = 6 bytes. ESI = infantry, EAX = its Type.
// 0x5196A6 loads the building from [ESI+0x5A4], confirms WhatAmI() == Building
// (cmp eax,6) and then calls CanBeOccupiedBy.
DEFINE_HOOK(0x519698, InfantryClass_UpdatePosition_PayloadOccupierGate, 0x6)
{
	enum { Proceed = 0x5196A6 };

	GET(InfantryClass* const, pInfantry, ESI);

	return PolicyAdmits(pInfantry, TechnoAt(pInfantry, 0x5A4), "UpdatePosition") ? Proceed : 0;
}

// ---------------------------------------------------------------------------
// FootClass::Mission_Capture @0x4D4B96 — THE ONE THAT MADE THEM STAND STILL.
//
// 2026-09-14: the admission diagnostic proved our matrix was never the problem:
//     gate ActionOnObject:   GGI -> NABNKR (Occupier=0 hasPolicy=1 admits=1)
//     gate UpdatePosition:   (no GGI line)
//     gate GarrisonBuilding: (no GGI line)
// We said YES, the order was accepted, and the unit then never arrived — because
// it never set off. Garrisoning is driven by Mission::Capture (=8): ActionOnObject
// force-missions Capture and Mission_Capture does the actual walking.
//
// Identified via the InfantryClass vtable at 0x7EB058 (assigned in the ctor at
// 0x517ACC/0x521A11): 0x4D4B20 is slot +0x214, and with MissionClass's declared
// virtual order anchored at +0x204 = Mission_Sleep that slot is Mission_Capture.
// Corroborated by InfantryClass::Mission_Hunt (slot +0x228, 0x51F540) ending in
// ForceMission(8) on a garrisonable building.
//
//   4d4b43  mov ecx,[esi+0x2B4]        ; the objective
//   4d4b4f  call [edx+0x2C]            ; WhatAmI() == 6 (Building)?
//   4d4b57  mov eax,[esi+0x5A4]        ; already have a Destination -> keep going
//   4d4b96  mov al,[edi+0xEB4]         ; Occupier   -> jne 4d4bb4   <-- HERE
//   4d4ba0  mov al,[edi+0xEB5]         ; Assaulter  -> jne 4d4bb4
//   4d4baa  mov al,[edi+0xEBE]         ; -> je 4d4bc7   BAIL
//   4d4bb4  SetDestination(target, 1)  ; THE WALK
//
// Bailing to 0x4D4BC7 skips SetDestination entirely, so the infantry keeps the
// Capture mission with a null Destination and simply stands there — precisely the
// reported symptom. Every other gate we had hooked sits DOWNSTREAM of this one,
// which is why none of them ever logged for GGI.
//
// Registers: the prologue is a branchless abstract_cast<InfantryClass*> —
//     mov edi,eax / sub edi,0xF / neg edi / sbb edi,edi / not edi / and edi,esi
// i.e. EDI = (WhatAmI()==Infantry) ? this : NULL, and 0x4D4B67 has already
// rejected NULL. But 0x4D4B86 REASSIGNS EDI to the Type, so at 0x4D4B96
// EDI = InfantryTypeClass* and the instance is ESI. Stolen bytes are the whole
// 6-byte mov.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x4D4B96, FootClass_MissionCapture_PayloadOccupierGate, 0x6)
{
	enum { SetDestinationAndWalk = 0x4D4BB4 };

	GET(InfantryClass* const, pInfantry, ESI);

	// [ESI+0x2B4] is the objective; 0x4D4B52 has already confirmed it is a
	// BuildingClass, and PolicyAdmits re-checks via abstract_cast regardless.
	return PolicyAdmits(pInfantry, TechnoAt(pInfantry, 0x2B4), "MissionCapture")
		? SetDestinationAndWalk : 0;
}

// ---------------------------------------------------------------------------
// InfantryClass::Mission_Hunt @0x51F576 — the same gate on the AI's path.
//
// Slot +0x228 of the InfantryClass vtable. Structurally the mirror of the above:
// if my objective is a building I may garrison, SetDestination + ForceMission(8)
// (Mission::Capture) — which then lands in Mission_Capture, hooked above. Without
// this, an AI house could never send a forced non-Occupier into a tagged
// building, so the two belong together.
//
// We proceed to 0x51F58A rather than the success label 0x51F59A so the engine
// still runs its CanBeOccupiedBy (0x457CE0) call, where the full matrix applies
// via BuildingClass_CanBeOccupiedBy_PayloadPolicy. (0x51F574 shows the engine
// itself skipping straight to 0x51F59A on Type+0xEBE, bypassing that check; we
// deliberately do not.)
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x51F576, InfantryClass_MissionHunt_PayloadOccupierGate, 0x6)
{
	enum { CheckCanBeOccupiedBy = 0x51F58A };

	GET(InfantryClass* const, pInfantry, ESI);

	return PolicyAdmits(pInfantry, TechnoAt(pInfantry, 0x2B4), "MissionHunt")
		? CheckCanBeOccupiedBy : 0;
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

	return PolicyAdmits(pInfantry, pBuilding, "GarrisonBuilding") ? Proceed : 0;
}
