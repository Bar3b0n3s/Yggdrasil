#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/FontData.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Engine {

	class TypeRegistry;

	// The codepoints a font's Ranges may name (FontCharacterRange's First and Last) and the limits that bound the baker's
	// work (Docs/Decisions/0033-m22-contract.md decision 6): at most MaxFontCharacterRanges ranges whose sizes (Last - First
	// + 1, overlaps counted in each) add up to at most MaxFontRangeCodepoints.
	inline constexpr uint32_t MinFontRangeCodepoint = 0x20;
	inline constexpr uint32_t MaxFontRangeCodepoint = 0x10FFFF;
	inline constexpr size_t MaxFontCharacterRanges = 64;
	inline constexpr uint64_t MaxFontRangeCodepoints = 65536;
	// The largest atlas side a font bakes into: a character set whose glyphs do not pack into a square of this side is
	// ImportFailed, before any glyph is rasterized (FontImporter::Bake).
	inline constexpr uint32_t MaxFontAtlasSize = 16384;

	// Registry struct "FontCharacterRange": an inclusive range of codepoints, MinFontRangeCodepoint <= First <= Last <=
	// MaxFontRangeCodepoint (located Validation errors otherwise).
	struct FontCharacterRange
	{
		uint32_t First = MinFontRangeCodepoint;
		uint32_t Last = MinFontRangeCodepoint;
	};

	// Registry struct "FontImportSettings".
	struct FontImportSettings
	{
		float PixelSize = 48.0f; // atlas pixels per em (§7.4: 48 px); 16 to 128
		float Spread = 8.0f;     // SDF spread in atlas pixels (§7.4: spread 8); 1 to 32
		// Codepoints baked besides ASCII and Latin-1 (M22), within the limits above, located under /Ranges.
		std::vector<FontCharacterRange> Ranges{};
		// C22.3 (Docs/Decisions/0033-m22-contract.md decision 42; registered by stream F after Ranges, with the importer's
		// version 5): the fonts a text in this font falls back to, in order, for a codepoint this font lacks (the Ui pass's
		// chain, BuildUiFontChain): at most MaxFontFallbacks, none null, none the font itself, located Validation errors under
		// /Fallbacks otherwise. Cooked into FontData::Fallbacks and recorded as the import's dependencies (Import.Dependencies),
		// so an export that prunes keeps them; a fallback need not exist when the font is imported.
		std::vector<TypedAssetHandle<AssetType::Font>> Fallbacks{};
	};

	// What one FontImporter::Bake measured and did: the bound on its work, which the tests read.
	struct FontBakeReport
	{
		uint32_t Glyphs = 0;           // the character set's codepoints the font has a glyph for
		uint32_t AtlasSize = 0;        // the atlas side their boxes pack into; 0 when they do not fit (or the bake failed first)
		uint32_t RasterizedGlyphs = 0; // the distance fields computed: none unless the boxes fit
		uint32_t ShapingGlyphs = 0;    // C22.3 (stream S): of Glyphs, those only shaping reaches (no baked codepoint maps to them)
	};

	// TTF and OTF (Architecture §7.4): an R8 signed-distance-field atlas of ASCII and Latin-1 (U+0020-U+007E and
	// U+00A0-U+00FF) and of the settings' Ranges (codepoints the font lacks are left out, and a codepoint named twice is baked
	// once) with glyph metrics, and the kerning between the glyphs of ASCII and Latin-1, rasterized through stb_truetype.h
	// v1.26 of the vendored stb (Vendor/stb), compiled with STBTT_STATIC in the importer's own translation unit only
	// (Appendix C), so it never clashes with Dear ImGui's own copy for its font atlas. Glyphs are packed deterministically
	// (sorted by codepoint, a fixed shelf packer into the smallest power-of-two square that fits, at most MaxFontAtlasSize),
	// from their measured boxes before any is rasterized (Bake). Cooked as one FontData (CookFont). A font stb_truetype
	// cannot read, one without any glyph of the character set, or one whose glyphs do not fit is ImportFailed. Version 2:
	// CFF (.otf) outlines keep their cubic segments in the distance field (ADR 0022 "Integration"); TrueType atlases are
	// byte-identical to version 1's. Version 3 (M19, Docs/Decisions/0030-m19-contract.md decision 23, A-STBTT): the
	// rasterizer compiles stb_truetype.h of the vendored stb (the pinned commit 2c980bb) instead of Dear ImGui's
	// imstb_truetype.h, so cached atlases are rebuilt once. The distance field follows that header's solver, with its
	// thresholds for near-singular segments, and Inter's atlases at the default settings are version 2's byte for byte; a
	// CID-keyed CFF glyph that its FDSelect gives no font dictionary and that calls a local subroutine is refused, since that
	// header asserts on it where Dear ImGui's copy reads no subroutines. Version 4 (M22, Docs/Decisions/0033-m22-contract.md
	// decision 6, R-UI-TEXT's character ranges): the Ranges setting, so cached atlases are rebuilt once; a font without
	// Ranges bakes version 3's FontData byte for byte. A range's glyphs are not kerned: the kerning reader holds a table of
	// every ordered pair of the glyphs it kerns, which ASCII and Latin-1 keep small (191 glyphs at most) where Ranges reach
	// 65,536 codepoints. Version 5 (M22, C22.3, decisions 42 and 44; streams S and F): FontData format 2. The bake adds the
	// glyphs shaping can produce from the baked codepoints with the shaper's default features (HarfBuzz's
	// hb_ot_shape_glyphs_closure, one buffer per script of the baked codepoints, no user features: an Arabic letter's
	// contextual forms, ligatures), counted in Glyphs and ShapingGlyphs; packs the codepoint glyphs first exactly as version
	// 4 does, then those glyphs, by glyph index, in the same packer, growing the square only when they do not fit; records
	// every baked glyph's index (IndexedGlyphs) and the font's units per em; embeds the font subset to the baked glyphs by
	// HarfBuzz's subsetter, glyph ids retained (ShapingSource); and cooks the settings' Fallbacks, which it records as the
	// import's dependencies. So a font whose shaping adds no glyph keeps version 4's atlas byte for byte, one whose added
	// glyphs fit keeps every codepoint glyph where version 4 put it, and every codepoint glyph keeps version 4's field.
	// Inter, the built-in default font, gains the glyphs its default features reach from ASCII and Latin-1 (contextual
	// alternates, and the fraction forms of frac, numr and dnom, among them), so its atlas changes deliberately
	// (Docs/Decisions/0033-m22-contract.md decision 44, the lead's decision 2 of the review of C22.3).
	class FontImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "Font";
		static constexpr uint32_t Version = 4;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::Font; }
		// ".ttf", ".otf".
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return "FontImportSettings"; }

		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;

		// Bakes the font `source` with `settings` into the FontData that Import cooks (`name`, the source's path, names it in
		// errors), recording its work in `report`. Every glyph's distance-field box (its outline's bounding box at PixelSize
		// with Spread pixels around it, TrueTypeFont::GetGlyphSdfBox) is measured and the boxes are packed before any field
		// is rasterized, so a character set that cannot fit a MaxFontAtlasSize atlas is ImportFailed with no field computed
		// (Docs/Decisions/0033-m22-contract.md's W22.1 review: a large Ranges at a large PixelSize and Spread would otherwise
		// rasterize gigabytes before failing). Errors: those of Import's bake (ImportFailed for an unreadable font, a glyph
		// box over 2048 pixels on a side, no glyph of the character set, or glyphs that do not fit). Pure and thread-safe.
		[[nodiscard]] static Result<FontData> Bake(std::span<const std::byte> source, const FontImportSettings& settings, std::string_view name, FontBakeReport& report);

		// Bounds-check the sfnt structure before an external font rasterizer reads the same source bytes. Reuses the
		// importer's checks without baking an atlas (ValidateTrueTypeTables: the tables, every TrueType glyph outline, and
		// the CFF table with every glyph's charstring as stb_truetype runs it). Pure and thread-safe; retains no bytes.
		// ImportFailed for invalid data. It covers what the importer makes stb_truetype read; a rasterizer that reads more
		// (Dear ImGui's atlas builder rasterizes and kerns with stb_truetype itself) still needs trusted font resources.
		[[nodiscard]] static Status ValidateSource(std::span<const std::byte> source);

		// Registers FontCharacterRange and FontImportSettings.
		static void RegisterTypes(TypeRegistry& registry);
	};

}
