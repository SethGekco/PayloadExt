#include "Body.h"

#include <Utilities/Macro.h>

#include <Ext/TechnoType/Body.h>

RulesExt::ExtData RulesExt::Data {};

void RulesExt::LoadBeforeTypeData(RulesClass* pThis, CCINIClass* pINI)
{
	INI_EX exINI(pINI);

	char key[0x40];

	for (int i = 1; i < 3; ++i)
	{
		_snprintf_s(key, _TRUNCATE, "Occupier%s.Default",
			TechnoTypeExt::OccupyClassKeys[i]);
		Data.OccupierClassDefault[i].Read(exINI, "General", key);
	}
}

// Rules load hook — 0x679A15 (RulesData_LoadBeforeTypeData), verified in Phobos.
// ECX = RulesClass*, [ESP+0x4] = CCINIClass*. Must run before type data so the
// default is known by the time InfantryTypes are parsed.
DEFINE_HOOK(0x679A15, RulesData_LoadBeforeTypeData_PayloadExt, 0x6)
{
	GET(RulesClass*, pItem, ECX);
	GET_STACK(CCINIClass*, pINI, 0x4);

	RulesExt::LoadBeforeTypeData(pItem, pINI);

	return 0;
}

// ---------------------------------------------------------------------------
// RulesClass::LoadAfterTypeData @0x679CAF (5 stolen; Phobos hooks the same
// address as RulesData_LoadAfterTypeData and returns 0, so we chain). ECX =
// RulesClass*, [ESP+0x4] = CCINIClass*.
//
// This is the earliest point at which BOTH BuildingTypes and InfantryTypes have
// been parsed, which is what the synthesis pass needs: it decides, per infantry
// type, whether any building with an occupancy policy admits it.
//
// Deliberately at load and nowhere else. It is the only write this DLL performs
// outside a decision, it happens before the first frame, and it derives purely
// from rules data -- so every machine computes the same result and it cannot
// desync. The same edit made at cursor time could not be safe: Occupier= lives on
// the TYPE and is shared by all players, while cursor position is per-machine.
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x679CAF, RulesData_LoadAfterTypeData_PayloadExt, 0x5)
{
	TechnoTypeExt::SynthesiseOccupiers();
	return 0;
}
