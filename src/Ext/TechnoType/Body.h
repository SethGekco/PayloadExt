#pragma once

#include <vector>

#include <Utilities/Container.h>
#include <Utilities/TemplateDef.h>

#include <TechnoTypeClass.h>

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

		std::vector<GarrisonWeaponEntry> GarrisonWeapons;

		bool UsesRA2Garrison() const { return this->CanOccupyFire_RA2Mode.Get(); }

		// Picks the garrison weapon for the given occupant type, or nullptr to
		// fall back to the building's own Primary=/Secondary= (which is exactly
		// how RA2's CABUNK01 worked, so an empty list is a valid setup).
		WeaponStruct* PickGarrisonWeapon(InfantryTypeClass* pOccupantType);

		ExtData(TechnoTypeClass* OwnerObject) : Extension<TechnoTypeClass>(OwnerObject)
			, Turret_FollowWeapon { false }
			, Turret_RangeBands {}
			, Turret_RangeIndices {}
			, CanOccupyFire_RA2Mode { false }
			, Garrison_ROFPerOccupant { true }
			, Garrison_ROFMaxOccupants { 0 }
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
};
