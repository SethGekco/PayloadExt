#include "PayloadExt.h"

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

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
	return 0;
}
