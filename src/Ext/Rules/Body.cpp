#include "Body.h"

#include <Utilities/Macro.h>

RulesExt::ExtData RulesExt::Data {};

void RulesExt::LoadBeforeTypeData(RulesClass* pThis, CCINIClass* pINI)
{
	INI_EX exINI(pINI);

	Data.RA2Garrison_OccupierDefault.Read(exINI, "General", "Occupier.RA2Mode.Default");
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
