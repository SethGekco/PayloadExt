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
		// Required by the open-topped firing/targeting paths, which read the
		// passenger's Transporter to know where it is shooting from.
		pInfantry->Transporter = pBuilding;
		pBuilding->EnteredOpenTopped(pInfantry);
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
