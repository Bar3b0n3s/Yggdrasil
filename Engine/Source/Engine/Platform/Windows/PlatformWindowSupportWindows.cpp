#include "EnginePCH.h"
#include "Engine/Platform/PlatformWindowSupport.h"

// CheckPlatformWindowSupport on Windows (Docs/Decisions/0035-m24-contract.md decision 17): a windowed process, on GLFW's
// native Win32 platform, may show platform windows, which the vendored GLFW backend creates as GLFW windows; a headless one,
// on GLFW's null platform, has no OS windows, so its panels stay docked in the main window (headless editors, their
// editor.screenshot and the editor-UI goldens).

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include "Engine/Core/Error.h"
	#include "Engine/Platform/GlfwLibrary.h"

	#include <string>
	#include <string_view>

namespace Engine {

	namespace Utils {

		// Unsupported for `reason`, with the hint every refusal shares.
		[[nodiscard]] static Status RefusePlatformWindows(std::string_view reason)
		{
			return std::unexpected(Error(ErrorCode::Unsupported, std::string(reason)).WithHint("every panel stays docked in the editor's main window"));
		}

	}

	Status CheckPlatformWindowSupport()
	{
		switch (GlfwLibrary::GetPlatform())
		{
			case GlfwPlatform::Win32: return {};
			case GlfwPlatform::Null:  return Utils::RefusePlatformWindows("a headless process has no platform windows: GLFW's null platform has no OS windows");
			case GlfwPlatform::None:  return Utils::RefusePlatformWindows("platform windows need GLFW, which is not initialized");
			case GlfwPlatform::Cocoa:
			case GlfwPlatform::X11:   break;
		}
		return Utils::RefusePlatformWindows("platform windows need GLFW's Win32 platform");
	}

}

#endif
