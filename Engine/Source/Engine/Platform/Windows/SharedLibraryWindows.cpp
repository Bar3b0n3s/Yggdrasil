#include "EnginePCH.h"
#include "Engine/Platform/SharedLibrary.h"

// SharedLibrary on Windows (SharedLibrary.h; Docs/Decisions/0030-m19-contract.md decision 31): LoadLibraryExW with
// LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, GetProcAddress and FreeLibrary, which stream N-C
// implements.

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include "Engine/Core/Error.h"
	#include "Engine/Platform/Private/PathsUtf8.h"

	#include <format>

namespace Engine {

	Status SharedLibrary::CheckSupported()
	{
		return {};
	}

	SharedLibrary::~SharedLibrary()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<Scope<SharedLibrary>> SharedLibrary::Load(const std::filesystem::path& path)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported,
			std::format("loading the library '{}' (Docs/Decisions/0030-m19-contract.md decision 31, stream N-C)", Utils::NativePathToUtf8(path))));
	}

	Result<void*> SharedLibrary::FindSymbol(std::string_view name) const
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported,
			std::format("resolving '{}' in '{}' (Docs/Decisions/0030-m19-contract.md decision 31, stream N-C)", name, Utils::NativePathToUtf8(m_Path))));
	}

}

#endif
