#include "TestsPCH.h"

#include "Engine/Asset/SourceFormats.h"

#include "Engine/AssetPipeline/ImporterRegistry.h"

#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

namespace Engine {

	TEST_SUITE("Asset")
	{
		// ADR 0022 decision 5: the Asset-layer table equals RegisterBuiltinImporters, so gate 9 knows the importable source
		// formats in exported Release and Dist runs, where AssetPipeline does not exist.
		TEST_CASE("SourceFormats: the table equals the built-in importers and marks only refused extensions")
		{
			ImporterRegistry importers;
			RegisterBuiltinImporters(importers);
			std::vector<std::tuple<std::string, std::string, AssetType>> registered;
			for (const IAssetImporter* importer : importers.GetImporters())
				for (const std::string_view extension : importer->GetExtensions())
					registered.emplace_back(std::string(importer->GetId()), std::string(extension), importer->GetMainType());
			std::ranges::sort(registered);

			std::vector<std::tuple<std::string, std::string, AssetType>> table;
			std::vector<std::string> refused;
			for (const SourceFormat& format : GetSourceFormats())
			{
				table.emplace_back(std::string(format.Importer), std::string(format.Extension), format.MainType);
				if (!format.Importable)
					refused.emplace_back(format.Extension);
			}
			CHECK(std::ranges::is_sorted(table));
			CHECK(table == registered);
			CHECK(refused == std::vector<std::string>{ ".exr" });

			const SourceFormat* jpeg = FindSourceFormat(".JPEG");
			REQUIRE(jpeg != nullptr);
			CHECK(jpeg->Importer == "Texture");
			CHECK(jpeg->Extension == ".jpeg");
			CHECK(FindSourceFormat(".txt") == nullptr);
			CHECK(FindSourceFormat("png") == nullptr);
		}
	}

}
