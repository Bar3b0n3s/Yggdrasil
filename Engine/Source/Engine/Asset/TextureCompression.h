#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Texture block compression (M23, C23.1: R-TEXC-BC and R-TEXC-HDR; Docs/Decisions/0034-m23-contract.md decisions 13 to
// 15): the BC codecs of Compressonator's CMP_Core (Vendor/CMP_Core) over whole image levels. TextureImporter and
// EnvironmentImporter (version 2, stream X and Y) compress their levels with it at import, and the GpuResourceCache
// decompresses them on a device without BC support (stream X), so the editor and the Runtime share one codec, as they
// share the pak's (PakCompression.h).
//
// Determinism (plan §7 item 12): a level compresses to the same bytes in every configuration of one compiler, never
// compared across compilers. The codecs run with the precise floating-point model (Vendor/CMP_Core/premake5.lua,
// Scripts/CheckBuildConfig.py) and on their scalar paths: InitializeTextureCodecs, the ProcessContext's TextureCodecs
// step, turns CMP_Core's SIMD selection off before any block is compressed, so the result never depends on the CPU's
// instruction set either. "TextureCompression: the fixture set compresses to the committed bytes in every
// configuration" holds the evidence: run in Debug and Release, both against one committed digest.
//
// Thread-safe once InitializeTextureCodecs ran: import workers compress in parallel, each call with its own codec state.

namespace Engine {

	// The block codecs: every codec of CMP_Core that passes the determinism check (the file comment). BC2 and BC3 are left
	// out: CMP_Core's BC3 colour fit reads uninitialized memory when a block has texels below its alpha threshold, so it
	// compresses differently in Debug and Release (Docs/Decisions/0034-m23-contract.md decision 13), and BC7 holds every
	// RGBA level at the same 16 bytes a block. The values are not persisted (TextureFormat's are, TextureData.h).
	enum class TextureBlockCodec : uint8_t
	{
		BC1,  // RGB, 8 bytes a block (opaque: alpha is not encoded)
		BC4,  // one channel (red), 8 bytes a block
		BC5,  // two channels (red, green), 16 bytes a block
		BC6H, // RGB half float, unsigned (UF16), 16 bytes a block
		BC7   // RGBA, 16 bytes a block
	};

	// "BC1", "BC4", "BC5", "BC6H", "BC7"; "Unknown" outside the enumeration.
	[[nodiscard]] std::string_view TextureBlockCodecToString(TextureBlockCodec codec);

	// The bytes of one 4x4 block: 8 for BC1 and BC4, 16 for the others; 0 outside the enumeration.
	[[nodiscard]] constexpr uint32_t GetTextureBlockBytes(TextureBlockCodec codec)
	{
		switch (codec)
		{
			case TextureBlockCodec::BC1:
			case TextureBlockCodec::BC4:  return 8;
			case TextureBlockCodec::BC5:
			case TextureBlockCodec::BC6H:
			case TextureBlockCodec::BC7:  return 16;
		}
		return 0;
	}

	// The bytes of a level of `width` x `height` texels: ceil(width / 4) * ceil(height / 4) blocks; 0 when a dimension is 0.
	[[nodiscard]] constexpr uint64_t ComputeTextureBlockLevelSize(TextureBlockCodec codec, uint32_t width, uint32_t height)
	{
		const uint64_t columns = (uint64_t{ width } + 3) / 4;
		const uint64_t rows = (uint64_t{ height } + 3) / 4;
		return columns * rows * GetTextureBlockBytes(codec);
	}

	// The bytes per texel of a level's uncompressed form, which CompressTextureBlocks reads and DecompressTextureBlocks
	// writes: 8 (RGBA16F: four IEEE 754 binary16 values, little-endian) for BC6H, 4 (RGBA8) for the others.
	[[nodiscard]] constexpr uint32_t GetTextureBlockTexelBytes(TextureBlockCodec codec)
	{
		return codec == TextureBlockCodec::BC6H ? 8 : 4;
	}

	// Sets CMP_Core's process-level state once, before any block is compressed: its SIMD selection off, so every codec runs
	// its scalar path (the BC1 endpoint search is the only one with SIMD variants). The ProcessContext's TextureCodecs step
	// calls it (Architecture §3 rule 5: process-level state); calling it again changes nothing. Not thread-safe: it runs
	// before any import worker exists.
	void InitializeTextureCodecs();

	// Compresses one level of `width` x `height` texels (each 1 to MaxTextureDimension), rows top first, tightly packed
	// (width * GetTextureBlockTexelBytes(codec) bytes a row): RGBA8 for BC1 (alpha ignored: every texel's colour is
	// encoded and decodes opaque) and BC7 (straight alpha), the red channel of RGBA8 for BC4, red and green for BC5,
	// RGBA16F for BC6H (alpha ignored; the values must be finite and non-negative, as an environment's are). A level whose size is not a multiple of 4 is padded by repeating its last column and row.
	// Returns ComputeTextureBlockLevelSize(codec, width, height) bytes: the blocks row by row, left to right. The codec
	// settings are fixed (TextureCompression.cpp), so the same texels always give the same bytes (the file comment).
	// Errors: InvalidArgument for a zero or too large dimension, a texel count that is not texels' size, or a BC6H texel
	// that is negative or not finite (naming the first such texel); ImportFailed when CMP_Core reports an error for a
	// block (naming the block and CMP_Core's code).
	[[nodiscard]] Result<Buffer> CompressTextureBlocks(TextureBlockCodec codec, uint32_t width, uint32_t height, std::span<const std::byte> texels);

	// Decompresses one level that CompressTextureBlocks (or any BC encoder) wrote into its uncompressed form, cropped to
	// `width` x `height`: RGBA8 for BC1 (alpha 255) and BC7; RGBA8 (r, 0, 0, 255) for BC4 and (r, g, 0, 255) for BC5, which is
	// what a sampler returns from the GPU's BC4 and BC5 formats; RGBA16F with alpha 1.0 for BC6H. This is the decode
	// fallback of a device without BC support (Docs/Decisions/0034-m23-contract.md decision 15): CMP_Core's reference
	// decoding, which a GPU's BC decoder matches within the formats' specified tolerance, not bit for bit. Pure and
	// deterministic. Errors: InvalidArgument for a zero or too large dimension or when blocks' size is not
	// ComputeTextureBlockLevelSize(codec, width, height).
	[[nodiscard]] Result<Buffer> DecompressTextureBlocks(TextureBlockCodec codec, uint32_t width, uint32_t height, std::span<const std::byte> blocks);

}
