#include "EnginePCH.h"
#include "Engine/Platform/PlatformWindowSupport.h"

// Platform windows on Linux and macOS: none. Windows backends only (ADR 0028 decision 10): CheckPlatformWindowSupport refuses
// with Unsupported naming the platform, as ResourcePatcher::Patch and NativeFileDialog::Open do here, so ImGuiLayer keeps
// ImGuiConfigFlags_ViewportsEnable cleared and every panel stays docked in the main window
// (Docs/Decisions/0035-m24-contract.md decision 17).

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

	#include "Engine/Core/Error.h"

	#include <format>
	#include <string_view>

namespace Engine {

	Status CheckPlatformWindowSupport()
	{
	#if defined(ENGINE_PLATFORM_MACOS)
		constexpr std::string_view Platform = "macOS";
	#else
		constexpr std::string_view Platform = "Linux";
	#endif
		return std::unexpected(Error(ErrorCode::Unsupported, std::format("{} has no platform windows: only Windows has a backend", Platform))
				.WithHint("every panel stays docked in the editor's main window"));
	}

}

#endif
