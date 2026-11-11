#pragma once

#include "Engine/Asset/HeightfieldData.h"
#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Engine {

	// M26 (C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 20): the heightmap sources terrain.importHeightmap
	// converts into a heightfield (ADR 0028's phase-3 ledger: "import of 16-bit PNG or RAW, at most 1025 squared samples").
	enum class HeightmapSourceKind : uint8_t
	{
		// A 16-bit PNG (grey, grey and alpha, RGB or RGBA: the first channel), read through the vendored stb_image's 16-bit path
		// (stbi_load_16_from_memory); 8-bit PNGs are refused, since their 256 levels terrace a terrain.
		Png16,
		// Headerless little-endian uint16 samples, row 0 first, square: N^2 samples in 2 N^2 bytes.
		Raw16
	};

	// The heightfield of a heightmap source (external input): a PNG's image or a RAW's square, N x N with N in
	// [MinHeightfieldResolution, MaxHeightfieldResolution], each sample kept bit for bit (row 0 of the image is row 0, along
	// the terrain's +Z). Never asserts on data. Errors: InvalidArgument for a non-square image, a side outside the range, an
	// 8-bit PNG or a RAW whose byte count is not 2 N^2 for such an N (each naming the size found); Parse for a PNG stb_image
	// cannot read (its reason quoted). Pure; identical in every configuration. Stream T-A, with terrain.importHeightmap.
	[[nodiscard]] Result<HeightfieldData> ConvertHeightmap(std::span<const std::byte> source, HeightmapSourceKind kind);

	// .heightfield files (M26, C26.1, C-TERR; decision 20; Architecture §6, §7.4): ReadHeightfieldSource (Asset/
	// HeightfieldData.h), cooked unchanged with CookHeightfield. One artifact, the heightfield, under the source's handle; no
	// sub-assets, no settings and no dependencies (a heightfield reads only its own source, ADR 0028's phase-3 ledger's
	// importer rule). A pure function of the source bytes and the importer version. Registered by stream T-A
	// (RegisterBuiltinImporters, with its row in Asset/SourceFormats.cpp: the importer Heightfield, ".heightfield", main type
	// Heightfield, importable; and asset.create's Heightfield, which writes MakeFlatHeightfield(129)). Thread-safe.
	class HeightfieldImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "Heightfield";
		static constexpr uint32_t Version = 1;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::Heightfield; }
		// ".heightfield".
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }

		// Errors: ImportFailed (Utils::MakeImportFailed) keeping the message of ReadHeightfieldSource's Parse, Validation or
		// UnsupportedVersion; atomic on failure.
		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;
	};

}
