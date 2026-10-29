#include "EnginePCH.h"
#include "Engine/Asset/TextureCompression.h"

#include "Engine/Asset/TextureData.h"
#include "Engine/Core/Assert.h"

#include <cmp_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

// The block codecs over CMP_Core (TextureCompression.h). Each call creates its own codec options, so concurrent calls
// share nothing but CMP_Core's process-level state, which InitializeTextureCodecs set before the first call.

namespace Engine {

	namespace Utils {

		// CMP_Core's result codes (its common_def.h, which cmp_core.h does not include): 0 is success, anything else an
		// error.
		inline constexpr int CmpCoreOk = 0;
		inline constexpr int CmpCoreUnknownError = 1;

		// The codec settings every compression uses. Quality is CMP_Core's 0-to-1 effort; the values trade import time for
		// fidelity at a level a Debug editor still imports a 2048 x 2048 texture with in seconds, and changing one changes
		// every cooked texture, so TextureImporter's and EnvironmentImporter's versions follow them.
		inline constexpr float BC1Quality = 1.0f;
		inline constexpr float BC4Quality = 1.0f;
		inline constexpr float BC5Quality = 1.0f;
		inline constexpr float BC6HQuality = 0.1f;
		inline constexpr float BC7Quality = 0.1f;

		// One codec's CMP_Core options, created with its settings and destroyed with the object.
		class BlockCodecOptions
		{
		public:
			BlockCodecOptions() = default;
			~BlockCodecOptions() { Destroy(); }

			BlockCodecOptions(const BlockCodecOptions&) = delete;
			BlockCodecOptions& operator=(const BlockCodecOptions&) = delete;

			[[nodiscard]] Status Create(TextureBlockCodec codec)
			{
				m_Codec = codec;
				int result = CmpCoreOk;
				switch (codec)
				{
					case TextureBlockCodec::BC1:
						result = CreateOptionsBC1(&m_Options);
						if (result == CmpCoreOk)
							result = SetQualityBC1(m_Options, BC1Quality);
						break;
					case TextureBlockCodec::BC4:
						result = CreateOptionsBC4(&m_Options);
						if (result == CmpCoreOk)
							result = SetQualityBC4(m_Options, BC4Quality);
						break;
					case TextureBlockCodec::BC5:
						result = CreateOptionsBC5(&m_Options);
						if (result == CmpCoreOk)
							result = SetQualityBC5(m_Options, BC5Quality);
						break;
					case TextureBlockCodec::BC6H:
						result = CreateOptionsBC6(&m_Options);
						if (result == CmpCoreOk)
							result = SetQualityBC6(m_Options, BC6HQuality);
						if (result == CmpCoreOk)
							result = SetSignedBC6(m_Options, false);
						break;
					case TextureBlockCodec::BC7:
						result = CreateOptionsBC7(&m_Options);
						if (result == CmpCoreOk)
							result = SetQualityBC7(m_Options, BC7Quality);
						break;
				}
				if (result != CmpCoreOk || m_Options == nullptr)
				{
					return MakeError(ErrorCode::ImportFailed,
						"CMP_Core could not set up its {} codec (error {})", TextureBlockCodecToString(codec), result);
				}
				return {};
			}

			[[nodiscard]] const void* Get() const { return m_Options; }
		private:
			void Destroy()
			{
				if (m_Options == nullptr)
					return;
				// The destroy functions fail only for a null pointer, which is excluded above.
				switch (m_Codec)
				{
					case TextureBlockCodec::BC1:  static_cast<void>(DestroyOptionsBC1(m_Options)); break;
					case TextureBlockCodec::BC4:  static_cast<void>(DestroyOptionsBC4(m_Options)); break;
					case TextureBlockCodec::BC5:  static_cast<void>(DestroyOptionsBC5(m_Options)); break;
					case TextureBlockCodec::BC6H: static_cast<void>(DestroyOptionsBC6(m_Options)); break;
					case TextureBlockCodec::BC7:  static_cast<void>(DestroyOptionsBC7(m_Options)); break;
				}
				m_Options = nullptr;
			}
		private:
			TextureBlockCodec m_Codec = TextureBlockCodec::BC1;
			void* m_Options = nullptr;
		};

		[[nodiscard]] static bool IsKnownCodec(TextureBlockCodec codec)
		{
			return GetTextureBlockBytes(codec) != 0;
		}

		[[nodiscard]] static Status ValidateLevelSize(TextureBlockCodec codec, uint32_t width, uint32_t height)
		{
			if (!IsKnownCodec(codec))
				return MakeError(ErrorCode::InvalidArgument, "unknown texture block codec {}", static_cast<uint32_t>(codec));
			if (width == 0 || height == 0 || width > MaxTextureDimension || height > MaxTextureDimension)
			{
				return MakeError(ErrorCode::InvalidArgument,
					"a {} level of {} x {} texels is outside 1 to {} in each dimension", TextureBlockCodecToString(codec), width, height,
					MaxTextureDimension);
			}
			return {};
		}

		// IEEE 754 binary16 to float, exactly (every binary16 value is a float).
		[[nodiscard]] static float HalfToFloat(uint16_t half)
		{
			const uint32_t sign = (half & 0x8000U) != 0 ? 1U : 0U;
			const uint32_t exponent = (half >> 10) & 0x1FU;
			const uint32_t mantissa = half & 0x3FFU;
			float magnitude = 0.0f;
			if (exponent == 0)
				magnitude = std::ldexp(static_cast<float>(mantissa), -24);
			else if (exponent == 0x1F)
				magnitude = mantissa == 0 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
			else
				magnitude = std::ldexp(static_cast<float>(mantissa | 0x400U), static_cast<int>(exponent) - 25);
			return sign != 0 ? -magnitude : magnitude;
		}

		[[nodiscard]] static uint16_t ReadUInt16(const std::byte* bytes)
		{
			return static_cast<uint16_t>(std::to_integer<uint32_t>(bytes[0]) | (std::to_integer<uint32_t>(bytes[1]) << 8));
		}

		static void WriteUInt16(std::byte* bytes, uint16_t value)
		{
			bytes[0] = static_cast<std::byte>(value & 0xFFU);
			bytes[1] = static_cast<std::byte>(value >> 8);
		}

		// The 4x4 block at block column `blockX` and row `blockY` of a level, as texelBytes-wide texels in row order, the
		// level's last column and row repeated where the block reaches past them.
		static void GatherBlock(std::span<const std::byte> texels, uint32_t width, uint32_t height, uint32_t texelBytes, uint32_t blockX,
			uint32_t blockY, std::span<std::byte> block)
		{
			for (uint32_t row = 0; row < 4; ++row)
			{
				const uint32_t y = std::min(blockY * 4 + row, height - 1);
				for (uint32_t column = 0; column < 4; ++column)
				{
					const uint32_t x = std::min(blockX * 4 + column, width - 1);
					const size_t source = (static_cast<size_t>(y) * width + x) * texelBytes;
					const size_t destination = (static_cast<size_t>(row) * 4 + column) * texelBytes;
					std::memcpy(block.data() + destination, texels.data() + source, texelBytes);
				}
			}
		}

		// Compresses one gathered block (16 texels of the codec's uncompressed form) into `output`.
		[[nodiscard]] static int CompressBlock(TextureBlockCodec codec, std::span<const std::byte> block, const BlockCodecOptions& options,
			std::span<std::byte> output)
		{
			const auto* rgba = reinterpret_cast<const unsigned char*>(block.data());
			auto* compressed = reinterpret_cast<unsigned char*>(output.data());
			switch (codec)
			{
				case TextureBlockCodec::BC1: return CompressBlockBC1(rgba, 16, compressed, options.Get());
				case TextureBlockCodec::BC7: return CompressBlockBC7(rgba, 16, compressed, options.Get());
				case TextureBlockCodec::BC4:
				{
					std::array<unsigned char, 16> red{};
					for (size_t texel = 0; texel < red.size(); ++texel)
						red[texel] = rgba[texel * 4];
					return CompressBlockBC4(red.data(), 4, compressed, options.Get());
				}
				case TextureBlockCodec::BC5:
				{
					std::array<unsigned char, 16> red{};
					std::array<unsigned char, 16> green{};
					for (size_t texel = 0; texel < red.size(); ++texel)
					{
						red[texel] = rgba[texel * 4];
						green[texel] = rgba[texel * 4 + 1];
					}
					return CompressBlockBC5(red.data(), 4, green.data(), 4, compressed, options.Get());
				}
				case TextureBlockCodec::BC6H:
				{
					// CMP_Core reads three binary16 values a texel (RGB); the gathered block holds four (RGBA).
					std::array<unsigned short, 48> rgb{};
					for (size_t texel = 0; texel < 16; ++texel)
					{
						for (size_t channel = 0; channel < 3; ++channel)
							rgb[texel * 3 + channel] = ReadUInt16(block.data() + texel * 8 + channel * 2);
					}
					return CompressBlockBC6(rgb.data(), 12, compressed, options.Get());
				}
			}
			return CmpCoreUnknownError;
		}

		// Decompresses one block into 16 texels of the codec's uncompressed form (TextureCompression.h).
		[[nodiscard]] static int DecompressBlock(TextureBlockCodec codec, const std::byte* compressedBytes, std::span<std::byte> block)
		{
			const auto* compressed = reinterpret_cast<const unsigned char*>(compressedBytes);
			auto* rgba = reinterpret_cast<unsigned char*>(block.data());
			switch (codec)
			{
				case TextureBlockCodec::BC1: return DecompressBlockBC1(compressed, rgba, nullptr);
				case TextureBlockCodec::BC7: return DecompressBlockBC7(compressed, rgba, nullptr);
				case TextureBlockCodec::BC4:
				{
					std::array<unsigned char, 16> red{};
					const int result = DecompressBlockBC4(compressed, red.data(), nullptr);
					for (size_t texel = 0; texel < red.size(); ++texel)
					{
						rgba[texel * 4] = red[texel];
						rgba[texel * 4 + 1] = 0;
						rgba[texel * 4 + 2] = 0;
						rgba[texel * 4 + 3] = 255;
					}
					return result;
				}
				case TextureBlockCodec::BC5:
				{
					std::array<unsigned char, 16> red{};
					std::array<unsigned char, 16> green{};
					const int result = DecompressBlockBC5(compressed, red.data(), green.data(), nullptr);
					for (size_t texel = 0; texel < red.size(); ++texel)
					{
						rgba[texel * 4] = red[texel];
						rgba[texel * 4 + 1] = green[texel];
						rgba[texel * 4 + 2] = 0;
						rgba[texel * 4 + 3] = 255;
					}
					return result;
				}
				case TextureBlockCodec::BC6H:
				{
					std::array<unsigned short, 48> rgb{};
					const int result = DecompressBlockBC6(compressed, rgb.data(), nullptr);
					constexpr uint16_t HalfOne = 0x3C00;
					for (size_t texel = 0; texel < 16; ++texel)
					{
						for (size_t channel = 0; channel < 3; ++channel)
							WriteUInt16(block.data() + texel * 8 + channel * 2, rgb[texel * 3 + channel]);
						WriteUInt16(block.data() + texel * 8 + 6, HalfOne);
					}
					return result;
				}
			}
			return CmpCoreUnknownError;
		}

	}

	std::string_view TextureBlockCodecToString(TextureBlockCodec codec)
	{
		switch (codec)
		{
			case TextureBlockCodec::BC1:  return "BC1";
			case TextureBlockCodec::BC4:  return "BC4";
			case TextureBlockCodec::BC5:  return "BC5";
			case TextureBlockCodec::BC6H: return "BC6H";
			case TextureBlockCodec::BC7:  return "BC7";
		}
		return "Unknown";
	}

	void InitializeTextureCodecs()
	{
		// DisableSIMD always succeeds (it only selects the scalar BC1 endpoint search).
		const int disabled = DisableSIMD();
		ENGINE_CORE_VERIFY(disabled == Utils::CmpCoreOk, "CMP_Core's DisableSIMD failed");
		// The BC7 codec builds its ramp tables on its first use; creating its options once builds them here, before any
		// import worker could build them concurrently.
		void* options = nullptr;
		const int created = CreateOptionsBC7(&options);
		ENGINE_CORE_VERIFY(created == Utils::CmpCoreOk && options != nullptr, "CMP_Core could not create its BC7 options");
		static_cast<void>(DestroyOptionsBC7(options));
	}

	Result<Buffer> CompressTextureBlocks(TextureBlockCodec codec, uint32_t width, uint32_t height, std::span<const std::byte> texels)
	{
		ENGINE_TRY(Utils::ValidateLevelSize(codec, width, height));
		const uint32_t texelBytes = GetTextureBlockTexelBytes(codec);
		const uint64_t expected = uint64_t{ width } * height * texelBytes;
		if (texels.size() != expected)
		{
			return MakeError(ErrorCode::InvalidArgument,
				"a {} x {} level has {} bytes of texels, not {}", width, height, texels.size(), expected);
		}
		if (codec == TextureBlockCodec::BC6H)
		{
			for (size_t texel = 0; texel < texels.size() / texelBytes; ++texel)
			{
				for (size_t channel = 0; channel < 3; ++channel)
				{
					const float value = Utils::HalfToFloat(Utils::ReadUInt16(texels.data() + texel * texelBytes + channel * 2));
					if (!std::isfinite(value) || value < 0.0f)
					{
						return MakeError(ErrorCode::InvalidArgument,
							"texel ({}, {}) has the value {} in channel {}: BC6H takes finite, non-negative values", texel % width,
							texel / width, value, channel);
					}
				}
			}
		}

		Utils::BlockCodecOptions options;
		ENGINE_TRY(options.Create(codec));
		const uint32_t columns = (width + 3) / 4;
		const uint32_t rows = (height + 3) / 4;
		const uint32_t blockBytes = GetTextureBlockBytes(codec);
		Buffer output(ComputeTextureBlockLevelSize(codec, width, height));
		std::array<std::byte, 16 * 8> block{};
		const std::span<std::byte> gathered(block.data(), size_t{ 16 } * texelBytes);
		for (uint32_t blockY = 0; blockY < rows; ++blockY)
		{
			for (uint32_t blockX = 0; blockX < columns; ++blockX)
			{
				Utils::GatherBlock(texels, width, height, texelBytes, blockX, blockY, gathered);
				const size_t offset = (static_cast<size_t>(blockY) * columns + blockX) * blockBytes;
				const int result = Utils::CompressBlock(codec, gathered, options, std::span<std::byte>(output.data() + offset, blockBytes));
				if (result != Utils::CmpCoreOk)
				{
					return MakeError(ErrorCode::ImportFailed,
						"CMP_Core failed to compress {} block ({}, {}) with error {}", TextureBlockCodecToString(codec), blockX, blockY,
						result);
				}
			}
		}
		return output;
	}

	Result<Buffer> DecompressTextureBlocks(TextureBlockCodec codec, uint32_t width, uint32_t height, std::span<const std::byte> blocks)
	{
		ENGINE_TRY(Utils::ValidateLevelSize(codec, width, height));
		const uint64_t expected = ComputeTextureBlockLevelSize(codec, width, height);
		if (blocks.size() != expected)
		{
			return MakeError(ErrorCode::InvalidArgument,
				"a {} level of {} x {} texels has {} bytes of blocks, not {}", TextureBlockCodecToString(codec), width, height,
				blocks.size(), expected);
		}

		const uint32_t texelBytes = GetTextureBlockTexelBytes(codec);
		const uint32_t columns = (width + 3) / 4;
		const uint32_t rows = (height + 3) / 4;
		const uint32_t blockBytes = GetTextureBlockBytes(codec);
		Buffer output(size_t{ width } * height * texelBytes);
		std::array<std::byte, 16 * 8> block{};
		const std::span<std::byte> decoded(block.data(), size_t{ 16 } * texelBytes);
		for (uint32_t blockY = 0; blockY < rows; ++blockY)
		{
			for (uint32_t blockX = 0; blockX < columns; ++blockX)
			{
				const size_t offset = (static_cast<size_t>(blockY) * columns + blockX) * blockBytes;
				const int result = Utils::DecompressBlock(codec, blocks.data() + offset, decoded);
				if (result != Utils::CmpCoreOk)
				{
					return MakeError(ErrorCode::InvalidArgument,
						"CMP_Core failed to decompress {} block ({}, {}) with error {}", TextureBlockCodecToString(codec), blockX, blockY,
						result);
				}
				for (uint32_t row = 0; row < 4 && blockY * 4 + row < height; ++row)
				{
					const uint32_t y = blockY * 4 + row;
					const uint32_t count = std::min(4U, width - blockX * 4);
					std::memcpy(output.data() + (static_cast<size_t>(y) * width + blockX * 4) * texelBytes,
						decoded.data() + static_cast<size_t>(row) * 4 * texelBytes, size_t{ count } * texelBytes);
				}
			}
		}
		return output;
	}

}
