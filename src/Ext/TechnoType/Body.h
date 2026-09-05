#pragma once

#include <vector>

#include <Utilities/Container.h>
#include <Utilities/TemplateDef.h>

#include <TechnoTypeClass.h>

class BuildingClass;
class InfantryClass;
class InfantryTypeClass;
class WeaponTypeClass;

// PayloadExt's TechnoType extension. Uses Container<T> in unordered_map mode
// (Canary defined, no ExtPointerOffset) so we claim no pointer slot inside
// TechnoTypeClass and never collide with Antares/Phobos extension storage.
class TechnoTypeExt
{
public:
	using base_type = TechnoTypeClass;

	// Unique canary (distinct from Phobos's 0x11111111 and the sibling DLLs',
	// e.g. TechnoAttachmentExt's 0x0A77AC77).
	static constexpr DWORD Canary = 0x0BA1F00D;
	// No ExtPointerOffset -> Container uses the unordered_map path.

	// One entry of the building's OWN garrison weapon list (RA2 mode, §GARRISON).
	struct GarrisonWeaponEntry
	{
		// Handed straight back to the engine from BuildingClass::GetWeapon, so it
		// must outlive the call — it lives in the type's ext data.
		WeaponStruct Resolved {};
		// Whitelist of occupant types that fire this weapon. EMPTY = matches any.
		ValueableVector<InfantryTypeClass*> Infantry {};
		// Blacklist, applied on top of the whitelist.
		ValueableVector<InfantryTypeClass*> Exclude {};
		// Per-entry modifiers for the crewman manning THIS weapon. All are
		// applied only while that crewman is the one firing (the engine
		// round-robins the crew, one shot each).
		// >1 = slower, <1 = faster. On top of the occupant-count divisor.
		Valueable<double> ROFMultiplier { 1.0 };
		// Scales the damage of the shot this crewman fires.
		Valueable<double> FirepowerMultiplier { 1.0 };
		// Added to the weapon's range, in CELLS. May be negative.
		Valueable<int> RangeBonus { 0 };
	};

	// The three occupant CLASSES a building can admit. They are independent
	// filters, not modes: a building may admit any combination.
	//   0 Vanilla    -- the stock garrison, gated by InfantryType Occupier=
	//   1 RA2        -- the building's own garrison weapon (CanOccupyFire.RA2Mode)
	//   2 OpenTopped -- occupants fire their own weapons out (OpenTopped=)
	static constexpr int OccupyClassCount = 3;

	// INI suffix per class: Occupier / Occupier.RA2 / Occupier.OpenTopped
	static const char* const OccupyClassKeys[OccupyClassCount];

	// Per-building gate for one occupant class.
	struct OccupyGate
	{
		// May this class enter at all? Unset -> defaults to CanBeOccupied=.
		Nullable<bool> Allow {};
		// Types admitted even when their own Occupier[.Class]= says no.
		ValueableVector<InfantryTypeClass*> Force {};
		// `Force=all` -- admit every type regardless of its own flag.
		bool ForceAll = false;
		// Types refused even when everything else would admit them. Combined
		// with Force=all this is the plain blacklist.
		ValueableVector<InfantryTypeClass*> Deny {};
	};

	class ExtData final : public Extension<TechnoTypeClass>
	{
	public:
		// --- Turret index selectors (docs/TURRETS.md) ----------------------
		Valueable<bool> Turret_FollowWeapon;
		ValueableVector<int> Turret_RangeBands;
		ValueableVector<int> Turret_RangeIndices;

		bool HasTurretSelector() const
		{
			return this->Turret_FollowWeapon.Get()
				|| !this->Turret_RangeBands.empty();
		}

		// --- RA2-style occupy fire (docs/GARRISON.md) -----------------------
		// YR: the occupant's own OccupyWeapon fires  (CanOccupyFire=yes).
		// RA2: the BUILDING owns the weapon and infantry crew it. Both coexist;
		// a building picks one. RA2 mode is designed to run with
		// CanOccupyFire=no, which is what stops the YR path from engaging.
		Valueable<bool> CanOccupyFire_RA2Mode;

		// RA2's signature behaviour: the more occupants, the faster it fires.
		Valueable<bool> Garrison_ROFPerOccupant;
		Valueable<int> Garrison_ROFMaxOccupants;   // 0 = uncapped
		// How many crew are needed before it fires at all. 1 = RA2 default
		// ("silent while empty"); 0 = it always fires, just at its base rate,
		// and extra crew only speed it up.
		Valueable<int> Garrison_MinOccupants;

		std::vector<GarrisonWeaponEntry> GarrisonWeapons;

		// --- Occupancy permissions (docs/GARRISON.md) -----------------------
		// InfantryType side. Index 0 (vanilla) is the engine's own Occupier=
		// field and is never stored here; 1 = RA2, 2 = OpenTopped. Unset falls
		// back to [General] -> Occupier.<Class>.Default, and failing that to the
		// type's own vanilla Occupier=.
		Nullable<bool> Occupier_Class[OccupyClassCount] {};

		// BuildingType side, one gate per class.
		OccupyGate OccupyGates[OccupyClassCount] {};

		bool UsesRA2Garrison() const { return this->CanOccupyFire_RA2Mode.Get(); }

		// True when this BUILDING type overrides occupancy policy in any way, so
		// the hook can leave everything else to vanilla/Antares.
		bool HasOccupancyPolicy() const;

		// Picks the garrison weapon for the given occupant type, or nullptr to
		// fall back to the building's own Primary=/Secondary= (which is exactly
		// how RA2's CABUNK01 worked, so an empty list is a valid setup).
		WeaponStruct* PickGarrisonWeapon(InfantryTypeClass* pOccupantType);
		// The entry that would be chosen, so callers can read its modifiers.
		GarrisonWeaponEntry* PickGarrisonEntry(InfantryTypeClass* pOccupantType);

		ExtData(TechnoTypeClass* OwnerObject) : Extension<TechnoTypeClass>(OwnerObject)
			, Turret_FollowWeapon { false }
			, Turret_RangeBands {}
			, Turret_RangeIndices {}
			, CanOccupyFire_RA2Mode { false }
			, Garrison_ROFPerOccupant { true }
			, Garrison_ROFMaxOccupants { 0 }
			, Garrison_MinOccupants { 1 }
			, GarrisonWeapons {}
		{ }

		virtual ~ExtData() = default;

		virtual void LoadFromINIFile(CCINIClass* pINI) override;
		virtual void Initialize() override { }
		virtual void InvalidatePointer(void* ptr, bool bRemoved) override { }

		virtual void LoadFromStream(PhobosStreamReader& Stm) override;
		virtual void SaveToStream(PhobosStreamWriter& Stm) override;

		int ResolveTurretIndex(TechnoClass* pThis, int weaponIndex) const;

	private:
		void ReadOccupancy(INI_EX& exINI, const char* pSection);
		void ReadGarrisonWeapons(INI_EX& exINI, const char* pSection);

		template <typename T>
		void Serialize(T& Stm);
	};

	class ExtContainer final : public Container<TechnoTypeExt>
	{
	public:
		ExtContainer();
		~ExtContainer();
	};

	static ExtContainer ExtMap;

	static bool EngineOwnsTurretNumber(TechnoTypeClass* pType);

	// Convenience: ext for a techno's type, or nullptr.
	static ExtData* Fetch(TechnoClass* pThis);

	// The whole admission rule, in one place. An infantry may enter as class C
	// when:  Allow[C] && (its own Occupier[C] || Force[C]) && !Deny[C]
	// and admission is true when that holds for ANY class.
	static bool AdmitsOccupant(BuildingClass* pBuilding, InfantryClass* pInfantry);
};
