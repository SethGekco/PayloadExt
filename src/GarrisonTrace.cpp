// TEMPORARY, REMOVABLE. See GarrisonTrace.h for why this exists and how it is
// designed to make ONE run sufficient.

#include "GarrisonTrace.h"

#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <CellClass.h>
#include <InfantryClass.h>
#include <InfantryTypeClass.h>
#include <MapClass.h>
#include <Unsorted.h>

#include <Utilities/Debug.h>

#include <map>

#include <Ext/TechnoType/Body.h>

namespace
{
	// Raw field reads. These offsets are all established elsewhere in this project
	// by disassembly; collected here so the tracer sees exactly what the engine
	// branches on rather than what YRpp thinks the layout is.
	void* FieldPtr(void* pObj, int offset)
	{
		return *reinterpret_cast<void**>(reinterpret_cast<BYTE*>(pObj) + offset);
	}

	BYTE FieldByte(void* pObj, int offset)
	{
		return *(reinterpret_cast<BYTE*>(pObj) + offset);
	}

	struct Sample
	{
		int Mission;        // MissionClass::CurrentMission  +0xAC
		int Queued;         // QueuedMission                 +0xB4
		void* Target;       // +0x2B4  (objective AND attack target)
		void* Destination;  // +0x5A4
		BYTE SeekGarrison;  // FootClass +0x691
		BYTE SeekBunker;    // FootClass +0x690
		BYTE InLimbo;       // ObjectClass +0x81
		int CellBuilding;   // 1 when standing on the destination building's cell

		bool operator==(const Sample& o) const
		{
			return Mission == o.Mission && Queued == o.Queued
				&& Target == o.Target && Destination == o.Destination
				&& SeekGarrison == o.SeekGarrison && SeekBunker == o.SeekBunker
				&& InLimbo == o.InLimbo && CellBuilding == o.CellBuilding;
		}
	};

	struct Tracked
	{
		int StartFrame;
		int Lines;
		bool HasPrev;
		Sample Prev;
		// RETIRED, not erased. Activate() is called every frame for any unit
		// near a governed building, so ERASING an exhausted unit handed it
		// straight back to Activate() with a fresh budget and a fresh
		// TRACK-START. That loop produced 117,746 TRACK-STARTs and an 848 MB
		// debug.log in one session: the per-unit budget was never a limit,
		// only a period. A retired entry must therefore stay in the map.
		bool Done;
	};

	std::map<InfantryClass*, Tracked> Units;

	// Absolute backstop, deliberately generous. This is NOT the global budget
	// the header warns about - that one was small enough to starve the unit
	// under test. This only exists so no future bug in the retirement logic can
	// cost another 848 MB; a normal diagnostic run never approaches it.
	constexpr int LinesTotalMax = 20000;
	int LinesTotal = 0;
	bool WarnedTotal = false;

	bool BudgetExhausted()
	{
		if (LinesTotal < LinesTotalMax)
			return false;
		if (!WarnedTotal)
		{
			WarnedTotal = true;
			Debug::Log("[PLXTRACE] total line budget (%d) reached - tracing off for "
				"the rest of this run\n", LinesTotalMax);
		}
		return true;
	}

	// Per unit, so a common type cannot starve a rare one.
	constexpr int LinesPerUnit = 45;
	// Long enough to cover hover -> order -> walk -> entry, then forgotten.
	constexpr int TrackFrames = 900;

	const char* MissionName(int m)
	{
		switch (m)
		{
		case -1: return "None";
		case 0:  return "Sleep";
		case 1:  return "Attack";
		case 2:  return "Move";
		case 5:  return "Guard";
		case 7:  return "Enter";
		case 8:  return "Capture";
		case 11: return "AreaGuard";
		case 13: return "Stop";
		case 15: return "Hunt";
		default: return "?";
		}
	}

	const char* NameOf(void* pAbstract)
	{
		if (!pAbstract)
			return "-";

		const auto pBld = abstract_cast<BuildingClass*>(
			static_cast<AbstractClass*>(pAbstract));
		return (pBld && pBld->Type) ? pBld->Type->ID : "?";
	}

	Sample Read(InfantryClass* pInfantry)
	{
		Sample s {};
		s.Mission = *reinterpret_cast<int*>(reinterpret_cast<BYTE*>(pInfantry) + 0xAC);
		s.Queued = *reinterpret_cast<int*>(reinterpret_cast<BYTE*>(pInfantry) + 0xB4);
		s.Target = FieldPtr(pInfantry, 0x2B4);
		s.Destination = FieldPtr(pInfantry, 0x5A4);
		s.SeekGarrison = FieldByte(pInfantry, 0x691);
		s.SeekBunker = FieldByte(pInfantry, 0x690);
		s.InLimbo = FieldByte(pInfantry, 0x81);

		// The test UpdatePosition actually performs at 0x5196CD: is the building on
		// my current cell the same object as my Destination? This is the single
		// condition that decides whether arrival turns into entry, so it is worth
		// sampling directly rather than inferring from coordinates.
		s.CellBuilding = 0;
		if (s.Destination)
		{
			if (const auto pCell = MapClass::Instance.TryGetCellAt(pInfantry->Location))
				s.CellBuilding = (pCell->GetBuilding() == s.Destination) ? 1 : 0;
		}

		return s;
	}

	void Emit(InfantryClass* pInfantry, Tracked& t, const Sample& s, const char* pWhy)
	{
		++t.Lines;
		++LinesTotal;
		Debug::Log("[PLXTRACE] f%d %s@%p %s: mission=%s queued=%s target=%s dest=%s "
			"seekGarrison=%d seekBunker=%d limbo=%d onDestCell=%d\n",
			Unsorted::CurrentFrame - t.StartFrame,
			pInfantry->Type ? pInfantry->Type->ID : "?", (void*)pInfantry, pWhy,
			MissionName(s.Mission), MissionName(s.Queued),
			NameOf(s.Target), NameOf(s.Destination),
			(int)s.SeekGarrison, (int)s.SeekBunker, (int)s.InLimbo, s.CellBuilding);
	}
}

namespace GarrisonTrace
{
	void Activate(InfantryClass* pInfantry, BuildingClass* pBuilding)
	{
		if (!pInfantry || !pInfantry->Type || !pBuilding || !pBuilding->Type)
			return;

		// Only units connected with a building WE govern, so an ordinary game does
		// not get traced at all.
		const auto pExt = TechnoTypeExt::ExtMap.Find(pBuilding->Type);
		if (!pExt || !pExt->HasOccupancyPolicy())
			return;

		const auto it = Units.find(pInfantry);
		if (it != Units.end())
			return; // already tracked; do not reset the clock or the budget

		if (BudgetExhausted())
			return;

		Units[pInfantry] = Tracked { Unsorted::CurrentFrame, 0, false, Sample {}, false };
		++LinesTotal;

		Debug::Log("[PLXTRACE] f0 %s@%p TRACK-START: asked about %s\n",
			pInfantry->Type->ID, (void*)pInfantry, pBuilding->Type->ID);
	}

	void Tick(TechnoClass* pThis)
	{
		if (Units.empty() || !pThis)
			return;

		const auto pInfantry = abstract_cast<InfantryClass*>(pThis);
		if (!pInfantry)
			return;

		const auto it = Units.find(pInfantry);
		if (it == Units.end())
			return;

		auto& t = it->second;
		if (t.Done)
			return;

		// Retire in place. Erasing here let Activate() re-add the same unit next
		// frame with a fresh budget, so this was an endless cycle rather than a
		// cap - see the note on Tracked::Done.
		if (Unsorted::CurrentFrame - t.StartFrame > TrackFrames
			|| t.Lines >= LinesPerUnit
			|| BudgetExhausted())
		{
			t.Done = true;
			return;
		}

		const auto s = Read(pInfantry);

		// The whole point: only transitions are interesting.
		if (t.HasPrev && s == t.Prev)
			return;

		t.Prev = s;
		t.HasPrev = true;
		Emit(pInfantry, t, s, "state");
	}

	void Event(InfantryClass* pInfantry, const char* pWhat, BuildingClass* pBuilding)
	{
		if (!pInfantry || !pWhat)
			return;

		const auto it = Units.find(pInfantry);
		if (it == Units.end())
			return;

		auto& t = it->second;
		if (t.Done || t.Lines >= LinesPerUnit || BudgetExhausted())
			return;

		const auto s = Read(pInfantry);
		t.Prev = s;
		t.HasPrev = true;

		// Events are always emitted, even when nothing changed, because knowing a
		// hook RAN is as informative as the state it ran with -- that distinction
		// is what several earlier rounds could not make.
		++t.Lines;
		++LinesTotal;
		Debug::Log("[PLXTRACE] f%d %s@%p EVENT %s(%s): mission=%s queued=%s "
			"target=%s dest=%s seekGarrison=%d limbo=%d onDestCell=%d\n",
			Unsorted::CurrentFrame - t.StartFrame,
			pInfantry->Type ? pInfantry->Type->ID : "?", (void*)pInfantry, pWhat,
			(pBuilding && pBuilding->Type) ? pBuilding->Type->ID : "-",
			MissionName(s.Mission), MissionName(s.Queued),
			NameOf(s.Target), NameOf(s.Destination),
			(int)s.SeekGarrison, (int)s.InLimbo, s.CellBuilding);
	}
}
