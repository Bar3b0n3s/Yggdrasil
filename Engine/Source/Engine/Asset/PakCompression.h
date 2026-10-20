#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <span>

// The entry codec of pak version 2 (Architecture §14.1; Docs/Decisions/0030-m19-contract.md decision 17, B-PAKZ): zlib
// streams (RFC 1950: a deflate stream with its 2-byte header and Adler-32 trailer) through the vendored stb's deflate
// (stb_image_write.h's stbi_zlib_compress) and inflate (stb_image.h's stbi_zlib_decode_buffer), whose one implementation
// translation unit is Graphics/ThirdParty/StbImageImplementation.cpp. They live in Asset beside the pak format, so the
// Runtime's PakReader inflates and the editor's PakWriter (AssetPipeline) deflates with the same pair. Pure and
// thread-safe; neither logs.

namespace Engine {

	// The deflate quality of every compressed pak entry: stb's own default (stbi_write_png_compression_level), fixed so two
	// exports of the same inputs are byte-identical (§7.6).
	inline constexpr int PakDeflateQuality = 8;

	// The largest entry, compressed or not, the codec takes: stb's lengths are int. The writer stores a larger entry
	// uncompressed.
	inline constexpr uint64_t MaxPakCompressedEntrySize = 0x7fffffffull;

	// `bytes` as a zlib stream at PakDeflateQuality. Deterministic: the same bytes give the same stream in every
	// configuration of one compiler (ADR 0028's encoder rule). Running out of memory is fatal, as for any engine allocation.
	// Errors: InvalidArgument for more than MaxPakCompressedEntrySize bytes (PakWriter::Build stores such an entry instead).
	[[nodiscard]] Result<Buffer> DeflatePakEntry(std::span<const std::byte> bytes);

	// Inflates the zlib stream `stored` into exactly `size` bytes. Never asserts on data: any byte sequence is a valid input,
	// which the fuzz case "PakCompression: 10,000 seeded mutations of a deflate stream never crash and always return Result"
	// proves (Tests/Data/Fuzz/Targets.json, reader PakInflate). stb checks no Adler-32, so a damaged stream that still
	// inflates to `size` bytes is caught by the caller's XXH64 of the result (PakReader::ReadEntry). Errors: Validation when
	// the stream is malformed, ends early or inflates to more or fewer than `size` bytes, and when `stored` or `size`
	// exceeds MaxPakCompressedEntrySize.
	[[nodiscard]] Result<Buffer> InflatePakEntry(std::span<const std::byte> stored, uint64_t size);

}
