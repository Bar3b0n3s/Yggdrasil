#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

// The common header of every cooked artifact (Architecture §6.8): project cache entries (Library/Cache), the engine cooked
// cache (bin/EngineCache) and pak entries. The payload layouts are documented next to their data types (MeshData.h,
// TextureData.h, FontData.h, MaterialData.h, DocumentData.h); the environment, audio clip, script and replay payloads join
// with M8, M12 and M13. Cooked bytes are read only through BinaryReader, which bounds-checks every read (§6.8), and the
// readers are fuzzed (§15.2).

static_assert(std::endian::native == std::endian::little, "cooked formats are little-endian (Architecture §6.8)");

namespace Engine {

	// The engine-wide cook version, part of every cache key (§7.5: "... ‖ engine cook version"). Bump it when a change to
	// the engine (not to one importer, which bumps its own version) alters cooked bytes, so every cache entry is rebuilt.
	inline constexpr uint32_t EngineCookVersion = 1;

	// The 32-byte little-endian header (§6.8):
	//     char Magic[4] = "ECKD"; uint16 FormatVersion; uint16 AssetType; uint32 ImporterVersion; uint32 Flags;
	//     uint64 PayloadSize; uint64 PayloadXXH64
	// FormatVersion is the version of the payload layout of that AssetType (each data header names its current one);
	// ImporterVersion is the version of the importer (or built-in generator) that produced the payload; Flags is reserved
	// and must be 0; PayloadXXH64 is XXH64 (seed 0) of the PayloadSize bytes that follow the header.
	struct CookedHeader
	{
		static constexpr std::array<char, 4> Magic = { 'E', 'C', 'K', 'D' };
		static constexpr size_t Size = 32;

		uint16_t FormatVersion = 0;
		AssetType Type = AssetType::None;
		uint32_t ImporterVersion = 0;
		uint32_t Flags = 0;
		uint64_t PayloadSize = 0;
		uint64_t PayloadHash = 0;

		bool operator==(const CookedHeader&) const = default;
	};

	// A validated view of one cooked artifact. The payload views the bytes passed to ReadCookedArtifact, which must outlive it.
	struct CookedArtifactView
	{
		CookedHeader Header{};
		std::span<const std::byte> Payload{};
	};

	// The complete artifact: the header for `payload` (its size and XXH64 filled in) followed by `payload`. Asserts a type
	// other than None. Pure and thread-safe; identical inputs give identical bytes.
	[[nodiscard]] Buffer WriteCookedArtifact(AssetType type, uint16_t formatVersion, uint32_t importerVersion,
		std::span<const std::byte> payload);

	// Reads and validates an artifact: the magic, Flags 0, a known AssetType other than None, PayloadSize equal to the bytes
	// after the header (no trailing data), and PayloadXXH64. It never trusts a size it read and never asserts on data
	// (fuzzed: "CookedFormat: 10,000 seeded mutations never crash and always return Result"). Pure and thread-safe.
	// Errors: Parse for truncated data, a wrong magic, a size mismatch or non-zero flags; Validation for an unknown or None
	// type and for a hash mismatch ("payload hash mismatch: the artifact is corrupted"), so a cache treats either as an
	// entry to rebuild (§7.5) and a pak as a corrupted entry (§14.1).
	[[nodiscard]] Result<CookedArtifactView> ReadCookedArtifact(std::span<const std::byte> bytes);

	// ReadCookedArtifact, then checks that the artifact holds `expectedType` at `formatVersion`. Errors: those of
	// ReadCookedArtifact; Validation naming both types for another type; UnsupportedVersion naming both versions for a
	// FormatVersion this build does not read.
	[[nodiscard]] Result<CookedArtifactView> ReadCookedArtifact(std::span<const std::byte> bytes, AssetType expectedType,
		uint16_t formatVersion);

}
