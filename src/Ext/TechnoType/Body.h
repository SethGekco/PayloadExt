#pragma once

#include <Utilities/Container.h>
#include <Utilities/TemplateDef.h>

#include <TechnoTypeClass.h>

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

	class ExtData final : public Extension<TechnoTypeClass>
	{
	public:
		// --- Turret index selectors (see docs/TURRETS.md) -------------------
		// Opt-in per type. When none is set the type is untouched, so vanilla
		// and every existing mod behave exactly as before.

		// Turret follows the weapon being fired. Reuses the engine's own
		// per-weapon turret index array (TechnoTypeClass+0x814), i.e. the
		// mapping Antares exposes as WeaponTurretIndex<N>= / <Name>TurretIndex=.
		Valueable<bool> Turret_FollowWeapon;

		// Turret follows distance to the current target. Ascending thresholds
		// in cells; Turret_RangeIndices holds one MORE entry than bands:
		//   dist <  band[0]                -> indices[0]
		//   band[i-1] <= dist < band[i]    -> indices[i]
		//   dist >= band[last]             -> indices[last + 1]
		ValueableVector<int> Turret_RangeBands;
		ValueableVector<int> Turret_RangeIndices;

		// True when this type opted into any turret selector at all.
		bool HasTurretSelector() const
		{
			return this->Turret_FollowWeapon.Get()
				|| !this->Turret_RangeBands.empty();
		}

		// --- Crewed weapon (RA2-style garrison) — see docs/GARRISON.md -------
		// The building fires its OWN weapon, but only while infantry are inside,
		// and faster the more of them there are.

		// Requires at least Crew_MinOccupants inside to fire at all.
		Valueable<bool> Crew_Required;
		Valueable<int> Crew_MinOccupants;
		// Divide the rearm delay by the occupant count (the RA2 feel). This
		// mirrors what vanilla does at 0x6FD17B for true occupy-path buildings,
		// but works for a building firing its own weapon.
		Valueable<bool> Crew_ROFPerOccupant;
		// Caps the divisor so a full building isn't absurdly fast. 0 = uncapped.
		Valueable<int> Crew_ROFMaxOccupants;

		bool HasCrewLogic() const
		{
			return this->Crew_Required.Get() || this->Crew_ROFPerOccupant.Get();
		}

		ExtData(TechnoTypeClass* OwnerObject) : Extension<TechnoTypeClass>(OwnerObject)
			, Turret_FollowWeapon { false }
			, Turret_RangeBands {}
			, Turret_RangeIndices {}
			, Crew_Required { false }
			, Crew_MinOccupants { 1 }
			, Crew_ROFPerOccupant { false }
			, Crew_ROFMaxOccupants { 0 }
		{ }

		virtual ~ExtData() = default;

		virtual void LoadFromINIFile(CCINIClass* pINI) override;
		virtual void Initialize() override { }
		virtual void InvalidatePointer(void* ptr, bool bRemoved) override { }

		virtual void LoadFromStream(PhobosStreamReader& Stm) override;
		virtual void SaveToStream(PhobosStreamWriter& Stm) override;

		// Resolves the turret index this type wants right now, or -1 to leave
		// CurrentTurretNumber alone. weaponIndex < 0 means "not firing".
		int ResolveTurretIndex(TechnoClass* pThis, int weaponIndex) const;

	private:
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

	// True when the engine itself owns CurrentTurretNumber for this type and we
	// must not touch it (gattling stages / charge-turret animation frames).
	static bool EngineOwnsTurretNumber(TechnoTypeClass* pType);
};
