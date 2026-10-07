#include "EnginePCH.h"
#include "Engine/Platform/ErrorDialog.h"

// The error dialog on Windows: a modal MessageBoxW without an owner window, so it also works before a window exists or
// after the window failed.

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include "Engine/Platform/Private/PathsUtf8.h"

	#include <windows.h>

namespace Engine {

	bool ShowErrorDialog(std::string_view title, std::string_view message)
	{
		const Utils::NativeString wideTitle = Utils::NativeStringFromUtf8(title);
		const Utils::NativeString wideMessage = Utils::NativeStringFromUtf8(message);
		// MB_TOPMOST and MB_SETFOREGROUND keep a dialog without an owner from opening behind the application's windows.
		const int result = MessageBoxW(nullptr, wideMessage.c_str(), wideTitle.c_str(), MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
		return result != 0;
	}

}

#endif
