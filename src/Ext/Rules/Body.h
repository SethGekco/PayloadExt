#pragma once

#include <RulesClass.h>
#include <Utilities/TemplateDef.h>

// A deliberately tiny stand-in for Phobos's RulesExt: we only need a few
// [General] defaults. Plain singleton (there is exactly one RulesClass and these
// are read once at rules load), so it claims no offset and cannot collide with
// Antares/Phobos extension storage.
class RulesExt
{
public:
	struct ExtData
	{
		// Default for InfantryType `Occupier.RA2Mode=`. Vanilla `Occupier=` is
		// only set on E1/E2/INIT, which would otherwise make RA2-mode garrisons
		// unusable for every other infantry type. Defaults to YES so RA2 mode
		// "just works"; set to no in [General] to opt in per type instead.
		Valueable<bool> RA2Garrison_OccupierDefault { true };
	};

	static ExtData Data;

	static ExtData* Global() { return &Data; }

	static void LoadBeforeTypeData(RulesClass* pThis, CCINIClass* pINI);
};
