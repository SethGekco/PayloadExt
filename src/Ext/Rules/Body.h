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
		// Global default for the per-class InfantryType flags, i.e. what
		// Occupier.RA2= / Occupier.OpenTopped= mean when a type does not set
		// them. Unset -> fall back to that type's own vanilla Occupier=.
		// Index 0 (vanilla) is unused; it is the engine's own field.
		//   [General] Occupier.RA2.Default= / Occupier.OpenTopped.Default=
		Nullable<bool> OccupierClassDefault[3] {};
	};

	static ExtData Data;

	static ExtData* Global() { return &Data; }

	static void LoadBeforeTypeData(RulesClass* pThis, CCINIClass* pINI);
};
