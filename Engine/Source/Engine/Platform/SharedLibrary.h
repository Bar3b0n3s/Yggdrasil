#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <filesystem>
#include <string_view>

// A shared library loaded into the process (Docs/Decisions/0030-m19-contract.md decision 31, C-NATIVE-DLL): the editor loads a
// game's native module through it, and hot-reloads it by loading a newly built library and releasing the old one. Windows
// backends only (ADR 0028 decision 10): Platform/Windows/SharedLibraryWindows.cpp loads with LoadLibraryExW and resolves
// with GetProcAddress; Platform/Posix/SharedLibraryPosix.cpp has no backend and refuses with Unsupported naming the
// platform, as ResourcePatcher::Patch does there, so on Linux and macOS the editor treats a module as not loaded. Exported
// games never load a library: their module is linked in (decision 30).

namespace Engine {

	// One loaded library. Not copyable or movable (symbols point into it); destroying it unloads the library, so whoever
	// holds a symbol keeps the library alive (NativeModuleTypes' keepalive). Thread-safe to read; Load and destruction on any
	// thread, but the native host loads and releases only on the main thread, while no play session runs (decision 31).
	class SharedLibrary
	{
	public:
		// Restricts construction to Load; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class SharedLibrary;
		};

		// Use Load.
		SharedLibrary(ConstructionKey key, std::filesystem::path path, void* handle);
		~SharedLibrary();

		SharedLibrary(const SharedLibrary&) = delete;
		SharedLibrary& operator=(const SharedLibrary&) = delete;

		// Whether this platform has a backend (Docs/Decisions/0030-m19-contract.md decision 34): success on Windows, and on
		// Linux and macOS Unsupported naming the platform, the error every native capability there reports (the build, the
		// load, the export of a game with Native/), so they all refuse through this one platform-layer path.
		[[nodiscard]] static Status CheckSupported();

		// Loads the library at `path` (absolute) and the libraries it imports from its own directory and the system's (never
		// the working directory or PATH). Errors: InvalidArgument for a relative path; NotFound when no file is there; Io for a
		// file the system cannot load as a library, naming it and the system error; Unsupported on Linux and macOS, naming the
		// platform ("native modules load on Windows only").
		[[nodiscard]] static Result<Scope<SharedLibrary>> Load(const std::filesystem::path& path);

		// The address of the exported function or data `name` (an extern "C" name). Errors: NotFound naming the library.
		[[nodiscard]] Result<void*> FindSymbol(std::string_view name) const;

		[[nodiscard]] const std::filesystem::path& GetPath() const { return m_Path; }
	private:
		std::filesystem::path m_Path{};
		void* m_Handle = nullptr; // the system's module handle (HMODULE)
	};

}
