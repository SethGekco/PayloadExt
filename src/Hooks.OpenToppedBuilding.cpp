// OpenTopped BUILDINGS — the mirror image of RA2 mode.
//
//   RA2 mode      : the BUILDING owns the weapon, infantry merely crew it.
//   OpenTopped    : each occupant fires ITS OWN weapon out, independently —
//                   exactly what BFRT (Battle Fortress, OpenTopped=yes,
//                   "passengers can shoot out") does for a vehicle.
//   YR garrison   : one shared occupy-weapon, round-robined between occupants.
//
// `OpenTopped` is a TechnoTypeClass field, so it already exists on every
// BuildingType — the engine simply never acts on it for buildings. Setting
// OpenTopped=yes on a BuildingType is therefore a no-op in vanilla, which makes
// it safe to reuse as the opt-in here and keeps the spelling identical to BFRT.
//
// How the engine makes an open-topped passenger fire (verified):
//   TechnoClass::EnteredOpenTopped @0x710470
//       mov byte [passenger+0x82],1   ; InOpenToppedTransport = true
//       call [vtable+0x3D0]           ; virtual on the passenger
//       mov ecx,0x87F778              ; LogicClass::Instance
//       call 0x55BAA0                 ; AddObject(passenger)
//   i.e. it flags the passenger and puts it in the LOGIC LAYER so it keeps
//   receiving Update() ticks while limboed inside its transport. From there the
//   ordinary open-topped machinery applies: CanFire @0x6FC5C7, the damage
//   multiplier @0x6FE43B, and the threat-eval sites that route targeting through
//   the transport. The passenger's Transporter must point at the transport for
//   those to work, so we set it.
//
// Buildings keep their infantry in BuildingClass::Occupants rather than
// Passengers, which is why none of this happens for them by default. We simply
// submit each occupant to the same system on entry and withdraw it on exit.
//
// Pair OpenTopped=yes with CanOccupyFire=no so the BUILDING itself stays silent
// (BuildingClass::CanFire @0x447F2D) and only the occupants shoot.

#include <BuildingClass.h>
#include <InfantryClass.h>
#include <TechnoClass.h>

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

#include <set>

#include <Unsorted.h>

#include <Ext/TechnoType/Body.h>

namespace
{
	bool BuildingIsOpenTopped(BuildingClass* pBuilding)
	{
		return pBuilding && pBuilding->Type && pBuilding->Type->OpenTopped;
	}
}

// ---------------------------------------------------------------------------
// Entry — InfantryClass::GarrisonBuilding @0x52297F.
//
// Verified: the preceding instructions append ESI to the occupants vector
// (`mov %esi,(%edx,%eax,4)`), and `cmpl $0x1,0x694(%ebp)` right after confirms
// EBP is the building (0x694 = Occupants.Count). Stolen bytes are exactly
// `mov eax,[ebp+0]` + `mov ecx,ebp` = 5. Antares hooks this same address as
// InfantryClass_GarrisonBuilding_OccupierEntered; we always return 0 so both
// chain.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x52297F, InfantryClass_GarrisonBuilding_PayloadOpenTopped, 0x5)
{
	GET(BuildingClass* const, pBuilding, EBP);
	GET(InfantryClass* const, pInfantry, ESI);

	if (pInfantry && BuildingIsOpenTopped(pBuilding))
	{
		// EnteredOpenTopped alone is NOT enough. Every Phobos call site pairs it
		// with the same companion state, and a limboed garrison occupant needs
		// all of it to behave like a real open-topped passenger:
		//
		//   Ext/Team/Hooks.cpp:43-47        IsInPlayfield = true; Transporter = ...
		//   Ext/Foot/Body.cpp:662-664       SetLocation(transport->Location)
		//   Ext/Techno/Hooks.Transport:303  SetSpeedPercentage(0.0)
		//                                   "to stop the passengers and let
		//                                    OpenTopped work normally"
		//
		// Without SetLocation the occupant keeps the coordinates it had while
		// walking outside, so its range checks and target scan run from the wrong
		// place; without IsInPlayfield it is not treated as live at all.
		pInfantry->Transporter = pBuilding;
		pInfantry->IsInPlayfield = true;
		pInfantry->SetLocation(pBuilding->Location);
		pInfantry->SetSpeedPercentage(0.0);

		pBuilding->EnteredOpenTopped(pInfantry);

		// The 2026-09-04 diagnostic showed the occupant ticking correctly
		// (InOpenToppedTransport=1, Transporter set, location matching the
		// building) but with tgt=00000000 forever -- it never acquires a target.
		// TechnoClass::Update does NOT skip limboed objects (the only InLimbo
		// test in it, at 0x6FA6FC, guards one small call), so the block is in the
		// MISSION layer: garrisoning leaves the occupant in a mission that never
		// scans. A Battle Fortress passenger keeps an ordinary one.
		//
		// Guard is the right mission for something that cannot move but should
		// shoot what comes into range. Logged below so the value before the
		// change is visible either way.
		const auto missionBefore = pInfantry->CurrentMission;
		pInfantry->QueueMission(Mission::Guard, true);

		// TEMPORARY DIAGNOSTIC: distinguishes "the occupant never got registered"
		// from "it registered but will not shoot". Logged once per building.
		static std::set<BuildingClass*> reported;
		if (reported.insert(pBuilding).second)
		{
			Debug::Log("[PayloadExt-diag] OpenTopped building %s: registered %s "
				"(InOpenToppedTransport=%d Transporter=%p occupants=%d "
				"mission %d -> %d)\n",
				pBuilding->Type->ID, pInfantry->Type->ID,
				(int)pInfantry->InOpenToppedTransport,
				(void*)pInfantry->Transporter,
				pBuilding->Occupants.Count,
				(int)missionBefore, (int)pInfantry->CurrentMission);
		}
	}

	return 0;
}

// ---------------------------------------------------------------------------
// Exit — the per-occupant step of the unload loop @0x4580BD.
//
// Verified: `mov (%eax,%ebp,4),%edi` at 0x4580B1 loads Occupants.Items[EBP] into
// EDI, so EDI is the occupant for every iteration. (0x458197 is only the
// "nowhere to place it" failure branch, so hooking there would miss normal
// exits.) Stolen bytes at 0x4580BD are exactly `call [edx+0xD8]` = 6.
//
// We deliberately drive this from the OCCUPANT alone rather than guessing the
// building register: TechnoClass::ExitedOpenTopped ignores its `this` (YRpp
// notes "this should be the transport, but it's unused"), so the stored
// Transporter is a sufficient and safe receiver.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x4580BD, BuildingClass_UnloadOccupants_PayloadOpenTopped, 0x6)
{
	GET(TechnoClass* const, pOccupant, EDI);

	if (pOccupant && pOccupant->InOpenToppedTransport)
	{
		if (const auto pTransport = pOccupant->Transporter)
		{
			pTransport->ExitedOpenTopped(pOccupant);
			pOccupant->Transporter = nullptr;
		}
	}

	return 0;
}

// ---------------------------------------------------------------------------
// TEMPORARY DIAGNOSTIC — is the occupant actually alive and hunting?
//
// TechnoClass::Update @0x6F9E50, the canonical benign shared entry point (all
// four frameworks hook it and return 0; ECX = TechnoClass*). We only look at
// technos that WE registered as open-topped inside a BUILDING, and only log one
// line per occupant per ~3 seconds.
//
// This separates the two remaining failure modes:
//   * no line at all      -> the occupant is not ticking (not in the logic layer)
//   * lines with tgt=0    -> it ticks but never acquires a target
//   * lines with tgt!=0   -> it has a target, so the problem is downstream in
//                            firing, not targeting
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x6F9E50, TechnoClass_Update_PayloadOpenToppedDiag, 0x5)
{
	GET(TechnoClass* const, pThis, ECX);

	if (!pThis || !pThis->InOpenToppedTransport)
		return 0;

	const auto pTransport = pThis->Transporter;

	if (!pTransport || pTransport->WhatAmI() != AbstractType::Building)
		return 0;

	static int lastFrame = -1000;
	const int frame = Unsorted::CurrentFrame;

	if (frame - lastFrame < 90)
		return 0;

	lastFrame = frame;

	Debug::Log("[PayloadExt-diag] occupant %s in %s: tgt=%p mission=%d inLimbo=%d "
		"playfield=%d loc=(%d,%d) bld=(%d,%d)\n",
		pThis->GetTechnoType()->ID,
		pTransport->GetTechnoType()->ID,
		(void*)pThis->Target,
		(int)pThis->CurrentMission,
		(int)pThis->InLimbo,
		(int)pThis->IsInPlayfield,
		pThis->Location.X, pThis->Location.Y,
		pTransport->Location.X, pTransport->Location.Y);

	return 0;
}
