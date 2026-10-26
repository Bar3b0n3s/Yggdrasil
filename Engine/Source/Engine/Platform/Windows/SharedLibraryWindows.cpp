#include "EnginePCH.h"
#include "Engine/Platform/SharedLibrary.h"

// SharedLibrary on Windows (SharedLibrary.h; Docs/Decisions/0030-m19-contract.md decision 31): LoadLibraryExW with
// LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, so a library's imports resolve from its own directory
// and the system's, never from the working directory or PATH; GetProcAddress; FreeLibrary.

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include "Engine/Core/Assert.h"
	#include "Engine/Core/Error.h"
	#include "Engine/Core/Log.h"
	#include "Engine/Platform/Private/PathsUtf8.h"

	#include <windows.h>

	#include <format>
	#include <string>
	#include <system_error>

namespace Engine {

	namespace Utils {

		// The system's text for a Win32 error code.
		[[nodiscard]] static std::string DescribeSharedLibraryError(DWORD code)
		{
			return std::format("{} (error {})", std::system_category().message(static_cast<int>(code)), code);
		}

		// Keeps the system from showing a critical-error dialog while the calling thread loads a library (a removable drive
		// that is not ready, a damaged image): the load fails with its error instead. Restores the thread's mode.
		class SharedLibraryErrorModeScope
		{
		public:
			SharedLibraryErrorModeScope()
			{
				m_Changed = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &m_Previous) != FALSE;
			}

			~SharedLibraryErrorModeScope()
			{
				if (m_Changed)
					SetThreadErrorMode(m_Previous, nullptr);
			}

			SharedLibraryErrorModeScope(const SharedLibraryErrorModeScope&) = delete;
			SharedLibraryErrorModeScope& operator=(const SharedLibraryErrorModeScope&) = delete;
		private:
			DWORD m_Previous = 0;
			bool m_Changed = false;
		};

	}

	Status SharedLibrary::CheckSupported()
	{
		return {};
	}

	SharedLibrary::~SharedLibrary()
	{
		if (m_Handle == nullptr)
			return;
		// Unloading cannot be refused for a library this object loaded; a failure leaves it mapped, which costs only memory.
		if (FreeLibrary(static_cast<HMODULE>(m_Handle)) == FALSE)
			ENGINE_CORE_WARN("Cannot unload the library '{}': {}", Utils::NativePathToUtf8(m_Path), Utils::DescribeSharedLibraryError(GetLastError()));
	}

	Result<Scope<SharedLibrary>> SharedLibrary::Load(const std::filesystem::path& path)
	{
		const std::string name = Utils::NativePathToUtf8(path);
		if (!path.is_absolute())
			return MakeError(ErrorCode::InvalidArgument, "cannot load the library '{}': its path is not absolute", name);
		std::error_code error;
		const bool regular = std::filesystem::is_regular_file(path, error);
		if (error && error != std::errc::no_such_file_or_directory)
			return MakeError(ErrorCode::Io, "cannot load the library '{}': {}", name, error.message());
		if (!regular)
			return MakeError(ErrorCode::NotFound, "cannot load the library '{}': no file is there", name);

		HMODULE handle = nullptr;
		{
			const Utils::SharedLibraryErrorModeScope errorMode;
			handle = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		}
		if (handle == nullptr)
		{
			const DWORD code = GetLastError();
			return std::unexpected(Error(ErrorCode::Io, std::format("cannot load the library '{}': {}", name, Utils::DescribeSharedLibraryError(code)))
					.WithHint("a library loads only when it was built for this platform and every library it imports is beside it or in the system's "
							  "directory; build it again"));
		}
		return CreateScope<SharedLibrary>(ConstructionKey{}, path, static_cast<void*>(handle));
	}

	Result<void*> SharedLibrary::FindSymbol(std::string_view name) const
	{
		ENGINE_CORE_ASSERT(m_Handle != nullptr, "SharedLibrary::FindSymbol on a library that is not loaded");
		// GetProcAddress takes a NUL-terminated name; one with an embedded NUL names nothing it exports.
		const std::string symbol(name);
		if (symbol.find('\0') == std::string::npos && !symbol.empty())
		{
			if (const FARPROC address = GetProcAddress(static_cast<HMODULE>(m_Handle), symbol.c_str()); address != nullptr)
				return reinterpret_cast<void*>(address);
		}
		return MakeError(ErrorCode::NotFound, "the library '{}' exports no '{}'", Utils::NativePathToUtf8(m_Path), name);
	}

}

#endif
