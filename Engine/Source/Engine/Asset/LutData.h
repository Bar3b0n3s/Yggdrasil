#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Colour-grading LUTs (M26, C26.1, R-LUT; Docs/Decisions/0037-m26-contract.md decisions 3 and 15): a 3D lookup table as an
// asset (AssetType::Lut, format version 1), imported from an Adobe/IRIDAS `.cube` file by LutImporter (version 1) or
// generated in code (the built-in LUTs, engine://Luts/<Name>, BuiltinAssetHandles' block 0x0281 to 0x02c0), and applied by
// the tonemap pass after its curve (PostProcess version 2's Lut and LutIntensity; Renderer/TonemapPass.h's step order:
// exposure, curve, LUT, encode, dither). Data only:
//   - Size^3 entries, Size in [MinLutSize, MaxLutSize], each a display-encoded RGB output (below); entry (r, g, b) is
//     Values[r + Size * (g + Size * b)] (red fastest, the .cube file's order).
//   - The table's domain is the display-encoded value: the input colour the grading addresses is the tonemapped,
//     display-linear value encoded with the sRGB OETF (Common/Color.slang), as grading LUTs are authored, mapped from
//     [DomainMin, DomainMax] per channel to the grid's first and last entries (the tonemap pass's LutScale and LutOffset,
//     Shared/TonemapConstants.h); values outside it clamp. An output entry is display-encoded too: the pass decodes it with
//     the sRGB EOTF before LutIntensity blends it with the ungraded value and the encode step encodes the result, so a grid
//     of identity entries changes an image only by the trilinear sampling's and the table's 16-bit float storage's rounding
//     (decision 15).
//   - Only 3D tables: a `.cube` file with LUT_1D_SIZE is refused.
//
// Frozen by the M26 contract C26.1; stream X-B implements the payload, the validation, the `.cube` parser and the built-in
// LUTs.

namespace Engine {

	// The limits ValidateLutData and ParseCubeLut enforce, so a table that external input controls cannot exhaust memory:
	// at most 64^3 entries (3 MiB of floats).
	inline constexpr uint32_t MinLutSize = 2;
	inline constexpr uint32_t MaxLutSize = 64;
	// The longest `.cube` file ParseCubeLut reads, in bytes (a 64^3 table of nine-digit values is about 8.5 MB).
	inline constexpr size_t MaxCubeLutBytes = 32u * 1024u * 1024u;

	// A loaded LUT (AssetType::Lut). Plain data; immutable once loaded.
	struct LutData : Asset
	{
		static constexpr AssetType StaticType = AssetType::Lut;
		// The cooked payload layout below (ADR 0028's phase-3 ledger: a new format at version 1).
		static constexpr uint16_t FormatVersion = 1;

		LutData()
			: Asset(StaticType)
		{
		}

		uint32_t Size = MinLutSize;
		glm::vec3 DomainMin{ 0.0f };
		glm::vec3 DomainMax{ 1.0f };
		// Size^3 entries, red fastest (see the file comment).
		std::vector<glm::vec3> Values{};
	};

	// The invariants every producer guarantees and every reader re-checks on untrusted data: Size in [MinLutSize,
	// MaxLutSize]; DomainMin and DomainMax finite with DomainMin < DomainMax per channel; exactly Size^3 values, each finite.
	// Pure. Errors: Validation naming the first violation.
	[[nodiscard]] Status ValidateLutData(const LutData& lut);

	// The payload (FormatVersion 1), little-endian, floats as their bits:
	//     uint32 Size; f32x3 DomainMin; f32x3 DomainMax; Size^3 x f32x3 Values (red fastest)
	// Asserts ValidateLutData. Pure; equal tables give equal bytes in every configuration.
	[[nodiscard]] Buffer SerializeLutPayload(const LutData& lut);

	// Reads a payload written by SerializeLutPayload, checking Size against its limit and the bytes left before anything is
	// allocated, then ValidateLutData. Never asserts on data (fuzzed with ParseCubeLut: the fuzz target LutData, "LutData:
	// 10,000 seeded mutations of a .cube file and of a payload never crash"). Errors: Parse for truncation, a size above its
	// limit or trailing bytes; Validation from ValidateLutData.
	[[nodiscard]] Result<LutData> DeserializeLutPayload(std::span<const std::byte> payload);

	// Parses an Adobe/IRIDAS `.cube` file (UTF-8 or ASCII text, external input): an optional TITLE "..." line, LUT_3D_SIZE N
	// (MinLutSize to MaxLutSize), optional DOMAIN_MIN r g b and DOMAIN_MAX r g b (default 0 0 0 and 1 1 1) or, instead of
	// them, DaVinci Resolve's LUT_3D_INPUT_RANGE min max (the uniform domain [min, max] on every channel: DOMAIN_MIN min min
	// min and DOMAIN_MAX max max max; the W26.1 integration's addition, Docs/Decisions/0037-m26-contract.md), then exactly N^3
	// lines of three floats, red fastest; blank lines and lines starting with '#' are ignored anywhere, keywords may come in
	// any order before the first data line, each at most once, and numbers are read with the locale-independent from_chars.
	// Never asserts on data. Errors: InvalidArgument for a file over MaxCubeLutBytes; Parse naming the line (1-based) for an
	// unknown or repeated keyword, LUT_3D_INPUT_RANGE beside DOMAIN_MIN or DOMAIN_MAX, a malformed number, a data line before
	// LUT_3D_SIZE, the wrong count of values on a line, or a table with fewer or more than N^3 entries; Unsupported for
	// LUT_1D_SIZE (only 3D tables are graded); Validation from ValidateLutData.
	[[nodiscard]] Result<LutData> ParseCubeLut(std::string_view text);

	// The complete cooked artifact (CookedHeader + payload, §6.8) of `lut`, produced by a source of version
	// `importerVersion`. Asserts ValidateLutData (through SerializeLutPayload).
	[[nodiscard]] Buffer CookLut(const LutData& lut, uint32_t importerVersion);

	// The LUT of a cooked artifact (both asset managers' loader for AssetType::Lut, RegisterBuiltinLoaders): ReadCookedArtifact
	// with AssetType::Lut and FormatVersion, then the payload. Errors: as ReadCookedArtifact and DeserializeLutPayload.
	[[nodiscard]] Result<AssetRef<LutData>> LoadCookedLut(std::span<const std::byte> cooked);

	// The built-in LUTs (ADR 0028's phase-3 ledger: the block 0x0281 to 0x02c0, the identity LUT first),
	// engine://Luts/<Name> (BuiltinAssetHandles::IdentityLut), procedural and generated in code.
	enum class BuiltinLut : uint8_t
	{
		Identity // 33^3 entries, entry (r, g, b) = (r, g, b) / 32, domain [0, 1]: grading with it leaves the colours as they were
	};

	// "Identity"; the name in engine://Luts/<Name>. "Unknown" for a value outside the enumeration. Pure and thread-safe.
	[[nodiscard]] std::string_view BuiltinLutToString(BuiltinLut lut);

	// Generates `lut` as its enumerator's comment defines it, deterministically (each entry an exact quotient), so its bytes
	// are identical in every configuration. The result passes ValidateLutData. Stream X-B, with the catalogue entries
	// (Resources/EngineAssets.json and the procedural table of BuiltinAssets.cpp) and the procedural Lut branch of
	// CreateProceduralBuiltinAsset.
	[[nodiscard]] LutData GenerateBuiltinLut(BuiltinLut lut);

}
