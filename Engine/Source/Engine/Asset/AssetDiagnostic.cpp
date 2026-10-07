#include "EnginePCH.h"
#include "Engine/Asset/AssetDiagnostic.h"

#include <array>
#include <format>

namespace Engine {

	std::span<const std::string_view> GetAssetDiagnosticCodes()
	{
		// §13.7 order (with ASSET_CONTENT_SKIPPED beside the other glTF warnings), then the runtime-only code.
		static constexpr std::array<std::string_view, 12> Codes = {
			AssetMissingCode,
			AssetTypeMismatchCode,
			AssetImportFailedCode,
			AssetOrphanMetaCode,
			AssetDuplicateHandleCode,
			AssetOrphanDependencyCode,
			AssetTangentsApproximatedCode,
			AssetUnsupportedUvSetCode,
			AssetVertexColorsIgnoredCode,
			AssetContentSkippedCode,
			PathCaseMismatchCode,
			AssetUploadFailedCode,
		};
		return Codes;
	}

	std::string AssetDiagnosticToString(const AssetDiagnostic& diagnostic)
	{
		std::string text = diagnostic.Path.empty() ? std::format("{}: {}", diagnostic.Code, diagnostic.Message)
												   : std::format("{} {}: {}", diagnostic.Code, diagnostic.Path, diagnostic.Message);
		if (!diagnostic.Hint.empty())
			text += std::format(" (hint: {})", diagnostic.Hint);
		return text;
	}

}
