#include "Body.h"

#include <cstring>
#include <utility>

#include <TechnoClass.h>
#include <BuildingClass.h>
#include <InfantryClass.h>
#include <InfantryTypeClass.h>
#include <WeaponTypeClass.h>
#include <Utilities/Macro.h>

#include <Ext/Rules/Body.h>

TechnoTypeExt::ExtContainer TechnoTypeExt::ExtMap;

// "" = vanilla garrison, ".RA2" = RA2 mode, ".OpenTopped" = open-topped.
const char* const TechnoTypeExt::OccupyClassKeys[TechnoTypeExt::OccupyClassCount] =
	{ "", ".RA2", ".OpenTopped" };

// TechnoTypeClass::GetWeaponTurretIndex(int weapon) — vanilla __thiscall at
// 0x7178B0. Verified by disassembly of a clean gamemd.exe:
//     mov eax, [esp+4]                 ; weapon index
//     mov eax, [ecx + eax*4 + 0x814]   ; per-weapon turret index array
//     retn 4
// The mapping Antares exposes as WeaponTurretIndex<N>= is backed by a VANILLA
// array; Antares only hooks this to serve weapon indices >= MaxWeapons (18) out
// of its own overflow vector. We call through the address rather than reading
// +0x814 directly so Antares' overflow handling is honored when it is loaded.
using GetWeaponTurretIndexFunc = int(__thiscall*)(TechnoTypeClass*, int);
static const auto CallGetWeaponTurretIndex =
	reinterpret_cast<GetWeaponTurretIndexFunc>(0x7178B0);

TechnoTypeExt::ExtData* TechnoTypeExt::Fetch(TechnoClass* pThis)
{
	return pThis ? ExtMap.Find(pThis->GetTechnoType()) : nullptr;
}

// The engine keeps CurrentTurretNumber for itself on gattling units (gattling
// stage) and charge-turret units (charge animation frame — the SREF pattern).
// Antares' own SwitchGunner hook guards IsChargeTurret for the same reason.
bool TechnoTypeExt::EngineOwnsTurretNumber(TechnoTypeClass* pType)
{
	return pType->IsGattling || pType->IsChargeTurret;
}

int TechnoTypeExt::ExtData::ResolveTurretIndex(TechnoClass* pThis, int weaponIndex) const
{
	const auto pType = this->OwnerObject();

	// Range selector wins when a target exists: it is the continuous one, and a
	// unit that is firing is also in range of something.
	if (!this->Turret_RangeBands.empty())
	{
		if (const auto pTarget = pThis->Target)
		{
			// DistanceFrom returns leptons; 256 leptons per cell.
			const int cells = pThis->DistanceFrom(pTarget) / 256;

			size_t band = 0;
			while (band < this->Turret_RangeBands.size()
				&& cells >= this->Turret_RangeBands[band])
			{
				++band;
			}

			if (!this->Turret_RangeIndices.empty())
			{
				const size_t slot = band < this->Turret_RangeIndices.size()
					? band
					: this->Turret_RangeIndices.size() - 1;
				return this->Turret_RangeIndices[slot];
			}

			return static_cast<int>(band);
		}
	}

	if (this->Turret_FollowWeapon.Get() && weaponIndex >= 0)
		return CallGetWeaponTurretIndex(pType, weaponIndex);

	return -1;
}

bool TechnoTypeExt::ExtData::HasOccupancyPolicy() const
{
	for (auto const& gate : this->OccupyGates)
	{
		if (gate.Allow.isset() || gate.ForceAll
			|| !gate.Force.empty() || !gate.Deny.empty())
		{
			return true;
		}
	}
	return false;
}

// Occupier<suffix>=            on InfantryTypes
// Occupier<suffix>.Allow=      on BuildingTypes  (default: CanBeOccupied=)
// Occupier<suffix>.Force=      list, or "all"
// Occupier<suffix>.Deny=       list
void TechnoTypeExt::ExtData::ReadOccupancy(INI_EX& exINI, const char* pSection)
{
	char key[0x40];

	for (int i = 0; i < OccupyClassCount; ++i)
	{
		const char* const suffix = OccupyClassKeys[i];

		// Infantry side. Index 0 is the engine's own Occupier= field, so there is
		// no PayloadExt key for it.
		if (i > 0)
		{
			_snprintf_s(key, _TRUNCATE, "Occupier%s", suffix);
			this->Occupier_Class[i].Read(exINI, pSection, key);
		}

		auto& gate = this->OccupyGates[i];

		_snprintf_s(key, _TRUNCATE, "Occupier%s.Allow", suffix);
		gate.Allow.Read(exINI, pSection, key);

		// Force accepts either a type list or the literal "all". Probe the raw
		// string first so the type parser never sees (and warns about) "all".
		_snprintf_s(key, _TRUNCATE, "Occupier%s.Force", suffix);

		if (exINI.ReadString(pSection, key) > 0)
		{
			const char* const raw = exINI.value();

			if (!_strcmpi(raw, "all") || !_strcmpi(raw, "any"))
				gate.ForceAll = true;
			else
				gate.Force.Read(exINI, pSection, key);
		}

		_snprintf_s(key, _TRUNCATE, "Occupier%s.Deny", suffix);
		gate.Deny.Read(exINI, pSection, key);
	}
}

namespace
{
	bool ListContains(const ValueableVector<InfantryTypeClass*>& list,
		InfantryTypeClass* pType)
	{
		for (auto const pEntry : list)
		{
			if (pEntry == pType)
				return true;
		}
		return false;
	}
}

bool TechnoTypeExt::AdmitsOccupant(BuildingClass* pBuilding, InfantryClass* pInfantry)
{
	if (!pBuilding || !pInfantry || !pBuilding->Type || !pInfantry->Type)
		return false;

	const auto pBldExt = ExtMap.Find(pBuilding->Type);
	const auto pInfExt = ExtMap.Find(pInfantry->Type);

	if (!pBldExt)
		return false;

	const bool vanillaOccupier = pInfantry->Type->Occupier;
	const bool canBeOccupied = pBuilding->Type->CanBeOccupied;

	for (int i = 0; i < OccupyClassCount; ++i)
	{
		auto const& gate = pBldExt->OccupyGates[i];

		// Does the building admit this CLASS at all? Unset -> CanBeOccupied=.
		if (!gate.Allow.Get(canBeOccupied))
			continue;

		// Deny wins over everything else.
		if (ListContains(gate.Deny, pInfantry->Type))
			continue;

		// Does the infantry qualify as this class on its own? Class 0 is the
		// engine's Occupier=; the others fall back to the [General] default and
		// then to Occupier= itself.
		bool qualifies = vanillaOccupier;

		if (i > 0 && pInfExt)
		{
			qualifies = pInfExt->Occupier_Class[i].Get(
				RulesExt::Global()->OccupierClassDefault[i].Get(vanillaOccupier));
		}

		// ...or the building forces it in regardless.
		if (qualifies || gate.ForceAll || ListContains(gate.Force, pInfantry->Type))
			return true;
	}

	return false;
}

WeaponStruct* TechnoTypeExt::ExtData::PickGarrisonWeapon(InfantryTypeClass* pOccupantType)
{
	const auto pEntry = this->PickGarrisonEntry(pOccupantType);
	return pEntry ? &pEntry->Resolved : nullptr;
}

// Specific whitelist entries beat catch-all entries, so declaration order does
// not force modders to put the catch-all last.
TechnoTypeExt::GarrisonWeaponEntry* TechnoTypeExt::ExtData::PickGarrisonEntry(InfantryTypeClass* pOccupantType)
{
	auto excluded = [pOccupantType](const GarrisonWeaponEntry& entry)
	{
		for (auto const pExcluded : entry.Exclude)
		{
			if (pExcluded == pOccupantType)
				return true;
		}
		return false;
	};

	// Pass 1 — an entry that names this infantry type explicitly.
	if (pOccupantType)
	{
		for (auto& entry : this->GarrisonWeapons)
		{
			if (entry.Infantry.empty() || excluded(entry))
				continue;

			for (auto const pAllowed : entry.Infantry)
			{
				if (pAllowed == pOccupantType)
					return &entry;
			}
		}
	}

	// Pass 2 — the first catch-all entry that does not exclude this type.
	for (auto& entry : this->GarrisonWeapons)
	{
		if (entry.Infantry.empty() && !excluded(entry))
			return &entry;
	}

	return nullptr;
}

// GarrisonWeapon=            + .Infantry= / .Exclude=
// GarrisonWeapon[N]=         + .Infantry= / .Exclude=     (N = 0..31)
// Indices may be sparse; a missing N is simply skipped.
void TechnoTypeExt::ExtData::ReadGarrisonWeapons(INI_EX& exINI, const char* pSection)
{
	this->GarrisonWeapons.clear();

	auto readEntry = [this, &exINI, pSection](const char* pBase)
	{
		char key[0x40];

		// Read as a vector: that specialization uses WeaponTypeClass::FindOrAllocate,
		// so a garrison weapon need not be registered in [WeaponTypes].
		ValueableVector<WeaponTypeClass*> weapon;
		weapon.Read(exINI, pSection, pBase);

		if (weapon.empty() || !weapon[0])
			return;

		GarrisonWeaponEntry entry {};
		entry.Resolved.WeaponType = weapon[0];

		_snprintf_s(key, _TRUNCATE, "%s.Infantry", pBase);
		entry.Infantry.Read(exINI, pSection, key);

		_snprintf_s(key, _TRUNCATE, "%s.Exclude", pBase);
		entry.Exclude.Read(exINI, pSection, key);

		_snprintf_s(key, _TRUNCATE, "%s.ROFMultiplier", pBase);
		entry.ROFMultiplier.Read(exINI, pSection, key);

		_snprintf_s(key, _TRUNCATE, "%s.FirepowerMultiplier", pBase);
		entry.FirepowerMultiplier.Read(exINI, pSection, key);

		_snprintf_s(key, _TRUNCATE, "%s.RangeBonus", pBase);
		entry.RangeBonus.Read(exINI, pSection, key);

		this->GarrisonWeapons.emplace_back(std::move(entry));
	};

	readEntry("GarrisonWeapon");

	for (int i = 0; i < 32; ++i)
	{
		char base[0x30];
		_snprintf_s(base, _TRUNCATE, "GarrisonWeapon[%d]", i);
		readEntry(base);
	}
}

void TechnoTypeExt::ExtData::LoadFromINIFile(CCINIClass* const pINI)
{
	const char* pSection = this->OwnerObject()->ID;

	if (!pINI->GetSection(pSection))
		return;

	INI_EX exINI(pINI);

	this->Turret_FollowWeapon.Read(exINI, pSection, "Turret.FollowWeapon");
	this->Turret_RangeBands.Read(exINI, pSection, "Turret.RangeBands");
	this->Turret_RangeIndices.Read(exINI, pSection, "Turret.RangeIndices");

	this->CanOccupyFire_RA2Mode.Read(exINI, pSection, "CanOccupyFire.RA2Mode");
	this->Garrison_ROFPerOccupant.Read(exINI, pSection, "GarrisonWeapon.ROFPerOccupant");
	this->Garrison_ROFMaxOccupants.Read(exINI, pSection, "GarrisonWeapon.ROFMaxOccupants");
	this->Garrison_MinOccupants.Read(exINI, pSection, "CanOccupyFire.RA2Mode.MinOccupants");

	this->ReadOccupancy(exINI, pSection);
	this->ReadGarrisonWeapons(exINI, pSection);
}

// GarrisonWeapons is type data, re-parsed from INI on every load, so it is
// deliberately NOT serialized. Save and Load stay symmetric.
template <typename T>
void TechnoTypeExt::ExtData::Serialize(T& Stm)
{
	Stm
		.Process(this->Turret_FollowWeapon)
		.Process(this->Turret_RangeBands)
		.Process(this->Turret_RangeIndices)
		.Process(this->CanOccupyFire_RA2Mode)
		.Process(this->Garrison_ROFPerOccupant)
		.Process(this->Garrison_ROFMaxOccupants)
		.Process(this->Garrison_MinOccupants)
		;
}

void TechnoTypeExt::ExtData::LoadFromStream(PhobosStreamReader& Stm)
{
	Extension<TechnoTypeClass>::LoadFromStream(Stm);
	this->Serialize(Stm);
}

void TechnoTypeExt::ExtData::SaveToStream(PhobosStreamWriter& Stm)
{
	Extension<TechnoTypeClass>::SaveToStream(Stm);
	this->Serialize(Stm);
}

TechnoTypeExt::ExtContainer::ExtContainer()
	: Container("TechnoTypeClass")
{ }

TechnoTypeExt::ExtContainer::~ExtContainer() = default;

// ============================================================================
// Container lifecycle hooks — addresses as used by the sibling DLLs (verified
// against Phobos develop).
// ============================================================================

DEFINE_HOOK(0x711835, TechnoTypeClass_CTOR_PayloadExt, 0x5)
{
	GET(TechnoTypeClass*, pItem, ESI);
	TechnoTypeExt::ExtMap.TryAllocate(pItem);
	return 0;
}

DEFINE_HOOK(0x711AE0, TechnoTypeClass_DTOR_PayloadExt, 0x5)
{
	GET(TechnoTypeClass*, pItem, ECX);
	TechnoTypeExt::ExtMap.Remove(pItem);
	return 0;
}

DEFINE_HOOK_AGAIN(0x716DC0, TechnoTypeClass_SaveLoad_Prefix_PayloadExt, 0x5)
DEFINE_HOOK(0x7162F0, TechnoTypeClass_SaveLoad_Prefix_PayloadExt, 0x6)
{
	GET_STACK(TechnoTypeClass*, pItem, 0x4);
	GET_STACK(IStream*, pStm, 0x8);
	TechnoTypeExt::ExtMap.PrepareStream(pItem, pStm);
	return 0;
}

DEFINE_HOOK(0x716DAC, TechnoTypeClass_Load_Suffix_PayloadExt, 0xA)
{
	TechnoTypeExt::ExtMap.LoadStatic();
	return 0;
}

DEFINE_HOOK(0x717094, TechnoTypeClass_Save_Suffix_PayloadExt, 0x5)
{
	TechnoTypeExt::ExtMap.SaveStatic();
	return 0;
}

DEFINE_HOOK(0x716123, TechnoTypeClass_LoadFromINI_PayloadExt, 0x5)
{
	GET(TechnoTypeClass*, pItem, EBP);
	GET_STACK(CCINIClass*, pINI, 0x380);
	TechnoTypeExt::ExtMap.LoadFromINI(pItem, pINI);
	return 0;
}
