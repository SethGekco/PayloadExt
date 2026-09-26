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

	// Not ours -> Antares (or vanilla) decides, untouched...
	if (!pBldExt || !pBldExt->HasOccupancyPolicy())
	{
		// ...with one exception: a type that only has Occupier= because WE granted
		// it at load must not be admitted here.
		//
		// Occupier= is type-wide, so granting it to let a unit into a tagged
		// building would otherwise also let it into every ordinary garrisonable
		// building, civilian ones included. This veto claws that scope back, and it
		// does so by ANSWERING A QUESTION rather than writing anything -- the rule
		// the 2026-09 rewind established. Authored occupiers are untouched, so E1
		// and friends behave exactly as they always have.
		if (TechnoTypeExt::HasSynthesisedOccupier(pInfantry->Type))
		{
			return CannotOccupy;
		}

		return 0;
	}

	if (!TechnoTypeExt::AdmitsOccupant(pBuilding, pInfantry))
	{
		return CannotOccupy;
	}

	// We admit it. If the infantry is a normal Occupier the downstream code would
	// admit it too, so hand back to Antares and let it apply its own extras
	// (capacity, raidable bunkers, ownership, mind-control). Only a FORCED
	// non-Occupier has to bypass, because Antares would reject it outright.
	if (pInfantry->Type->Occupier)
	{
		return 0;
	}

	// Bypassing means we owe the guards Antares would have applied. Replicate
	// the two that actually matter for a forced occupant.
	if (pBuilding->GetOccupantCount() >= pBuilding->Type->MaxNumberOccupants)
	{
		return CannotOccupy;
	}

	if (pInfantry->IsMindControlled())
	{
		return CannotOccupy;
	}

	//

	return CanOccupy;
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

// FootClass::Mission_Capture @0x4D4B43 — `mov ecx,[esi+0x2B4]`, exactly 6 bytes,
// the Target load whose null case bails. ESI = this (set by `mov esi,ecx` in the
// prologue). We return 0 in every case: Syringe runs the stolen bytes after us,
// so the re-read picks up whatever we just stored and the engine carries on as
// if the order had carried the target all along.
// ===========================================================================
// REMOVED 2026-09-24 — the forced-entry state machine that used to live here.
//
// It wrote unit state: Target (+0x2B4), Destination (+0x5A4) and the garrison-seek
// flag (+0x691). Over a dozen builds it broke vanilla three separate ways:
//
//   * units opened fire on the building they had been sent to occupy, because
//     Target is also the attack field;
//   * E1 stopped being able to garrison civilian buildings at all, because a
//     record naming an ungoverned building made the machine cancel the order;
//   * E1 began ignoring orders and entering whichever building was NEAREST,
//     because FindGarrisonStructure picks the nearest and our per-frame pin
//     fought it.
//
// Each fix produced the next regression, which is the signature of an approach
// that is wrong rather than incomplete. Rex called the rewind and he was right.
//
// THE RULE THIS LEAVES BEHIND: hooks in this file may ANSWER QUESTIONS — return a
// branch target, report a verdict — but must not WRITE unit state. Everything that
// has ever worked here (the permission matrix, RA2-mode garrison, open-topped
// buildings, the per-entry modifiers) only ever decided and reported. Everything
// that broke vanilla wrote a field the engine also owned.
//
// Forced entry for a non-Occupier via a PLAYER order therefore remains unsolved.
// What is known, for whoever picks it up:
//   * admission already works — CanBeOccupiedBy says yes for a forced occupant;
//   * the AI reaches it fine through Mission_Hunt, which has no C4 gate;
//   * the player path dies because vanilla's Mission_Capture bails for a
//     non-Occupier before it ever calls SetDestination, and nothing else does;
//   * anything that supplies that call from outside has to win a fight with
//     FindGarrisonStructure every frame, and loses.
// The next attempt should look for a way to make VANILLA make that call, not to
// make it on vanilla's behalf.
// ===========================================================================

