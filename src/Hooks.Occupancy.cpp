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
#include <CellClass.h>
#include <HouseClass.h>
#include <InfantryClass.h>
#include <InfantryTypeClass.h>
#include <MapClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <Unsorted.h>

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include <Ext/TechnoType/Body.h>

namespace
{
	// TEMPORARY DIAGNOSTIC. One line per (infantry type, building type, outcome)
	// recording what CanBeOccupiedBy finally answered and why. This is the one
	// question none of the earlier logging could answer: our gates said "admits=1"
	// yet the unit still never entered, and the actual decision was silent.
	// Defined further down, beside the fix that consumes it: remembers which
	// building we just admitted this infantry into, so Mission_Capture can be
	// given the target the order failed to carry.
	void RememberAdmission(InfantryClass* pInfantry, BuildingClass* pBuilding);

	// Can this infantry still get into this building RIGHT NOW?
	//
	// 2026-09-23: AdmitsOccupant answers "is this type allowed in", which is NOT
	// the same question and in particular ignores CAPACITY. Using it as the gate
	// sent units to full buildings: the walk branch ran, SetDestination filled in
	// a Destination, and from then on 0x4D4B5F jumped past every refusal path, so
	// the unit stood at a full building holding it as Target -- and Target is the
	// field an attack reads, so it opened fire. Every decision about whether to
	// keep pursuing a garrison goes through here now.
	bool GarrisonStillPossible(BuildingClass* pBuilding, InfantryClass* pInfantry)
	{
		if (!pBuilding || !pBuilding->Type || !pInfantry || !pInfantry->Type)
			return false;

		const auto pExt = TechnoTypeExt::ExtMap.Find(pBuilding->Type);
		if (!pExt || !pExt->HasOccupancyPolicy())
			return false;

		if (!TechnoTypeExt::AdmitsOccupant(pBuilding, pInfantry))
			return false;

		// The one that was missing.
		if (pBuilding->GetOccupantCount() >= pBuilding->Type->MaxNumberOccupants)
			return false;

		return !pInfantry->IsMindControlled();
	}

	// True when this building is one we govern, i.e. safe for us to touch the
	// unit's Target over. Untagged buildings are never interfered with.
	bool IsGovernedBuilding(BuildingClass* pBuilding)
	{
		if (!pBuilding || !pBuilding->Type)
			return false;

		const auto pExt = TechnoTypeExt::ExtMap.Find(pBuilding->Type);
		return pExt && pExt->HasOccupancyPolicy();
	}

	void Verdict(InfantryClass* pInfantry, BuildingClass* pBuilding, const char* pWhy)
	{
		if (!pInfantry || !pInfantry->Type || !pBuilding || !pBuilding->Type)
			return;

		static std::set<std::tuple<const void*, const void*, const void*>> reported;
		if (reported.emplace((const void*)pInfantry->Type,
			(const void*)pBuilding->Type, (const void*)pWhy).second)
		{
			Debug::Log("[PayloadExt-diag] VERDICT %s -> %s: %s\n",
				pInfantry->Type->ID, pBuilding->Type->ID, pWhy);
		}
	}
}

// ---------------------------------------------------------------------------
// Diagnostic only — BuildingClass::CanBeOccupiedBy @0x457CE0, the ENTRY.
//
// All nine call sites of this function converge here, so logging the caller's
// return address says exactly which code path is asking about a given pairing —
// and, by its absence, which paths never ask at all. That distinction is what
// the previous rounds kept guessing at.
//
// Prologue `sub esp,0xC / push esi / push edi` = exactly 5 bytes, so the steal
// lands on an instruction boundary. At entry ESP is untouched, so [ESP] is the
// return address and [ESP+4] the infantry argument — confirmed by 0x457CE5
// reading that argument as [ESP+0x18] after 0x14 bytes of prologue. Always
// returns 0; this only observes.
//
// Callers, for decoding the logged address:
//   0x4DFD89 / 0x4DFE54  FootClass, garrison-target selection
//   0x5196D4             InfantryClass::UpdatePosition
//   0x51E694             InfantryClass::WhatAction   (the CURSOR)
//   0x51F4A4             InfantryClass::Mission_Attack
//   0x51F591             InfantryClass::Mission_Hunt
//   0x6F832F / 0x6F844E  TechnoClass threat evaluation
//   0x6FA3CE             TechnoClass::AI
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x457CE0, BuildingClass_CanBeOccupiedBy_PayloadTrace, 0x5)
{
	GET(BuildingClass* const, pBuilding, ECX);
	GET_STACK(DWORD const, callerAddr, 0x0);
	GET_STACK(InfantryClass* const, pInfantry, 0x4);

	if (pBuilding && pBuilding->Type && pInfantry && pInfantry->Type)
	{
		static std::set<std::tuple<DWORD, const void*, const void*>> reported;
		if (reported.emplace(callerAddr, (const void*)pInfantry->Type,
			(const void*)pBuilding->Type).second)
		{
			Debug::Log("[PayloadExt-diag] ASKED from 0x%X: %s -> %s "
				"(Occupier=%d Assaulter=%d)\n",
				callerAddr, pInfantry->Type->ID, pBuilding->Type->ID,
				(int)pInfantry->Type->Occupier, (int)pInfantry->Type->Assaulter);
		}
	}

	return 0;
}

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
	{
		Verdict(pInfantry, pBuilding, "no policy -> deferred to Antares/vanilla");
		return 0;
	}

	if (!TechnoTypeExt::AdmitsOccupant(pBuilding, pInfantry))
	{
		Verdict(pInfantry, pBuilding, "REFUSED by matrix");
		return CannotOccupy;
	}

	// We admit it. If the infantry is a normal Occupier the downstream code would
	// admit it too, so hand back to Antares and let it apply its own extras
	// (capacity, raidable bunkers, ownership, mind-control). Only a FORCED
	// non-Occupier has to bypass, because Antares would reject it outright.
	if (pInfantry->Type->Occupier)
	{
		Verdict(pInfantry, pBuilding, "admitted, but Occupier=yes -> deferred to Antares");
		return 0;
	}

	// Bypassing means we owe the guards Antares would have applied. Replicate
	// the two that actually matter for a forced occupant.
	if (pBuilding->GetOccupantCount() >= pBuilding->Type->MaxNumberOccupants)
	{
		Verdict(pInfantry, pBuilding, "REFUSED: building full");
		return CannotOccupy;
	}

	if (pInfantry->IsMindControlled())
	{
		Verdict(pInfantry, pBuilding, "REFUSED: mind-controlled");
		return CannotOccupy;
	}

	Verdict(pInfantry, pBuilding, "ADMITTED (forced non-Occupier)");

	// Record ONLY here — on the path that actually says yes.
	//
	// 2026-09-21: this used to run right after AdmitsOccupant, i.e. BEFORE the
	// capacity check and before the Occupier early-return. Two bugs fell out of
	// that, both seen in game:
	//   * a FULL building was still recorded, so the restore hook reinstated the
	//     target, the infantry walked over, was refused entry, and — now holding
	//     the building as its TARGET — opened fire on its owner's own structure;
	//   * Occupiers were recorded too, though they never need help, and the
	//     stale target made them pace back and forth in front of a full building.
	// Admission is the only thing that licenses a restore, so only a completed
	// admission may record one.
	RememberAdmission(pInfantry, pBuilding);

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

		// TEMPORARY DIAGNOSTIC: exactly one line per (gate, infantry type,
		// building type).
		//
		// The previous version keyed on (gate,building) OR (infantryType,building)
		// with a short-circuiting ||, which mixed two key namespaces in one set:
		// whether a combination printed depended on what had printed before it.
		// That made ABSENCE of a line unreadable, and I misread it twice — first
		// concluding a gate was never reached when it simply lost the dedupe.
		// A single unambiguous triple key is worth the extra entries.
		static std::set<std::tuple<const void*, const void*, const void*>> reported;
		if (reported.emplace((const void*)pGate, (const void*)pInfantry->Type,
			pBuilding ? (const void*)pBuilding->Type : nullptr).second)
		{
			Debug::Log("[PayloadExt-diag] gate %s: %s -> %s "
				"(Occupier=%d Assaulter=%d hasPolicy=%d admits=%d)\n",
				pGate, pInfantry->Type->ID,
				(pBuilding && pBuilding->Type) ? pBuilding->Type->ID : "<not-a-building>",
				(int)pInfantry->Type->Occupier, (int)pInfantry->Type->Assaulter,
				(int)hasPolicy, (int)admits);
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

// InfantryClass::Mission_Attack @0x51F489 — "my target is a building I could
// garrison, so go garrison it instead of shooting it".
//
// ⚠ NAME CORRECTED 2026-09-14. This was called ActionOnObject for six days and
// the diagnostic labelled it that way; it is WRONG. The InfantryClass vtable at
// 0x7EB058 puts 0x51F3E0 in slot +0x210, and the function is nop-padded at
// 0x51F3E0 and runs through 0x51F53E — so 0x51F489 is inside it. Anchoring
// MissionClass's declared virtual order via QueueMission=+0x1E8 /
// ForceMission=+0x1F0 (both observed at 0x51F449 / 0x51F4C6) puts Mission_Sleep
// at +0x204, hence +0x210 = Mission_Attack.
//
// It matters: a mission handler runs PER FRAME on a unit that already holds the
// Attack mission, whereas ActionOnObject would be the one-shot order decision.
// Reading this as the order path sent me looking for the click handler in the
// wrong place.
//
// `mov cl,[eax+0xEB4]` = 6 bytes. ESI = infantry, EAX = its Type.
// 0x51F49D loads the building from [ESI+0x2B4] and calls CanBeOccupiedBy; the
// success path at 0x51F4AD does SetDestination(building,1) + ForceMission(8).
// ===========================================================================
// THE ACTUAL FIX (2026-09-19) — the C4 gate, upstream of everything else.
//
// Rex ran the matched pair and it flipped BOTH ways:
//     GHOST with C4=yes commented out -> stopped entering (it had worked)
//     GGI   with C4=yes added         -> started entering (it had failed)
//     SNIPE untouched (no C4)         -> still fails
// so `C4=` — not `Occupier=` — is what lets a player's click end in a garrison.
//
// InfantryTypeClass::C4 = +0xEC2 (INI key "C4" at 0x825978, read 0x524545,
// stored 0x524559). It guards the whole garrison-conversion branch at the TOP
// of both mission handlers:
//
//   Mission_Attack  0x51F3E9  mov cl,[Type+0xEC2]      ; C4?
//                   0x51F3F1  jne 0x51F400             ; yes -> consider it
//                   0x51F3F3  push 0xE / call 0x70D0D0 ; else HasAbility(14)?
//                   0x51F3FE  je  0x51F456             ; neither -> never even
//                                                      ;   looks at the target
//   Mission_Capture 0x4D4B6F  same shape -> 0x4D4BB4 (SetDestination)
//
// 0x70D0D0 is HasAbility: it reads the veterancy struct at techno+0x150 via
// 0x74FF90/0x750010, so the vanilla rule is "C4 or the ability".
//
// Every hook this DLL had — MissionAttack 0x51F489, MissionCapture 0x4D4B96,
// MissionHunt, UpdatePosition, GarrisonBuilding, CanBeOccupiedBy — sits INSIDE
// that branch. With no C4 we never reached any of them, which is why admission
// said "ADMITTED" and nothing happened. The AI Hunt path worked all along
// because Mission_Hunt (0x51F540) has no C4 gate.
//
// So: open the gate for infantry that a policy building would admit. A building
// with no policy is untouched, and we never suppress the vanilla C4 path — we
// only ADD a reason to proceed, so C4 units behave exactly as before.
// ===========================================================================
DEFINE_HOOK(0x51F3E9, InfantryClass_MissionAttack_PayloadC4Gate, 0x6)
{
	enum { ConsiderGarrison = 0x51F400 };

	GET(InfantryClass* const, pInfantry, ESI);

	return PolicyAdmits(pInfantry, TechnoAt(pInfantry, 0x2B4), "MissionAttack.C4")
		? ConsiderGarrison : 0;
}

// FootClass::Mission_Capture @0x4D4B6F — the same gate on the capture path.
// ESI and EDI are the same object here: the prologue's branchless
// abstract_cast leaves EDI = (WhatAmI()==Infantry ? this : nullptr) and
// 0x4D4B67 has already rejected null, so ESI == EDI. (EDI only becomes the
// Type later, at 0x4D4B86.) Proceeding lands on SetDestination at 0x4D4BB4.
DEFINE_HOOK(0x4D4B6F, FootClass_MissionCapture_PayloadC4Gate, 0x6)
{
	enum { SetDestinationAndWalk = 0x4D4BB4 };

	GET(InfantryClass* const, pInfantry, ESI);

	// GarrisonStillPossible, not PolicyAdmits: a full building must not be walked
	// to. Cancelling is handled once, at 0x4D4B43, which runs earlier in the same
	// frame -- so by the time we get here an impossible garrison has already had
	// its Target cleared and we simply will not see one.
	const auto pBuilding = abstract_cast<BuildingClass*>(TechnoAt(pInfantry, 0x2B4));

	if (GarrisonStillPossible(pBuilding, pInfantry))
	{
		PolicyAdmits(pInfantry, pBuilding, "MissionCapture.C4");
		return SetDestinationAndWalk;
	}

	return 0;
}

DEFINE_HOOK(0x51F489, InfantryClass_MissionAttack_PayloadOccupierGate, 0x6)
{
	enum { Proceed = 0x51F49D };

	GET(InfantryClass* const, pInfantry, ESI);

	return PolicyAdmits(pInfantry, TechnoAt(pInfantry, 0x2B4), "MissionAttack") ? Proceed : 0;
}

// InfantryClass::UpdatePosition @0x519698 — the arrival step.
// `mov cl,[eax+0xEB4]` = 6 bytes. ESI = infantry, EAX = its Type.
// 0x5196A6 loads the building from [ESI+0x5A4], confirms WhatAmI() == Building
// (cmp eax,6) and then calls CanBeOccupiedBy.
DEFINE_HOOK(0x519698, InfantryClass_UpdatePosition_PayloadOccupierGate, 0x6)
{
	enum { Proceed = 0x5196A6 };

	GET(InfantryClass* const, pInfantry, ESI);

	const auto pDest = TechnoAt(pInfantry, 0x5A4);
	const bool admits = PolicyAdmits(pInfantry, pDest, "UpdatePosition");

	// TEMPORARY DIAGNOSTIC: the approach itself.
	//
	// Everything upstream of here is now proven working — our matrix ADMITS the
	// infantry and it does hold Mission::Capture with the right Destination — yet
	// it never enters. The only surviving test between this point and the entry
	// is at 0x5196CD:
	//
	//     ecx = MapClass::GetCellAt(this->Location)      ; [esp+0x14], set 0x519664
	//     edi = this->Destination                        ; 0x5196C2
	//     call 0x47C520                                  ; CellClass::GetBuilding()
	//     cmp edi,eax / jne 0x51973C                     ; must be standing ON it
	//
	// i.e. the unit has to be physically on the destination building's cell. So
	// the question is no longer "is it allowed in" but "does it ever arrive".
	// Logging both sides of that comparison each time the branch runs shows
	// whether it closes in and stops, or never moves at all. Bounded rather than
	// deduped, because here the SEQUENCE is the evidence.
	if (admits)
	{
		static int approachLines = 0;
		if (approachLines < 40)
		{
			++approachLines;
			// MapClass::Instance is a DEFINE_REFERENCE (an object at 0x87F7E8),
			// not a pointer — same instance the hooked code loads into ECX.
			const auto pCell = MapClass::Instance.TryGetCellAt(pInfantry->Location);
			const auto pOnCell = pCell ? pCell->GetBuilding() : nullptr;
			// admits==true implies PolicyAdmits' abstract_cast succeeded, so the
			// destination really is a BuildingClass here.
			const auto pDestBld = abstract_cast<BuildingClass*>(pDest);
			Debug::Log("[PayloadExt-diag] approach %s: loc=(%d,%d) cellBld=%s "
				"dest=%p destBld=%s %s\n",
				pInfantry->Type->ID,
				pInfantry->Location.X, pInfantry->Location.Y,
				(pOnCell && pOnCell->Type) ? pOnCell->Type->ID : "<none>",
				(void*)pDest,
				(pDestBld && pDestBld->Type) ? pDestBld->Type->ID : "<none>",
				(pOnCell == pDestBld) ? "MATCH -> entering" : "no match");
		}
	}

	return admits ? Proceed : 0;
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

// NOTE (2026-09-18): a QueueMission/ForceMission trace lived here. It answered
// its question and was removed. The conclusion it produced — "GGI never receives
// Mission::Capture" — came from a run that did not actually contain a completed
// order, and is WRONG: with a real order GGI takes the mission, walks the whole
// way, and reaches InfantryClass::GarrisonBuilding. Two lessons kept here rather
// than relearned: absence of a log line is only evidence when the run is known to
// contain the event, and a budgeted diagnostic on a hot shared path (every mission
// on every unit) gets spent by AI traffic long before the human test happens —
// filter on the governed buildings, not on the mission id.

// NOTE (2026-09-19): an ORDER trace at 0x4DF0E0 (InfantryClass vtable +0x4A4,
// the Action->Mission translator) lived here. It produced ZERO lines, because it
// filtered on the objective at +0x2B4 — which is not yet populated when the event
// is built. Removed rather than kept as noise on a hot path. The question it was
// meant to answer got settled by Rex's C4 A/B instead; see the C4 gate below.
// Worth remembering: a filter is only valid where the field it reads is live.

// ===========================================================================
// TEMPORARY DIAGNOSTIC — what mission does the PLAYER'S click actually assign?
//
// 2026-09-19, after the C4-gate fix did NOT work. What the logs now pin down:
//
//   GHOST/E1 click -> ENTERED ... appended=1                      (works)
//   GGI/SNIPE click -> ASKED from 0x51E699 + VERDICT ADMITTED, then nothing
//   gate MissionAttack.C4 / MissionCapture.C4 NEVER fire for a policy building,
//     for ANY unit — not even for GHOST, which succeeds.
//
// That last point is the important one and it rules out my previous reading:
// Mission_Attack and Mission_Capture are not on the successful path at all. The
// order itself must set BOTH Mission::Capture and the Destination, because
// UpdatePosition's garrison branch requires GetCurrentMission()==8 and GHOST
// reaches it without ever passing through those two handlers.
//
// So the divergence is inside order execution, and C4 gates it there. There are
// several `C4 || HasAbility(14)` sites in FootClass around 0x4D53xx-0x4D54xx,
// one branch of which does SetDestination + QueueMission(0x11 Sabotage) and
// another SetDestination + QueueMission(8 Capture). Rather than guess which
// branch each unit takes — that guess has been wrong repeatedly — log the
// assignment and its CALLER, which names the branch outright.
//
// QueueMission @0x5B35E0: `mov eax,[esp+4]` + `push esi` = exactly 5 bytes.
// ForceMission @0x5B2FD0: `mov eax,[ecx+0xAC]`           = exactly 6 bytes.
// At entry ECX = this, [ESP] = caller's return address, [ESP+4] = mission.
// Both unhooked by every framework.
//
// FILTER: infantry owned by the CURRENT PLAYER only. The previous attempt at
// this filtered on mission==Capture and had its whole budget eaten by AI
// infantry garrisoning civilian buildings before Rex ever clicked. The player
// issues a handful of orders; the AI issues thousands.
// ===========================================================================
namespace
{
	int PlayerOrderBudget = 60;
}

// ===========================================================================
// TEMPORARY DIAGNOSTIC — "the IFV drives off to attack the enemy base the
// moment it is built. Which DLL did that?"
//
// 2026-09-20. Twenty-seven DLLs are injected and roughly a third of them can
// issue missions, so reading sources to find the guilty one is guesswork. The
// order itself already carries the answer: whoever assigns the mission leaves
// their RETURN ADDRESS on the stack. Log it and resolve it to a module
// in-process, and the culprit names itself in one game.
//
// Reading the caller:
//   * inside gamemd-spawn.exe  -> vanilla / spawner logic, no DLL involved
//   * inside SomeExt.dll       -> that DLL, at the printed offset
//   * no module at all         -> a Syringe stub; read the `push <origin>` at
//                                 stub_base+0x02 to get the hooked address
//                                 (see syringe-hook-size-resume-boundary).
//
// Filter is the type ID prefix "FV", which covers FV plus the TraitExt
// per-instance variants FV$0 / FV$1, and deliberately does NOT filter by owner
// — only the Allied human can build one here, and an owner filter would hide
// the case where something reassigns the unit's house. Change TraceIdPrefix to
// point this at a different unit.
//
// Budget-bounded and observation-only (always returns 0). Delete this block,
// the two calls below and TraceIdPrefix once the question is answered.
// ===========================================================================
namespace
{
	constexpr const char* TraceIdPrefix = "FV";
	int VehicleOrderBudget = 80;

	const char* MissionName(int mission)
	{
		switch (mission)
		{
		case  0: return "Sleep";      case  1: return "Attack";
		case  2: return "Move";       case  3: return "QMove";
		case  4: return "Retreat";    case  5: return "Guard";
		case  6: return "Sticky";     case  7: return "Enter";
		case  8: return "Capture";    case 10: return "Harvest";
		case 11: return "AreaGuard";  case 12: return "Return";
		case 13: return "Stop";       case 14: return "Ambush";
		case 15: return "HUNT";       case 16: return "Unload";
		case 17: return "Sabotage";   case 25: return "Patrol";
		default: return "?";
		}
	}

	// Name the module a code address lives in, and its offset within it.
	// Static buffer, single-threaded game loop, printed immediately — no
	// lifetime concerns. Returns "SYRINGE-STUB-or-heap" when the address
	// belongs to no loaded image, which is itself the answer: a Syringe stub.
	const char* ModuleOf(DWORD address, DWORD& offset)
	{
		offset = 0;

		HMODULE hMod = nullptr;
		if (!GetModuleHandleExA(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
					| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(address), &hMod)
			|| !hMod)
			return "SYRINGE-STUB-or-heap";

		offset = address - static_cast<DWORD>(reinterpret_cast<size_t>(hMod));

		static char path[MAX_PATH];
		if (!GetModuleFileNameA(hMod, path, MAX_PATH))
			return "<unnamed module>";

		const char* const slash = strrchr(path, '\\');
		return slash ? slash + 1 : path;
	}

	void TraceVehicleOrder(void* pThis, DWORD caller, int mission, const char* pHow)
	{
		if (VehicleOrderBudget <= 0)
			return;

		const auto pUnit = abstract_cast<UnitClass*>(
			static_cast<AbstractClass*>(pThis));
		if (!pUnit || !pUnit->Type || !pUnit->Owner)
			return;
		if (_strnicmp(pUnit->Type->ID, TraceIdPrefix, strlen(TraceIdPrefix)) != 0)
			return;

		--VehicleOrderBudget;

		DWORD offset = 0;
		const char* const pModule = ModuleOf(caller, offset);

		Debug::Log("[PayloadExt-diag] IFVORDER %s@%p (%s#%d) <- %s(%d %s) "
			"from 0x%X [%s+0x%X] was=%d target=%p dest=%p frame=%d\n",
			pUnit->Type->ID, (void*)pUnit,
			pUnit->Owner->get_ID(), pUnit->Owner->ArrayIndex,
			pHow, mission, MissionName(mission), caller,
			pModule, offset,
			(int)pUnit->CurrentMission,
			(void*)pUnit->Target, (void*)pUnit->Destination,
			Unsorted::CurrentFrame);
	}
}

DEFINE_HOOK(0x5B35E0, MissionClass_QueueMission_PayloadPlayerTrace, 0x5)
{
	GET(void* const, pThis, ECX);
	GET_STACK(DWORD const, caller, 0x0);
	GET_STACK(int const, mission, 0x4);

	TraceVehicleOrder(pThis, caller, mission, "QueueMission");

	if (PlayerOrderBudget > 0)
	{
		const auto pInf = abstract_cast<InfantryClass*>(
			static_cast<AbstractClass*>(pThis));
		if (pInf && pInf->Type && pInf->Owner && pInf->Owner->IsCurrentPlayer())
		{
			--PlayerOrderBudget;
			Debug::Log("[PayloadExt-diag] PLAYERMISSION %s <- QueueMission(%d) "
				"from 0x%X (target=%p dest=%p)\n",
				pInf->Type->ID, mission, caller,
				(void*)TechnoAt(pInf, 0x2B4), (void*)TechnoAt(pInf, 0x5A4));
		}
	}

	return 0;
}

DEFINE_HOOK(0x5B2FD0, MissionClass_ForceMission_PayloadPlayerTrace, 0x6)
{
	GET(void* const, pThis, ECX);
	GET_STACK(DWORD const, caller, 0x0);
	GET_STACK(int const, mission, 0x4);

	TraceVehicleOrder(pThis, caller, mission, "ForceMission");

	if (PlayerOrderBudget > 0)
	{
		const auto pInf = abstract_cast<InfantryClass*>(
			static_cast<AbstractClass*>(pThis));
		if (pInf && pInf->Type && pInf->Owner && pInf->Owner->IsCurrentPlayer())
		{
			--PlayerOrderBudget;
			Debug::Log("[PayloadExt-diag] PLAYERMISSION %s <- ForceMission(%d) "
				"from 0x%X (target=%p dest=%p)\n",
				pInf->Type->ID, mission, caller,
				(void*)TechnoAt(pInf, 0x2B4), (void*)TechnoAt(pInf, 0x5A4));
		}
	}

	return 0;
}

// ===========================================================================
// TEMPORARY DIAGNOSTIC — why does the Capture order get reverted?
//
// 2026-09-20. The player-order trace finally shows the real failure, and it is
// NOT the order being wrong:
//
//   PLAYERMISSION GGI <- QueueMission(8) from 0x4C73BF     <- correct order!
//   PLAYERMISSION GGI <- QueueMission(5) from 0x51CD9C     <- reverted to Guard
//   PLAYERMISSION E1  <- QueueMission(8) from 0x4C73BF     <- and nothing after
//
// So GGI DOES receive Mission::Capture from the click, exactly like E1, and is
// then overridden one call later. Every earlier theory (Occupier, Deployer, the
// C4 gates in Mission_Attack/Mission_Capture, the event's mission byte) was
// looking in the wrong place: the order is issued correctly for everyone.
//
// 0x51CD9C is the return address of the QueueMission inside the function at
// 0x51CBA0 = InfantryClass vtable slot +0x484, a two-arg routine that re-derives
// the unit's mission:
//
//   0x51CBE5  mov eax,[this+0x2B4]      ; Target
//   0x51CBED  je  0x51CC1F              ; NULL -> fall through to idle logic
//   0x51CC0F  cmp eax,8                 ; else: CurrentMission == Capture?
//   0x51CC18  mov edi,eax / jmp         ;   yes -> KEEP Capture
//   ...0x51CD37 mov edi,5               ; otherwise -> Guard
//   0x51CD96  call [vtable+0x1E8]       ; QueueMission(edi, 0)
//
// i.e. the keep-Capture branch needs a non-null Target. So the question is
// exactly: what is this called with, and what is the unit's state at that
// moment? Logging the arguments answers it directly instead of another guess.
//
// Prologue `mov eax,[esp+8]` + `push ebx` = exactly 5 bytes. At entry ECX =
// this, [ESP+4] = arg1, [ESP+8] = arg2. Current-player infantry only, bounded.
// ===========================================================================
namespace
{
	int RetargetBudget = 120;
}

DEFINE_HOOK(0x51CBA0, InfantryClass_Retarget_PayloadTrace, 0x5)
{
	GET(InfantryClass* const, pInf, ECX);
	GET_STACK(DWORD const, caller, 0x0);
	GET_STACK(void* const, arg1, 0x4);
	GET_STACK(void* const, arg2, 0x8);

	if (RetargetBudget > 0 && pInf && pInf->Type
		&& pInf->Owner && pInf->Owner->IsCurrentPlayer())
	{
		--RetargetBudget;
		const auto pTgt = TechnoAt(pInf, 0x2B4);
		const auto pDst = TechnoAt(pInf, 0x5A4);
		const auto pTgtBld = abstract_cast<BuildingClass*>(pTgt);
		const auto pDstBld = abstract_cast<BuildingClass*>(pDst);
		Debug::Log("[PayloadExt-diag] RETARGET %s: from 0x%X arg1=%p arg2=%p mission=%d "
			"target=%p(%s) dest=%p(%s)\n",
			pInf->Type->ID, caller, arg1, arg2, (int)pInf->CurrentMission,
			(void*)pTgt, (pTgtBld && pTgtBld->Type) ? pTgtBld->Type->ID : "-",
			(void*)pDst, (pDstBld && pDstBld->Type) ? pDstBld->Type->ID : "-");
	}

	return 0;
}

// NOTE (2026-09-21): a SetTarget trace at 0x51B1F0 lived here and produced ZERO
// lines -- for GGI *and* for E1, which works. So that address is not the SetTarget
// the order dispatcher reaches, and my vtable +0x3C8 resolution was wrong. Removed
// rather than kept as a misleading dead end. Recorded because the absence was only
// interpretable thanks to E1 being in the same run as a positive control: with no
// known-good case logged alongside, 'no lines' would have looked like a finding.


// ===========================================================================
// THE FIX — restore the garrison target the order failed to deliver.
//
// Established over the preceding rounds, all from logs rather than inference:
//
//   1. Our matrix admits the unit:
//        VERDICT GGI -> GAPILE: ADMITTED (forced non-Occupier)
//   2. The click assigns the right mission:
//        PLAYERMISSION GGI <- QueueMission(8) from 0x4C73BF
//   3. A frame later Mission_Capture runs with NO target and cancels it:
//        RETARGET GGI: from 0x4D4BDF ... mission=8 target=0 dest=0
//        PLAYERMISSION GGI <- QueueMission(5) from 0x51CD9C
//
// Mission_Capture's first test is `mov ecx,[this+0x2B4]; je 0x4D4BC7` — a null
// Target goes straight to the bail, which calls [vtable+0x484] and re-derives
// the mission to Guard. That is why the C4 gate hooked at 0x4D4B6F never fired:
// the bail happens 0x2C bytes earlier.
//
// Why the order arrives without a target is still unexplained, and I have
// stopped trying to find out by inspection — the last two attempts to name the
// responsible site were both wrong. What is NOT in doubt is which building the
// player asked for: CanBeOccupiedBy is called with (building, infantry) and we
// answer ADMITTED one or two frames earlier. So record that answer and put the
// target back when the engine reaches Mission_Capture without one.
//
// This composes with the existing C4-gate hook rather than duplicating it: once
// Target is non-null the function proceeds normally to 0x4D4B6F, where that hook
// opens the gate, and vanilla's own SetDestination at 0x4D4BB4 does the walking.
// We set the target and nothing else.
//
// SYNC: the admission test runs identically on every machine and the frame
// counter is synced, so the record and its expiry are deterministic. No RNG, no
// float, no per-machine state.
//
// POINTER SAFETY: a raw BuildingClass* in a long-lived map is the mistake behind
// [[aggressivestance-rawptr-map-leak]]. This keeps the window to a handful of
// frames, erases the entry the moment it is used, and prunes stale entries on
// every lookup, so a recorded building cannot outlive the order that created it.
// ===========================================================================
namespace
{
	struct PendingGarrison
	{
		BuildingClass* Building;
		int Frame;
	};

	// Deliberately tiny and short-lived; see POINTER SAFETY above.
	std::map<InfantryClass*, PendingGarrison> PendingGarrisons;

	// The order lands the frame after admission; a couple of frames of slack is
	// plenty and keeps any stale pointer from surviving long enough to matter.
	constexpr int PendingGarrisonWindow = 15;

	void RememberAdmission(InfantryClass* pInfantry, BuildingClass* pBuilding)
	{
		if (!pInfantry || !pBuilding)
			return;

		PendingGarrisons[pInfantry] = { pBuilding, Unsorted::CurrentFrame };
	}

	BuildingClass* TakeAdmission(InfantryClass* pInfantry)
	{
		if (PendingGarrisons.empty())
			return nullptr;

		const int now = Unsorted::CurrentFrame;

		for (auto it = PendingGarrisons.begin(); it != PendingGarrisons.end(); )
		{
			if (now - it->second.Frame > PendingGarrisonWindow)
				it = PendingGarrisons.erase(it);
			else
				++it;
		}

		const auto it = PendingGarrisons.find(pInfantry);
		if (it == PendingGarrisons.end())
			return nullptr;

		const auto pBuilding = it->second.Building;
		PendingGarrisons.erase(it);
		return pBuilding;
	}
}

// FootClass::Mission_Capture @0x4D4B43 — `mov ecx,[esi+0x2B4]`, exactly 6 bytes,
// the Target load whose null case bails. ESI = this (set by `mov esi,ecx` in the
// prologue). We return 0 in every case: Syringe runs the stolen bytes after us,
// so the re-read picks up whatever we just stored and the engine carries on as
// if the order had carried the target all along.
// ---------------------------------------------------------------------------
// FootClass::Mission_Capture @0x4D4B43 — route the garrison via DESTINATION, not
// Target. 6 bytes (`mov ecx,[esi+0x2B4]`); ESI = this.
//
// 2026-09-23, from Rex: "infantry are displaying the attack target line rather
// than enter... sometimes they shoot once real quick even when there is space."
// That is not a timing problem, and no amount of clearing Target later fixes it:
// a unit with a Target IS attacking, from the instant the order is given. Target
// (+0x2B4) is the field the attack logic and the order-line renderer both read.
// Writing it was the wrong mechanism, and the stray shot was the giveaway —
// writing the raw field also skipped the bookkeeping the engine's own SetTarget
// does (it resets +0x5E0 and relinks the targeting chain), leaving stale attack
// state that discharged once before the unit went in.
//
// So stop touching Target entirely and set DESTINATION instead — the field that
// drives movement and nothing else. Mission_Capture tolerates this exactly:
//
//   0x4D4B4B  Target null        -> 0x4D4BC7
//   0x4D4BC7  Destination SET    -> jne 0x4D4C14   (no cancel)
//   0x4D4C14  Target null        -> house check 0x50B730
//   0x4D4C24  human-controlled   -> jne 0x4D4C71   -> just returns a delay
//
// so the mission stays Capture, the unit walks to the Destination, and
// UpdatePosition garrisons it on arrival — that path needs a Destination and a
// mission of Capture, and never reads Target. Vanilla itself passes the building
// as the destination at 0x4D4BB4, so this is the same shape, just reached
// without a Target.
//
// ⚠ AI CAVEAT: 0x50B730 tests HouseClass+0x1EC/+0x1ED, i.e. human control. For an
// AI house it returns false and a non-Occupier falls through 0x4D4C3F to
// 0x4D4C49, which clears the destination and queues Hunt. Not handled here
// because the AI reaches garrisons through Mission_Hunt instead, which works —
// but if AI houses are ever wanted on this path, 0x4D4C3F is the gate.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x4D4B43, FootClass_MissionCapture_PayloadRouteToBuilding, 0x6)
{
	GET(TechnoClass* const, pThis, ESI);

	const auto pInfantry = abstract_cast<InfantryClass*>(pThis);
	if (!pInfantry || !pInfantry->Type)
		return 0;

	const auto pRouted = abstract_cast<BuildingClass*>(TechnoAt(pInfantry, 0x5A4));

	// Already walking to a building we govern: cancel if it stopped being
	// enterable (someone else took the last slot while this unit was crossing
	// the map — the over-order case), otherwise leave it to finish the journey.
	if (pRouted && IsGovernedBuilding(pRouted))
	{
		if (!GarrisonStillPossible(pRouted, pInfantry))
		{
			// Destination only. With Target never set, dropping the Destination
			// sends the next frame through 0x4D4BC7 to the engine's own cancel,
			// which re-derives to Guard: a quiet lapse, no attack, no retry.
			pInfantry->SetDestination(nullptr, true);

			static std::set<std::pair<const void*, const void*>> lapsed;
			if (lapsed.emplace((const void*)pInfantry->Type,
				(const void*)pRouted->Type).second)
			{
				Debug::Log("[PayloadExt-diag] LAPSED %s -> %s: no longer enterable "
					"(occupants %d/%d); destination cleared\n",
					pInfantry->Type->ID, pRouted->Type->ID,
					pRouted->GetOccupantCount(),
					pRouted->Type->MaxNumberOccupants);
			}
		}

		return 0;
	}

	// Never override a destination or target the engine set for itself.
	if (TechnoAt(pInfantry, 0x5A4) || TechnoAt(pInfantry, 0x2B4))
		return 0;

	const auto pBuilding = TakeAdmission(pInfantry);
	if (!pBuilding || !GarrisonStillPossible(pBuilding, pInfantry))
		return 0;

	pInfantry->SetDestination(pBuilding, true);

	Debug::Log("[PayloadExt-diag] ROUTED %s -> %s: destination set for "
		"Mission::Capture (no target, so no attack)\n",
		pInfantry->Type->ID, pBuilding->Type->ID);

	return 0;
}
