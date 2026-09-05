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
