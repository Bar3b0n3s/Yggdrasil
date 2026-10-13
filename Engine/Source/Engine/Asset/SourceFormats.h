#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"

#include <span>
#include <string_view>

// The source formats the editor's importers accept (Architecture §7.4, §15.6 gate 9), frozen by the M14 contract
// (Docs/Decisions/0022-m14-contract.md decision 5). The importers live in AssetPipeline, which exported Release and Dist
// builds do not have, but gate 9 is evaluated in every mode, so this table states the same set in the Asset layer:
// one row per importer id and extension. A unit test keeps it equal to RegisterBuiltinImporters' importers (ids,
// extensions and main types), so neither list can drift; FileFormats.md is generated from the registry and reads
// Importable from here. Pure, immutable and thread-safe.

namespace Engine {

	struct SourceFormat
	{
		std::string_view Importer{};          // the importer id, as in .meta files: "Texture"
		std::string_view Extension{};         // lower case with the dot: ".jpeg"
		AssetType MainType = AssetType::None; // the importer's main asset type
		// False for an extension an importer claims only to refuse it with a hint (.exr: EnvironmentImporter fails with
		// "OpenEXR is not supported"). Gate 9 requires a cooked and loaded fixture for exactly the importable rows.
		bool Importable = true;
	};

	// Every row, sorted by (Importer, Extension) byte-wise.
	[[nodiscard]] std::span<const SourceFormat> GetSourceFormats();

	// The row of `extension` (with the dot, ASCII case-insensitive; extensions are unique across importers), or nullptr.
	[[nodiscard]] const SourceFormat* FindSourceFormat(std::string_view extension);

}
