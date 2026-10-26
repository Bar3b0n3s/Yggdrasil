#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The pak layout (Architecture §14.1), frozen by the M6 contract and given per-entry compression by M19
// (Docs/Decisions/0030-m19-contract.md decision 17, B-PAKZ): written by AssetPipeline/PakWriter, read by PakReader, mounted
// by PakMount, served by RuntimeAssetManager. Little-endian throughout.
//
//     [64-byte header][entry data, each entry at a 16-byte-aligned offset, zero padding between][TOC: canonical JSON]
//
// Header: char Magic[8] = "ENGPAK01"; uint32 Version; uint32 EntryCount; uint64 TocOffset; uint64 TocSize; uint64 TocXXH64;
// uint8 Reserved[24] (zero). The TOC, written by JsonWriter (Pretty, so a pak is easy to inspect):
//     { "Format": "PakToc", "Version": 2,
//       "Entries": [ { "Handle": "77e1a0c4d2b95f01", "Type": "Mesh", "Path": "Assets/Models/Track.glb#mesh:0:Straight",
//                      "Offset": 64, "Size": 1234, "XXH64": "9f1c...", "Compression": "Deflate", "StoredSize": 517 }, ... ],
//       "Metadata": { ... } }
// Entries are sorted by (Handle, Path). Two kinds:
//   - cooked assets: Handle is the asset handle, Type its AssetType name, Path its readable reference (FormatAssetReference:
//     "Assets/..." with an optional "#<key>", or "engine://..." for built-ins), and the bytes a complete cooked artifact
//     (CookedHeader + payload, §6.8);
//   - plain files (the target configuration's SPIR-V in Engine.pak, M7): Handle null, Type "File", Path a relative VFS
//     path ("Shaders/Forward_VSMain.spv"), and the file's bytes. PakMount serves these.
// "Size" and "XXH64" are those of the entry's own bytes, the ones PakReader::ReadEntry returns (for a cooked asset, the
// cooked artifact whose XXH64 AssetManager::GetCookedHash shares with the editor's cache). "Compression" says how they are
// stored at Offset ("None" or "Deflate", PakCompression) and "StoredSize" how many bytes they take there: a "None" entry
// has StoredSize == Size; a "Deflate" entry holds a zlib stream (RFC 1950, Asset/PakCompression.h) that inflates to its
// Size bytes, with 0 < StoredSize < Size and 8 × (Size − StoredSize) >= Size (IsValidPakDeflateSize: the writer keeps a
// stream only when it saves at least an eighth). The byte-range rules apply to [Offset, Offset + StoredSize).
// "Metadata" is an object the exporter fills (the project settings and the asset registry data the runtime needs ride in
// the TOC, §14.1; M7 and M15 define its members); readers keep it verbatim. Integrity (§14.1): the TOC hash is verified at
// open; each entry's XXH64 is verified on its first read in every configuration, after a deflated entry is inflated.
// Readers read exactly the current version: a pak is a build output, never committed, and an export's paks and Runtime
// come from one build, so there is no migration (version 1 had no Compression or StoredSize).

namespace Engine {

	// The first pak version with per-entry compression (decision 17), reserved by the phase 1 ledger: the current version of
	// the header and the TOC.
	inline constexpr uint32_t PakCompressionFormatVersion = 2;

	// How an entry's bytes are stored (version 2's "Compression" TOC member, spelled as the enumerator).
	enum class PakCompression : uint8_t
	{
		None,   // stored as they are: StoredSize == Size
		Deflate // a zlib stream (RFC 1950) that inflates to exactly Size bytes (InflatePakEntry, Asset/PakCompression.h)
	};

	struct PakHeader
	{
		static constexpr std::array<char, 8> Magic = { 'E', 'N', 'G', 'P', 'A', 'K', '0', '1' };
		static constexpr size_t Size = 64;
		static constexpr uint32_t CurrentVersion = PakCompressionFormatVersion;

		uint32_t Version = CurrentVersion;
		uint32_t EntryCount = 0;
		uint64_t TocOffset = 0;
		uint64_t TocSize = 0;
		uint64_t TocHash = 0; // XXH64 (seed 0) of the TOC bytes

		bool operator==(const PakHeader&) const = default;
	};

	// Entry data starts at multiples of this.
	inline constexpr uint64_t PakEntryAlignment = 16;
	// The Type of a plain-file entry.
	inline constexpr std::string_view PakFileEntryType = "File";

	struct PakEntry
	{
		AssetHandle Handle{}; // null for a plain file
		std::string Type{};   // an AssetType name, or PakFileEntryType
		std::string Path{};
		uint64_t Offset = 0; // from the start of the pak, a multiple of PakEntryAlignment, after the header
		uint64_t Size = 0;
		uint64_t Hash = 0; // XXH64 (seed 0) of the entry's bytes ("XXH64" in the TOC)
		// How the bytes are stored, and how many bytes they take at Offset ("StoredSize"; equal to Size for None). Size and
		// Hash stay those of the entry's own bytes, before compression (decision 17).
		PakCompression Compression = PakCompression::None;
		uint64_t StoredSize = 0;

		bool operator==(const PakEntry&) const = default;
	};

	struct PakToc
	{
		static constexpr std::string_view FormatName = "PakToc";
		static constexpr uint32_t CurrentVersion = PakCompressionFormatVersion;

		std::vector<PakEntry> Entries{}; // sorted by (Handle, Path)
		VariantValue Metadata{};         // a JSON object (null is written as {})

		bool operator==(const PakToc&) const = default;
	};

	// Whether a Deflate entry of `size` bytes may take `storedSize` bytes: 0 < storedSize < size and
	// 8 × (size − storedSize) >= size, so the stream saves at least an eighth (decision 17). PakWriter keeps a deflated stream
	// only when this holds, and ParsePakToc refuses a Deflate entry for which it does not. Pure; never overflows.
	[[nodiscard]] bool IsValidPakDeflateSize(uint64_t size, uint64_t storedSize);

	// The 64 header bytes. Pure.
	[[nodiscard]] std::array<std::byte, PakHeader::Size> WritePakHeader(const PakHeader& header);

	// Reads and checks the first 64 bytes of `bytes`: the magic, zero reserved bytes, a Version this build reads. Never
	// asserts on data. Errors: Parse for truncation, a wrong magic or non-zero reserved bytes; UnsupportedVersion naming
	// both versions.
	[[nodiscard]] Result<PakHeader> ReadPakHeader(std::span<const std::byte> bytes);

	// The canonical TOC text (the layout above). Asserts sorted entries with unique paths. Pure; identical TOCs give
	// identical bytes.
	[[nodiscard]] std::string SerializePakToc(const PakToc& toc);

	// Reads a TOC strictly: the format and version, every member present with its type, entries sorted by (Handle, Path)
	// with unique paths and, among cooked assets, unique handles; each Type an AssetType name other than None or "File"
	// (with a null handle exactly for "File"); offsets aligned, after the header and before `dataEnd`, ranges inside
	// [PakHeader::Size, dataEnd) and not overlapping, where an entry's range is [Offset, Offset + StoredSize); a known
	// Compression whose StoredSize follows its rule (None: equal to Size; Deflate: IsValidPakDeflateSize). Never asserts on
	// data. Errors: Parse; Validation (located), also for an older TOC Version; UnsupportedVersion for a newer one.
	[[nodiscard]] Result<PakToc> ParsePakToc(std::string_view text, uint64_t dataEnd);

}
