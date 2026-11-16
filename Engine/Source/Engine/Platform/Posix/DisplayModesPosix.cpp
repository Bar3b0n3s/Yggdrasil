#include "EnginePCH.h"
#include "Engine/Platform/DisplayModes.h"

// The display modes on Linux and macOS (Docs/Decisions/0043-m28-game-services.md decision 3): no backend, as
// Docs/Decisions/0028-backlog-program.md decision 10 asks for a new platform capability, so both calls report Unsupported
// (PlatformWindowSupportPosix.cpp's precedent).

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

	#include "Engine/Core/Error.h"

	#include <format>

namespace Engine {

	namespace Utils {

	#if defined(ENGINE_PLATFORM_MACOS)
		constexpr std::string_view DisplayPlatform = "macOS";
	#else
		constexpr std::string_view DisplayPlatform = "Linux";
	#endif

		[[nodiscard]] static Error RefuseDisplayModes()
		{
			return Error(ErrorCode::Unsupported, std::format("{} has no display mode switching: only Windows has a backend", DisplayPlatform))
				.WithHint("the window keeps the project's Window settings");
		}

	}

	Result<std::vector<DisplayVideoMode>> QueryDisplayVideoModes(const Window& /*window*/)
	{
		return std::unexpected(Utils::RefuseDisplayModes());
	}

	Result<DisplayVideoMode> QueryCurrentVideoMode(const Window& /*window*/)
	{
		return std::unexpected(Utils::RefuseDisplayModes());
	}

	Status ApplyDisplaySettings(Window& /*window*/, const DisplaySettings& /*settings*/)
	{
		return std::unexpected(Utils::RefuseDisplayModes());
	}

}

#endif
