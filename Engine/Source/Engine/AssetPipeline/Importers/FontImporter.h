#pragma once

#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Engine {

	class TypeRegistry;

	// Registry struct "FontImportSettings".
	struct FontImportSettings
	{
		float PixelSize = 48.0f; // atlas pixels per em (§7.4: 48 px); 16 to 128
		float Spread = 8.0f;     // SDF spread in atlas pixels (§7.4: spread 8); 1 to 32
	};

	// TTF and OTF (Architecture §7.4): an R8 signed-distance-field atlas of ASCII and Latin-1 (U+0020-U+007E and
	// U+00A0-U+00FF; codepoints the font lacks are left out) with glyph metrics and kerning, rasterized through
	// imstb_truetype.h from the vendored Dear ImGui (stb_truetype v1.26), compiled with STBTT_STATIC in the importer's own
	// translation unit only (Appendix C), so it never clashes with Dear ImGui's copy. Glyphs are packed deterministically
	// (sorted by codepoint, a fixed shelf packer into the smallest power-of-two square that fits). Cooked as one FontData
	// (CookFont). A font stb_truetype cannot read, or one without any glyph of the character set, is ImportFailed.
	class FontImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "Font";
		static constexpr uint32_t Version = 1;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::Font; }
		// ".ttf", ".otf".
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return "FontImportSettings"; }

		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;

		// Bounds-check the sfnt structure before an external font rasterizer reads the same source bytes. Reuses the
		// importer's checks without baking an atlas. Pure and thread-safe; retains no bytes. ImportFailed for invalid data.
		// This checks table structure, not arbitrary glyph programs; callers still use trusted font resources.
		[[nodiscard]] static Status ValidateSource(std::span<const std::byte> source);

		// Registers FontImportSettings.
		static void RegisterTypes(TypeRegistry& registry);
	};

}
