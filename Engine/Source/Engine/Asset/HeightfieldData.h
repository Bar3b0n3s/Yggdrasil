#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Heightfields (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decisions 3 and 20): a terrain's height samples as
// an asset (AssetType::Heightfield, format version 1). Its source file, `<name>.heightfield`, is the 8-byte source header
// ("EHFD" then the uint32 version 1, little-endian) followed by the payload below, which HeightfieldImporter (version 1)
// cooks unchanged after its CookedHeader. A heightfield is made by asset.create {type: "Heightfield"} (flat), converted from
// a 16-bit PNG or a RAW heightmap by terrain.importHeightmap (ConvertHeightmap, AssetPipeline/Importers/
// HeightfieldImporter.h), and sculpted by terrain.edit, which rewrites the source (EditorCore/Terrain/TerrainEditing.h).
// Data only:
//   - Resolution x Resolution samples, Resolution in [MinHeightfieldResolution, MaxHeightfieldResolution] (ADR 0028's
//     phase-3 ledger: at most 1025 squared samples); square, because Jolt's height field is (PhysicsShape.h's
//     HeightFieldShapeGeometry).
//   - Sample (x, z) is Heights[x + Resolution * z]: column x along the terrain's local +X, row z along its local +Z, row 0
//     first, as a heightmap image's top row is row 0.
//   - A sample is a uint16 fraction of the terrain's HeightScale: height = sample / 65535 * HeightScale metres above the
//     terrain's origin (TerrainComponent.h), so the format carries no units and an imported 16-bit heightmap keeps its bits.
//
// Frozen by the M26 contract C26.1; stream T-A implements the payload, the validation and the source reader and writer.

namespace Engine {

	// ADR 0028's phase-3 ledger: at most 1025 x 1025 samples. At least 3 x 3, the fewest Jolt's height field builds (its
	// sample count rounded up to its block size of 2 must hold two blocks).
	inline constexpr uint32_t MinHeightfieldResolution = 3;
	inline constexpr uint32_t MaxHeightfieldResolution = 1025;
	// The largest sample: a height of exactly HeightScale.
	inline constexpr uint32_t MaxHeightfieldSample = 65535;
	// The source file's magic, its first four bytes.
	inline constexpr std::array<char, 4> HeightfieldSourceMagic = { 'E', 'H', 'F', 'D' };

	// A loaded heightfield (AssetType::Heightfield). Plain data; immutable once loaded.
	struct HeightfieldData : Asset
	{
		static constexpr AssetType StaticType = AssetType::Heightfield;
		// The cooked payload layout below and the source file's version (ADR 0028's phase-3 ledger: a new format at version 1).
		static constexpr uint16_t FormatVersion = 1;

		HeightfieldData()
			: Asset(StaticType)
		{
		}

		uint32_t Resolution = MinHeightfieldResolution;
		// Resolution^2 samples, row-major from row 0 (see the file comment).
		std::vector<uint16_t> Heights{};
	};

	// A flat heightfield of `resolution` samples a side, every sample `height` (asset.create's default: 129 and 0). Asserts
	// that the resolution is in range. Pure.
	[[nodiscard]] HeightfieldData MakeFlatHeightfield(uint32_t resolution, uint16_t height = 0);

	// The invariants every producer guarantees and every reader re-checks on untrusted data: Resolution in
	// [MinHeightfieldResolution, MaxHeightfieldResolution] and exactly Resolution^2 samples. Pure. Errors: Validation naming
	// the violation.
	[[nodiscard]] Status ValidateHeightfieldData(const HeightfieldData& heightfield);

	// The payload (FormatVersion 1), little-endian: uint32 Resolution; Resolution^2 x uint16 Heights (row-major from row 0).
	// Asserts ValidateHeightfieldData. Pure; equal heightfields give equal bytes in every configuration, which is what makes
	// a stroke through terrain.edit byte-identical to the same stroke made in the editor.
	[[nodiscard]] Buffer SerializeHeightfieldPayload(const HeightfieldData& heightfield);

	// Reads a payload written by SerializeHeightfieldPayload, checking Resolution against its limits and the bytes left
	// before anything is allocated, then ValidateHeightfieldData. Never asserts on data (fuzzed: the fuzz target
	// HeightfieldData, "HeightfieldData: 10,000 seeded mutations of a payload and of 16-bit PNG and RAW heightmaps never
	// crash"). Errors: Parse for truncation, a resolution outside its limits or trailing bytes; Validation from
	// ValidateHeightfieldData.
	[[nodiscard]] Result<HeightfieldData> DeserializeHeightfieldPayload(std::span<const std::byte> payload);

	// The source file of `heightfield`: the source header (HeightfieldSourceMagic, then uint32 FormatVersion) followed by
	// SerializeHeightfieldPayload. What asset.create, terrain.importHeightmap and terrain.edit write. Asserts
	// ValidateHeightfieldData.
	[[nodiscard]] Buffer WriteHeightfieldSource(const HeightfieldData& heightfield);

	// Reads a source file (the importer's input, external input): the magic, then a version of exactly FormatVersion, then
	// DeserializeHeightfieldPayload of the rest. Errors: Parse for a missing or wrong magic; UnsupportedVersion for another
	// version (naming it); those of DeserializeHeightfieldPayload.
	[[nodiscard]] Result<HeightfieldData> ReadHeightfieldSource(std::span<const std::byte> source);

	// The complete cooked artifact (CookedHeader + payload, §6.8) of `heightfield`, produced by a source of version
	// `importerVersion`. Asserts ValidateHeightfieldData (through SerializeHeightfieldPayload).
	[[nodiscard]] Buffer CookHeightfield(const HeightfieldData& heightfield, uint32_t importerVersion);

	// The heightfield of a cooked artifact (both asset managers' loader for AssetType::Heightfield, RegisterBuiltinLoaders):
	// ReadCookedArtifact with AssetType::Heightfield and FormatVersion, then the payload. Errors: as ReadCookedArtifact and
	// DeserializeHeightfieldPayload.
	[[nodiscard]] Result<AssetRef<HeightfieldData>> LoadCookedHeightfield(std::span<const std::byte> cooked);

}
