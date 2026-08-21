#include "Body.h"

#include <TechnoClass.h>
#include <Utilities/Macro.h>

TechnoTypeExt::ExtContainer TechnoTypeExt::ExtMap;

// TechnoTypeClass::GetWeaponTurretIndex(int weapon) — vanilla __thiscall at
// 0x7178B0. Verified by disassembly of a clean gamemd.exe:
//     mov eax, [esp+4]                 ; weapon index
//     mov eax, [ecx + eax*4 + 0x814]   ; per-weapon turret index array
//     retn 4
// So the mapping Antares exposes as WeaponTurretIndex<N>= / <Name>TurretIndex=
// is backed by a VANILLA array; Antares only hooks this to serve weapon
// indices >= TechnoTypeClass::MaxWeapons (18) out of its own overflow vector.
// We call through the address rather than reading +0x814 directly so that
// Antares' overflow handling is honored when Antares is loaded.
using GetWeaponTurretIndexFunc = int(__thiscall*)(TechnoTypeClass*, int);
static const auto CallGetWeaponTurretIndex =
	reinterpret_cast<GetWeaponTurretIndexFunc>(0x7178B0);

// The engine keeps CurrentTurretNumber for itself on gattling units (it encodes
// the gattling stage) and on charge-turret units (it encodes the charge
// animation frame — this is the SREF pattern). Antares' own SwitchGunner hook
// guards IsChargeTurret for exactly this reason. Writing it on those types
// would fight the engine every frame, so we never do.
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

			// Indices holds one more entry than bands. A short list falls back
			// to its last entry rather than reading out of bounds.
			if (!this->Turret_RangeIndices.empty())
			{
				const size_t slot = band < this->Turret_RangeIndices.size()
					? band
					: this->Turret_RangeIndices.size() - 1;
				return this->Turret_RangeIndices[slot];
			}

			// No explicit index list: bands map straight onto turret indices.
			return static_cast<int>(band);
		}
	}

	if (this->Turret_FollowWeapon.Get() && weaponIndex >= 0)
		return CallGetWeaponTurretIndex(pType, weaponIndex);

	return -1;
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

	this->Crew_Required.Read(exINI, pSection, "Crew.Required");
	this->Crew_MinOccupants.Read(exINI, pSection, "Crew.MinOccupants");
	this->Crew_ROFPerOccupant.Read(exINI, pSection, "Crew.ROFPerOccupant");
	this->Crew_ROFMaxOccupants.Read(exINI, pSection, "Crew.ROFMaxOccupants");
}

template <typename T>
void TechnoTypeExt::ExtData::Serialize(T& Stm)
{
	Stm
		.Process(this->Turret_FollowWeapon)
		.Process(this->Turret_RangeBands)
		.Process(this->Turret_RangeIndices)
		.Process(this->Crew_Required)
		.Process(this->Crew_MinOccupants)
		.Process(this->Crew_ROFPerOccupant)
		.Process(this->Crew_ROFMaxOccupants)
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
// against Phobos develop). map-mode container: TryAllocate on ctor, Remove on
// dtor, Prepare/Static on save/load, LoadFromINI on the type's INI read.
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
