#include "TestsPCH.h"

#include "Engine/Asset/AssetDiagnostic.h"

#include <algorithm>

namespace Engine {

	TEST_SUITE("Asset")
	{
		TEST_CASE("AssetDiagnostic: the codes follow the §13.7 spelling and are unique")
		{
			const std::span<const std::string_view> codes = GetAssetDiagnosticCodes();
			REQUIRE(codes.size() == 12);
			CHECK(codes.front() == "ASSET_MISSING");
			CHECK(codes.back() == AssetUploadFailedCode);
			for (const std::string_view code : codes)
			{
				CAPTURE(std::string(code));
				CHECK(std::ranges::count(codes, code) == 1);
				CHECK(std::ranges::all_of(code, [](char character)
				{
					return (character >= 'A' && character <= 'Z') || character == '_' || (character >= '0' && character <= '9');
				}));
			}
		}

		TEST_CASE("AssetDiagnostic: ToString names the code, the path, the message and the hint" * doctest::skip(true))
		{
			const AssetDiagnostic diagnostic{
				.Severity = DiagnosticSeverity::Error,
				.Code = std::string(AssetDuplicateHandleCode),
				.Asset = AssetHandle(0x3c9f2e7a11d04b88ull),
				.Path = "Assets/Copy.png.meta",
				.Message = "duplicate handle 3c9f2e7a11d04b88 (also in Assets/Wood.png.meta)",
				.Hint = "project.validate {fix: true} gives the copy a fresh handle",
				.Subject = "Assets/Wood.png.meta",
				.AutoFixable = true,
			};
			CHECK(AssetDiagnosticToString(diagnostic)
				== "ASSET_DUPLICATE_HANDLE Assets/Copy.png.meta: duplicate handle 3c9f2e7a11d04b88 (also in Assets/Wood.png.meta) "
				   "(hint: project.validate {fix: true} gives the copy a fresh handle)");
		}
	}

}
