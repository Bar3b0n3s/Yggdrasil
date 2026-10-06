#include "EnginePCH.h"
#include "Engine/Platform/ErrorDialog.h"

// M5 contract stub (Roadmap rule 3): stream A (loader, device, selection, creation wrappers, host image upload) implements
// the dialog with MessageBoxW (MB_OK | MB_ICONERROR | MB_TOPMOST). Until then no dialog is shown.

#if defined(ENGINE_PLATFORM_WINDOWS)

namespace Engine {

	bool ShowErrorDialog(std::string_view /*title*/, std::string_view /*message*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

}

#endif
