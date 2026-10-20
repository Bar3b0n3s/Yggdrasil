#include "EnginePCH.h"
#include "Engine/Platform/SharedLibrary.h"

#include <utility>

// The host-independent half of SharedLibrary (SharedLibrary.h): the OS halves load, resolve and unload.

namespace Engine {

	SharedLibrary::SharedLibrary(ConstructionKey /*key*/, std::filesystem::path path, void* handle)
		: m_Path(std::move(path)), m_Handle(handle)
	{
	}

}
