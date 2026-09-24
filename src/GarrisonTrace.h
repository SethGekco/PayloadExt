// TEMPORARY, REMOVABLE: a single-run lifecycle tracer for garrison orders.
//
// Why this exists. Diagnosing "GGI will not enter the building" took many
// build-test rounds, each answering one question, and three of those rounds were
// wasted on the same two mistakes:
//
//   * a GLOBAL log budget, which E1 and GHOST exhausted before the unit under
//     test ever moved, so the interesting lines were never written;
//   * reading the ABSENCE of lines as evidence, in runs that could not support
//     that conclusion because no known-good case was captured alongside.
//
// So this tracer is built to make one run sufficient:
//
//   1. PER-UNIT budgets. Every tracked infantry gets its own allowance, so common
//      types cannot crowd out a rare one.
//   2. LOG ON CHANGE. A tracked unit is sampled every frame but only emits a line
//      when something it cares about actually changes, so the output is a timeline
//      of transitions rather than thousands of duplicates.
//   3. AUTOMATIC CONTROLS. Tracking is keyed on "was asked about a governed
//      building", which catches E1 and GHOST in the same run as GGI and SNIPE.
//      A working case is therefore always present to diff against.
//   4. DISCRETE EVENTS INTERLEAVED. Hook-level moments (admission verdict,
//      routing, arrival gate, entry, lapse) share the same prefix and frame
//      numbers as the sampled state, so cause and effect line up in one stream.
//
// Delete this file, its entry in the vcxproj, and the GarrisonTrace:: calls in
// Hooks.Occupancy.cpp / Hooks.OpenToppedBuilding.cpp to remove it entirely.

#pragma once

class TechnoClass;
class InfantryClass;
class BuildingClass;

namespace GarrisonTrace
{
	// Begin (or refresh) tracking for an infantry seen in connection with a
	// building we govern. Safe to call every frame; cheap once tracking exists.
	void Activate(InfantryClass* pInfantry, BuildingClass* pBuilding);

	// Sample one tracked unit. Called from the per-frame TechnoClass::Update hook
	// for every techno; returns immediately for anything untracked.
	void Tick(TechnoClass* pThis);

	// Record a named moment against the same timeline as the samples.
	void Event(InfantryClass* pInfantry, const char* pWhat, BuildingClass* pBuilding);
}
