#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/EnvironmentImporter.h"

#include "Engine/Core/Error.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <array>

namespace Engine {

	std::span<const std::string_view> EnvironmentImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 2> Extensions = { ".hdr", ".exr" };
		return Extensions;
	}

	Result<ImportResult> EnvironmentImporter::Import(ImportContext& /*context*/, const AssetMetadata& /*metadata*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "the environment importer is not implemented yet (M8 stream B)")
				.WithHint("start the editor with a GPU once to bake this environment"));
	}

	void EnvironmentImporter::RegisterTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
