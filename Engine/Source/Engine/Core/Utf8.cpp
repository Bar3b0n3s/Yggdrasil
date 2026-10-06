#include "EnginePCH.h"
#include "Engine/Core/Utf8.h"

// M1 contract stub (Roadmap rule 3): stream C implements UTF-8 validation. Until then nothing validates.

namespace Engine {

	bool IsValidUtf8(std::string_view /*text*/)
	{
		return false;
	}

	size_t FindInvalidUtf8(std::string_view /*text*/)
	{
		return 0;
	}

}
