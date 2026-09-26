#include "PayloadExt.h"

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

#include <psapi.h>

// Linked here rather than in the three per-configuration AdditionalDependencies
// lists in the vcxproj: this is the only translation unit that needs it, and on
// current SDKs psapi.lib is a thin forwarder to kernel32's K32* exports.
#pragma comment(lib, "psapi.lib")

namespace
{
	// Dump every loaded module with its base and size, once, at startup.
	//
	// WHY THIS EARNS ITS PLACE — 2026-09-25, snapshot-20260925-174303.
	// That crash faulted with EIP outside gamemd's image and had five return
	// addresses sitting in one injected DLL, which normally names the culprit in
	// one step. It could not be attributed at all, because:
	//
	//   1. extcrashdump.dmp came out 0 BYTES, so there was no ModuleList stream
	//      and no memory to read a Syringe stub's `push <origin>` out of.
	//   2. The snapshot carries no syringe.log.
	//   3. Wine does NOT place these DLLs at stable bases. Proven across two
	//      consecutive runs: PayloadExt was at 0x773F0000 on 09-24 and
	//      0x77380000 on 09-25, and the whole table reshuffles with it — so
	//      yesterday's module list cannot be borrowed to read today's crash.
	//
	// Ares writes except.txt (registers + a 1KB stack dump) even when the
	// minidump fails, and debug.log is always present. Twenty-odd lines here
	// make that pair self-sufficient: every code address in any future crash
	// resolves to module+offset without needing the dump to have survived.
	//
	// EnumProcessModules is in psapi, which the game already links via Syringe's
	// host process; if it is ever unavailable this degrades to the one-line
	// self-report below rather than failing the hook.
	void LogModuleTable()
	{
		HMODULE mods[256];
		DWORD needed = 0;

		if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
		{
			Debug::Log("[" PAYLOADEXT_NAME "] module table unavailable"
				" (EnumProcessModules failed, error %u)\n", GetLastError());
			return;
		}

		const int count = static_cast<int>(needed / sizeof(HMODULE));
		Debug::Log("[" PAYLOADEXT_NAME "] === module table (%d loaded) ===\n",
			count);

		for (int i = 0; i < count && i < 256; ++i)
		{
			MODULEINFO info = {};
			if (!GetModuleInformation(GetCurrentProcess(), mods[i],
					&info, sizeof(info)))
				continue;

			char path[MAX_PATH] = {};
			if (!GetModuleFileNameA(mods[i], path, MAX_PATH))
				continue;

			const char* const slash = strrchr(path, '\\');
			const DWORD base = static_cast<DWORD>(
				reinterpret_cast<size_t>(info.lpBaseOfDll));

			Debug::Log("[" PAYLOADEXT_NAME "] MODULE %08X - %08X  %s\n",
				base, base + info.SizeOfImage, slash ? slash + 1 : path);
		}

		Debug::Log("[" PAYLOADEXT_NAME "] === end module table ===\n");
	}
}

// WinMain startup hook (verified in Phobos: 0x6BD68D runs once at game start,
// alongside the engine's own factory registrations). For Phase 0 this is just a
// build-stamp probe so a debug.log unambiguously identifies which PayloadExt
// build is loaded, and confirms Syringe successfully injected the DLL. Feature
// hooks are added under src/Hooks.* / src/Ext as each phase lands.
DEFINE_HOOK(0x6BD68D, PayloadExt_WinMain_Startup, 0x6)
{
	Debug::Log("[" PAYLOADEXT_NAME "] Module base: 0x%X\n",
		(unsigned int)(size_t)GetModuleHandleA(PAYLOADEXT_NAME ".dll"));
	Debug::Log("[" PAYLOADEXT_NAME "] Build: " __DATE__ " " __TIME__ "\n");

	// Crash-triage groundwork, not a PayloadExt feature. See LogModuleTable.
	LogModuleTable();

	return 0;
}
