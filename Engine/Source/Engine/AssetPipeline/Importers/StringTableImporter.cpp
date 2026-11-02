#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/StringTableImporter.h"

#include "Engine/Asset/StringTableData.h"
#include "Engine/AssetPipeline/Private/ImportErrors.h"

#include <array>
#include <format>
#include <utility>

namespace Engine {

	std::span<const std::string_view> StringTableImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 1> Extensions = { ".stringtable" };
		return Extensions;
	}

	Result<ImportResult> StringTableImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		const std::string sourcePath = context.GetSourcePath().ToString();

		// Strict: an unknown member is an error at import, so a misspelled key never silently keeps its default.
		StringTableLoadReport report;
		Result<StringTableData> table = StringTableFromText(AsStringView(context.GetSourceBytes()), context.GetRegistry(), report, true);
		if (!table.has_value())
			return std::unexpected(Utils::MakeImportFailed(table.error(), sourcePath, std::format("while importing string table '{}'", sourcePath)));

		ImportResult result;
		result.Artifacts.push_back(
			ImportedArtifact{ .Handle = metadata.Handle, .Type = AssetType::StringTable, .SubAssetKey = {}, .Cooked = CookStringTable(*table, Version) });
		// A table references no asset (GetStringTableDependencies), set explicitly as every importer's dependencies are.
		result.Dependencies = GetStringTableDependencies(*table);
		return result;
	}

}
