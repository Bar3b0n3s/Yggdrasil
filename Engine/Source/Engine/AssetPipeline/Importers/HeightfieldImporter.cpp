#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/HeightfieldImporter.h"

#include <array>

// The heightfield importer and the heightmap conversion (HeightfieldImporter.h). The extension list is the contract's; the
// conversion and the import are contract stubs of M26's C26.1, implemented by stream T-A (Docs/Decisions/0037-m26-contract.md
// decision 20), which also registers the importer.

namespace Engine {

	namespace Utils {

		constexpr std::array<std::string_view, 1> HeightfieldExtensions = { ".heightfield" };

	}

	Result<HeightfieldData> ConvertHeightmap(std::span<const std::byte> /*source*/, HeightmapSourceKind /*kind*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M26 terrain contract (Docs/Decisions/0037-m26-contract.md decision 20, stream T-A)");
	}

	std::span<const std::string_view> HeightfieldImporter::GetExtensions() const
	{
		return Utils::HeightfieldExtensions;
	}

	Result<ImportResult> HeightfieldImporter::Import(ImportContext& /*context*/, const AssetMetadata& /*metadata*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M26 terrain contract (Docs/Decisions/0037-m26-contract.md decision 20, stream T-A)");
	}

}
