#include "EnginePCH.h"
#include "Engine/Platform/SharedLibrary.h"

// SharedLibrary on Linux and macOS (SharedLibrary.h): no backend. The owner dropped the native module's Linux and macOS half
// (C-NATIVE-POSIX, Docs/Decisions/0028-backlog-program.md decision 10: "Windows backends only"), so Load refuses with
// Unsupported naming the platform, as ResourcePatcher::Patch does there, and the editor treats a module as not loaded
// (Docs/Decisions/0030-m19-contract.md decision 34). No library is ever loaded, so nothing is unloaded either.

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

	#include "Engine/Core/Error.h"
	#include "Engine/Platform/Private/PathsUtf8.h"

	#include <format>

namespace Engine {

	namespace Utils {

		// The platform the refusal names.
		static std::string_view GetSharedLibraryPlatformName()
		{
	#if defined(ENGINE_PLATFORM_MACOS)
			return "macOS";
	#else
			return "Linux";
	#endif
		}

	}

	Status SharedLibrary::CheckSupported()
	{
		return std::unexpected(Error(ErrorCode::Unsupported,
			std::format("native modules are built, loaded and exported on Windows only, not on {}", Utils::GetSharedLibraryPlatformName()))
				.WithHint("build, load and export a game with a Native/ folder on Windows; a game without one runs everywhere"));
	}

	SharedLibrary::~SharedLibrary() = default;

	Result<Scope<SharedLibrary>> SharedLibrary::Load(const std::filesystem::path& path)
	{
		return std::unexpected(Error(ErrorCode::Unsupported,
			std::format("cannot load '{}': native modules load on Windows only, not on {}", Utils::NativePathToUtf8(path), Utils::GetSharedLibraryPlatformName()))
				.WithHint("build, load and export a game with a Native/ folder on Windows; a game without one runs everywhere"));
	}

	Result<void*> SharedLibrary::FindSymbol(std::string_view name) const
	{
		return std::unexpected(Error(ErrorCode::Unsupported,
			std::format("cannot resolve '{}' in '{}': native modules load on Windows only, not on {}", name, Utils::NativePathToUtf8(m_Path),
				Utils::GetSharedLibraryPlatformName())));
	}

}

#endif
