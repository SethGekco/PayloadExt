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
