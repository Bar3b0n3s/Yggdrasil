#include "EnginePCH.h"
#include "Engine/Asset/SourceFormats.h"

namespace Engine {

	std::span<const SourceFormat> GetSourceFormats()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	const SourceFormat* FindSourceFormat(std::string_view /*extension*/)
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

}
