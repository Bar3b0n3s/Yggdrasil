#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/EnvironmentImporter.h"

#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/IEnvironmentBaker.h"
#include "Engine/Asset/TextureCompression.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <stb_image.h>
#include <tinyexr.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

	namespace {

		// Releases pixels stb_image allocated.
		struct StbFloatDeleter
		{
			void operator()(float* pixels) const { stbi_image_free(pixels); }
		};

		// A header ParseEXRHeaderFromMemory fills, freed once with FreeEXRHeader.
		class ExrHeaderOwner
		{
		public:
			ExrHeaderOwner() { InitEXRHeader(&m_Header); }
			~ExrHeaderOwner()
			{
				const int freed = FreeEXRHeader(&m_Header);
				ENGINE_CORE_ASSERT(freed == TINYEXR_SUCCESS, "FreeEXRHeader fails only for a null header");
			}
			ExrHeaderOwner(const ExrHeaderOwner&) = delete;
			ExrHeaderOwner& operator=(const ExrHeaderOwner&) = delete;

			[[nodiscard]] EXRHeader& Get() { return m_Header; }
		private:
			EXRHeader m_Header{};
		};

		// An image LoadEXRImageFromMemory fills, freed with FreeEXRImage (which clears what it frees, so an image the
		// failed load already freed is freed again harmlessly).
		class ExrImageOwner
		{
		public:
			ExrImageOwner() { InitEXRImage(&m_Image); }
			~ExrImageOwner()
			{
				const int freed = FreeEXRImage(&m_Image);
				ENGINE_CORE_ASSERT(freed == TINYEXR_SUCCESS, "FreeEXRImage fails only for a null image");
			}
			ExrImageOwner(const ExrImageOwner&) = delete;
			ExrImageOwner& operator=(const ExrImageOwner&) = delete;

			[[nodiscard]] EXRImage& Get() { return m_Image; }
		private:
			EXRImage m_Image{};
		};

		// The error message a tinyexr function may allocate, freed with FreeEXRErrorMessage.
		class ExrErrorOwner
		{
		public:
			ExrErrorOwner() = default;
			~ExrErrorOwner() { FreeEXRErrorMessage(m_Message); }
			ExrErrorOwner(const ExrErrorOwner&) = delete;
			ExrErrorOwner& operator=(const ExrErrorOwner&) = delete;

			[[nodiscard]] const char** Out() { return &m_Message; }

			// The message without its trailing line breaks, or "unknown reason".
			[[nodiscard]] std::string GetText() const
			{
				std::string text = m_Message != nullptr ? std::string(m_Message) : std::string();
				while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
					text.pop_back();
				std::ranges::replace(text, '\n', ' ');
				return text.empty() ? std::string("unknown reason") : text;
			}
		private:
			const char* m_Message = nullptr;
		};

		// One level of a tiled OpenEXR image, in the order of its offset table.
		struct ExrTileLevel
		{
			uint64_t Width = 0;
			uint64_t Height = 0;
			uint64_t TilesX = 0;
			uint64_t TilesY = 0;
			int32_t LevelX = 0;
			int32_t LevelY = 0;
		};

	}

	namespace Utils {

		// The widest equirectangular source, as EnvironmentBaker accepts it (8k: a 1024² skybox face, the largest, needs no more
		// than a 4k source).
		constexpr uint32_t MaxEquirectWidth = 8192;
		// The range of EnvironmentImportSettings::MaxLuminance: 1 to the largest binary16 value.
		constexpr double MinClampLuminance = 1.0;
		constexpr double MaxClampLuminance = 65504.0;
		constexpr int RgbChannels = 3;
		constexpr std::array<std::string_view, 3> ChannelNames = { "red", "green", "blue" };
		constexpr std::array<std::string_view, 3> ExrChannelNames = { "R", "G", "B" };
		// The OpenEXR header's chunk size limits tinyexr applies too: the 8-byte magic number and version, a scanline
		// block's line number and size, a tile's four coordinates and size.
		constexpr uint64_t ExrVersionBytes = 8;
		constexpr uint64_t ExrScanlineChunkHeaderBytes = 8;
		constexpr uint64_t ExrTileChunkHeaderBytes = 20;
		// The attributes of a header tinyexr reads at most (TINYEXR_MAX_HEADER_ATTRIBUTES) and the longest attribute name or
		// type (255 bytes, OpenEXR's long names).
		constexpr size_t MaxExrAttributes = 1024;
		constexpr size_t MaxExrNameBytes = 256;

		[[nodiscard]] static bool IsOpenExrExtension(std::string_view extension)
		{
			constexpr std::string_view Exr = ".exr";
			return std::ranges::equal(extension, Exr, [](char character, char lower)
			{
				return (character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character) == lower;
			});
		}

		// stb_image's reason for its last failure on this thread (its failure reason is thread-local).
		[[nodiscard]] static std::string_view GetStbFailureReason()
		{
			const char* reason = stbi_failure_reason();
			return reason != nullptr ? std::string_view(reason) : std::string_view("unknown reason");
		}

		// The settings object of an import as the registered struct (null: the defaults); the registry enforces the field
		// ranges. Errors: Validation for settings the struct rejects; InvalidState for a registry without
		// EnvironmentImportSettings.
		[[nodiscard]] static Result<EnvironmentImportSettings> ReadSettings(const Json& settings, const TypeRegistry& registry)
		{
			EnvironmentImportSettings result;
			if (settings.is_null())
				return result;
			const StructInfo* type = registry.FindStruct<EnvironmentImportSettings>();
			if (type == nullptr)
				return MakeError(ErrorCode::InvalidState, "the type registry has no EnvironmentImportSettings (RegisterAssetPipelineTypes was not called)");
			ReadContext context;
			context.Strict = true;
			ENGINE_TRY(type->FromJson(&result, JsonReader(settings), context));
			return result;
		}

		// The equirectangular checks both formats apply to an image's size before any texel is decoded: twice as wide as
		// high, at most MaxEquirectWidth texels wide. Errors: ImportFailed naming the size, with a hint.
		[[nodiscard]] static Status CheckEquirectangularSize(int64_t width, int64_t height, std::string_view name)
		{
			if (width <= 0 || height <= 0 || width != 2 * height)
			{
				return std::unexpected(Error(ErrorCode::ImportFailed,
					std::format("'{}' is {}x{} texels; an equirectangular environment is exactly twice as wide as it is high", name, width, height))
						.WithHint("use a 2:1 equirectangular (latitude-longitude) HDRI"));
			}
			if (width > int64_t{ MaxEquirectWidth })
			{
				return std::unexpected(Error(ErrorCode::ImportFailed,
					std::format("'{}' is {}x{} texels; environments are limited to {} texels in width", name, width, height, MaxEquirectWidth))
						.WithHint("use the 8k or a smaller variant of the HDRI"));
			}
			return {};
		}

		// Every value of a decoded image finite and non-negative. Errors: ImportFailed naming the first offending texel.
		[[nodiscard]] static Status CheckBakeTexels(const EnvironmentBakeInput& input, std::string_view name)
		{
			const auto bad = std::ranges::find_if(input.Texels, [](float value)
			{
				return !std::isfinite(value) || value < 0.0f;
			});
			if (bad == input.Texels.end())
				return {};
			const size_t index = static_cast<size_t>(bad - input.Texels.begin());
			const size_t texel = index / RgbChannels;
			return MakeError(ErrorCode::ImportFailed, "'{}' has a non-finite or negative {} value ({}) at texel ({}, {})", name, ChannelNames[index % RgbChannels],
				*bad, texel % input.Width, texel / input.Width);
		}

		// Decodes a Radiance .hdr (stbi_loadf from memory, §8.6 step 1) into the bake's input: an equirectangular image twice as
		// wide as high, at most MaxEquirectWidth texels wide, every value finite and non-negative. The size is checked from the
		// header before any texel is decoded. Errors: ImportFailed, naming the first offending texel for bad values.
		[[nodiscard]] static Result<EnvironmentBakeInput> DecodeRadiance(std::span<const std::byte> encoded, std::string_view name)
		{
			if (encoded.empty())
				return MakeError(ErrorCode::ImportFailed, "'{}' is empty", name);
			if (encoded.size() > static_cast<size_t>(INT_MAX))
				return MakeError(ErrorCode::ImportFailed, "'{}' is {} bytes, too large to decode", name, encoded.size());
			const auto* data = reinterpret_cast<const stbi_uc*>(encoded.data());
			const int length = static_cast<int>(encoded.size());
			// stbi_loadf also decodes LDR images (converted to linear floats): only a Radiance header is an environment.
			if (stbi_is_hdr_from_memory(data, length) == 0)
			{
				return std::unexpected(Error(ErrorCode::ImportFailed, std::format("'{}' is not a Radiance HDR image (no #?RADIANCE header)", name))
						.WithHint("environments import Radiance .hdr and OpenEXR .exr equirectangular images"));
			}
			int width = 0;
			int height = 0;
			int channels = 0;
			if (stbi_info_from_memory(data, length, &width, &height, &channels) == 0)
				return MakeError(ErrorCode::ImportFailed, "'{}' is not a valid Radiance HDR image: {}", name, GetStbFailureReason());
			ENGINE_TRY(CheckEquirectangularSize(width, height, name));

			const std::unique_ptr<float, StbFloatDeleter> texels(stbi_loadf_from_memory(data, length, &width, &height, &channels, RgbChannels));
			if (texels == nullptr)
				return MakeError(ErrorCode::ImportFailed, "could not decode the Radiance HDR image '{}': {}", name, GetStbFailureReason());

			EnvironmentBakeInput input;
			input.Width = static_cast<uint32_t>(width);
			input.Height = static_cast<uint32_t>(height);
			const size_t valueCount = static_cast<size_t>(input.Width) * input.Height * RgbChannels;
			input.Texels.assign(texels.get(), texels.get() + valueCount);
			ENGINE_TRY(CheckBakeTexels(input, name));
			return input;
		}

		// The name of an OpenEXR compression byte (OpenEXR's names, ZFP tinyexr's extension).
		[[nodiscard]] static std::string DescribeExrCompression(int compression)
		{
			switch (compression)
			{
				case TINYEXR_COMPRESSIONTYPE_NONE:  return "NONE";
				case TINYEXR_COMPRESSIONTYPE_RLE:   return "RLE";
				case TINYEXR_COMPRESSIONTYPE_ZIPS:  return "ZIPS";
				case TINYEXR_COMPRESSIONTYPE_ZIP:   return "ZIP";
				case TINYEXR_COMPRESSIONTYPE_PIZ:   return "PIZ";
				case TINYEXR_COMPRESSIONTYPE_PXR24: return "PXR24";
				case TINYEXR_COMPRESSIONTYPE_B44:   return "B44";
				case TINYEXR_COMPRESSIONTYPE_B44A:  return "B44A";
				case TINYEXR_COMPRESSIONTYPE_DWAA:  return "DWAA";
				case TINYEXR_COMPRESSIONTYPE_DWAB:  return "DWAB";
				case TINYEXR_COMPRESSIONTYPE_ZFP:   return "ZFP";
				default:                            return std::format("an unknown ({})", compression);
			}
		}

		// The scanlines of one block of a supported compression (OpenEXR's: 1 for NONE, RLE and ZIPS, 16 for ZIP and PXR24, 32
		// for PIZ); none for the others, which the importer refuses.
		[[nodiscard]] static std::optional<uint32_t> GetExrLinesPerBlock(int compression)
		{
			switch (compression)
			{
				case TINYEXR_COMPRESSIONTYPE_NONE:
				case TINYEXR_COMPRESSIONTYPE_RLE:
				case TINYEXR_COMPRESSIONTYPE_ZIPS:  return 1;
				case TINYEXR_COMPRESSIONTYPE_ZIP:
				case TINYEXR_COMPRESSIONTYPE_PXR24: return 16;
				case TINYEXR_COMPRESSIONTYPE_PIZ:   return 32;
				default:                            return std::nullopt;
			}
		}

		// The little-endian unsigned integer of `byteCount` bytes at `offset` (the caller checked the bounds).
		[[nodiscard]] static uint64_t ReadLittleEndian(std::span<const std::byte> bytes, uint64_t offset, size_t byteCount)
		{
			uint64_t value = 0;
			for (size_t index = 0; index < byteCount; ++index)
				value |= uint64_t{ std::to_integer<uint8_t>(bytes[static_cast<size_t>(offset) + index]) } << (8 * index);
			return value;
		}

		// The little-endian signed 32-bit integer at `offset`.
		[[nodiscard]] static int32_t ReadLittleEndianI32(std::span<const std::byte> bytes, uint64_t offset)
		{
			return static_cast<int32_t>(static_cast<uint32_t>(ReadLittleEndian(bytes, offset, 4)));
		}

		// a * b, or the largest uint64_t when the product does not fit.
		[[nodiscard]] static uint64_t MultiplySaturating(uint64_t a, uint64_t b)
		{
			if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a)
				return std::numeric_limits<uint64_t>::max();
			return a * b;
		}

		// The compression byte of a single-part OpenEXR header (its "compression" attribute), read before tinyexr parses the
		// header, because tinyexr refuses the DWA and unknown compressions without naming them. None when the header holds
		// none or ends early (tinyexr then names the problem).
		[[nodiscard]] static std::optional<int> FindExrCompression(std::span<const std::byte> bytes)
		{
			uint64_t position = ExrVersionBytes;
			const auto readName = [&bytes, &position]() -> std::optional<std::string_view>
			{
				const size_t start = static_cast<size_t>(position);
				const size_t limit = std::min(bytes.size(), start + MaxExrNameBytes);
				for (size_t index = start; index < limit; ++index)
				{
					if (bytes[index] == std::byte{ 0 })
					{
						position = index + 1;
						return std::string_view(reinterpret_cast<const char*>(bytes.data()) + start, index - start);
					}
				}
				return std::nullopt;
			};
			for (size_t attribute = 0; attribute < MaxExrAttributes && position < bytes.size(); ++attribute)
			{
				if (bytes[static_cast<size_t>(position)] == std::byte{ 0 })
					return std::nullopt; // the end of the header
				const std::optional<std::string_view> attributeName = readName();
				if (!attributeName.has_value() || !readName().has_value() || position + 4 > bytes.size())
					return std::nullopt;
				const uint64_t valueBytes = ReadLittleEndian(bytes, position, 4);
				position += 4;
				if (valueBytes > bytes.size() - position)
					return std::nullopt;
				if (*attributeName == "compression" && valueBytes >= 1)
					return std::to_integer<int>(bytes[static_cast<size_t>(position)]);
				position += valueBytes;
			}
			return std::nullopt;
		}

		// tinyexr's LevelSize: a level's edge from the top level's, rounded down or up, at least 1.
		[[nodiscard]] static uint64_t GetExrLevelSize(uint64_t topLevel, uint32_t level, int rounding)
		{
			const uint64_t scale = uint64_t{ 1 } << level;
			uint64_t size = topLevel / scale;
			if (rounding == TINYEXR_TILE_ROUND_UP && size * scale < topLevel)
				++size;
			return std::max<uint64_t>(size, 1);
		}

		// tinyexr's RoundLog2: floor(log2(value)) rounding down, ceil(log2(value)) rounding up (value >= 1).
		[[nodiscard]] static uint32_t GetExrRoundLog2(uint64_t value, int rounding)
		{
			uint32_t log = 0;
			bool remainder = false;
			while (value > 1)
			{
				remainder = remainder || (value & 1U) != 0;
				++log;
				value >>= 1U;
			}
			return log + (rounding == TINYEXR_TILE_ROUND_UP && remainder ? 1U : 0U);
		}

		// The levels of a tiled image in its offset table's order (tinyexr's InitTileOffsets): one level; the mipmap levels
		// (l, l); or the ripmap levels (x, y), x fastest. Errors: ImportFailed for a tile size below 1 or an unknown level or
		// rounding mode.
		[[nodiscard]] static Result<std::vector<ExrTileLevel>> GetExrTileLevels(const EXRHeader& header, uint64_t width, uint64_t height, std::string_view name)
		{
			if (header.tile_size_x < 1 || header.tile_size_y < 1)
				return MakeError(ErrorCode::ImportFailed, "'{}' has {}x{} tiles; an OpenEXR tile is at least 1x1 texels", name, header.tile_size_x, header.tile_size_y);
			if (header.tile_rounding_mode != TINYEXR_TILE_ROUND_DOWN && header.tile_rounding_mode != TINYEXR_TILE_ROUND_UP)
				return MakeError(ErrorCode::ImportFailed, "'{}' has the unknown tile rounding mode {}", name, header.tile_rounding_mode);
			const auto tileX = static_cast<uint64_t>(header.tile_size_x);
			const auto tileY = static_cast<uint64_t>(header.tile_size_y);
			const int rounding = header.tile_rounding_mode;
			const auto makeLevel = [tileX, tileY, width, height, rounding](uint32_t levelX, uint32_t levelY)
			{
				const uint64_t levelWidth = GetExrLevelSize(width, levelX, rounding);
				const uint64_t levelHeight = GetExrLevelSize(height, levelY, rounding);
				return ExrTileLevel{ .Width = levelWidth,
					.Height = levelHeight,
					.TilesX = (levelWidth + tileX - 1) / tileX,
					.TilesY = (levelHeight + tileY - 1) / tileY,
					.LevelX = static_cast<int32_t>(levelX),
					.LevelY = static_cast<int32_t>(levelY) };
			};
			std::vector<ExrTileLevel> levels;
			switch (header.tile_level_mode)
			{
				case TINYEXR_TILE_ONE_LEVEL:
					levels.push_back(makeLevel(0, 0));
					break;
				case TINYEXR_TILE_MIPMAP_LEVELS:
				{
					const uint32_t count = GetExrRoundLog2(std::max(width, height), rounding) + 1;
					for (uint32_t level = 0; level < count; ++level)
						levels.push_back(makeLevel(level, level));
					break;
				}
				case TINYEXR_TILE_RIPMAP_LEVELS:
				{
					const uint32_t countX = GetExrRoundLog2(width, rounding) + 1;
					const uint32_t countY = GetExrRoundLog2(height, rounding) + 1;
					for (uint32_t levelY = 0; levelY < countY; ++levelY)
					{
						for (uint32_t levelX = 0; levelX < countX; ++levelX)
							levels.push_back(makeLevel(levelX, levelY));
					}
					break;
				}
				default:
					return MakeError(ErrorCode::ImportFailed, "'{}' has the unknown tile level mode {}", name, header.tile_level_mode);
			}
			return levels;
		}

		// Checks what tinyexr trusts before it decodes a pixel (the class comment): the decode budget (every channel as tinyexr
		// allocates it, `bytesPerPixel` bytes a texel, full tiles for a tiled image's every level), the chunkCount attribute,
		// and every entry of the offset table, each naming the chunk at its place: a scanline block that starts at its first
		// line, or a tile at its coordinates and level. So the chunks cover the image exactly once, and tinyexr writes every
		// texel it returns and none outside its buffers. Errors: ImportFailed naming the first violation.
		[[nodiscard]] static Status CheckExrChunks(std::span<const std::byte> bytes, const EXRHeader& header, uint64_t width, uint64_t height,
			uint32_t linesPerBlock, uint64_t bytesPerPixel, std::string_view name)
		{
			const uint64_t size = bytes.size();
			const uint64_t tableStart = ExrVersionBytes + header.header_len;
			const auto checkBudget = [&name](uint64_t decodedBytes) -> Status
			{
				if (decodedBytes <= EnvironmentImporter::MaxOpenExrDecodeBytes)
					return {};
				return std::unexpected(Error(ErrorCode::ImportFailed,
					std::format("'{}' would decode to more than {} MiB, its every channel decoded", name, EnvironmentImporter::MaxOpenExrDecodeBytes >> 20))
						.WithHint("save the environment with its R, G and B channels only"));
			};
			const auto checkChunkCount = [&header, &name](uint64_t chunks) -> Status
			{
				if (header.chunk_count > 0 && static_cast<uint64_t>(header.chunk_count) != chunks)
					return MakeError(ErrorCode::ImportFailed, "'{}' declares {} chunks (chunkCount), but its image has {}", name, header.chunk_count, chunks);
				return {};
			};
			// The offset table entry `index` of `entries`, which must point at a chunk header of `headerBytes` in the file,
			// after the table.
			const auto readOffset = [&bytes, size, tableStart, &name](uint64_t index, uint64_t entries, uint64_t headerBytes) -> Result<uint64_t>
			{
				const uint64_t tableEnd = tableStart + entries * 8;
				const uint64_t offset = ReadLittleEndian(bytes, tableStart + index * 8, 8);
				if (offset < tableEnd || offset > size || size - offset < headerBytes)
					return MakeError(ErrorCode::ImportFailed, "'{}' has the chunk offset {} at offset table entry {}, outside the chunks of its {} bytes", name, offset, index, size);
				return offset;
			};

			if (header.tiled == 0)
			{
				const uint64_t blocks = (height + linesPerBlock - 1) / linesPerBlock;
				ENGINE_TRY(checkBudget(MultiplySaturating(MultiplySaturating(width, height), bytesPerPixel)));
				ENGINE_TRY(checkChunkCount(blocks));
				if (tableStart > size || (size - tableStart) / 8 < blocks)
					return MakeError(ErrorCode::ImportFailed, "'{}' ends inside its offset table of {} scanline blocks", name, blocks);
				for (uint64_t block = 0; block < blocks; ++block)
				{
					ENGINE_TRY_ASSIGN(const uint64_t offset, readOffset(block, blocks, ExrScanlineChunkHeaderBytes));
					const int64_t line = ReadLittleEndianI32(bytes, offset);
					const int64_t expected = int64_t{ header.data_window.min_y } + static_cast<int64_t>(block * linesPerBlock);
					if (line != expected)
					{
						return MakeError(ErrorCode::ImportFailed, "'{}' has a scanline block starting at line {} at offset table entry {}, where the block of line {} belongs",
							name, line, block, expected);
					}
				}
				return {};
			}

			ENGINE_TRY_ASSIGN(const std::vector<ExrTileLevel> levels, GetExrTileLevels(header, width, height, name));
			const auto tileTexels = MultiplySaturating(static_cast<uint64_t>(header.tile_size_x), static_cast<uint64_t>(header.tile_size_y));
			uint64_t tiles = 0;
			uint64_t decodedBytes = 0;
			for (const ExrTileLevel& level : levels)
			{
				const uint64_t levelTiles = MultiplySaturating(level.TilesX, level.TilesY);
				tiles = levelTiles > std::numeric_limits<uint64_t>::max() - tiles ? std::numeric_limits<uint64_t>::max() : tiles + levelTiles;
				const uint64_t levelBytes = MultiplySaturating(MultiplySaturating(levelTiles, tileTexels), bytesPerPixel);
				decodedBytes = levelBytes > std::numeric_limits<uint64_t>::max() - decodedBytes ? std::numeric_limits<uint64_t>::max() : decodedBytes + levelBytes;
			}
			ENGINE_TRY(checkBudget(decodedBytes));
			ENGINE_TRY(checkChunkCount(tiles));
			if (tableStart > size || (size - tableStart) / 8 < tiles)
				return MakeError(ErrorCode::ImportFailed, "'{}' ends inside its offset table of {} tiles", name, tiles);
			uint64_t entry = 0;
			for (const ExrTileLevel& level : levels)
			{
				for (uint64_t tileY = 0; tileY < level.TilesY; ++tileY)
				{
					for (uint64_t tileX = 0; tileX < level.TilesX; ++tileX)
					{
						ENGINE_TRY_ASSIGN(const uint64_t offset, readOffset(entry, tiles, ExrTileChunkHeaderBytes));
						const std::array<int64_t, 4> found = { ReadLittleEndianI32(bytes, offset), ReadLittleEndianI32(bytes, offset + 4),
							ReadLittleEndianI32(bytes, offset + 8), ReadLittleEndianI32(bytes, offset + 12) };
						const std::array<int64_t, 4> expected = { static_cast<int64_t>(tileX), static_cast<int64_t>(tileY), level.LevelX, level.LevelY };
						if (found != expected)
						{
							return MakeError(ErrorCode::ImportFailed, "'{}' has the tile ({}, {}) of level ({}, {}) at offset table entry {}, where tile ({}, {}) of level ({}, {}) belongs",
								name, found[0], found[1], found[2], found[3], entry, expected[0], expected[1], expected[2], expected[3]);
						}
						++entry;
					}
				}
			}
			return {};
		}

		// The value at `index` of a decoded channel of `pixelType` (FLOAT, or UINT converted to the nearest float).
		[[nodiscard]] static float ReadExrValue(const unsigned char* channel, size_t index, int pixelType)
		{
			if (pixelType == TINYEXR_PIXELTYPE_UINT)
			{
				uint32_t value = 0;
				std::memcpy(&value, channel + index * sizeof(value), sizeof(value));
				return static_cast<float>(value);
			}
			float value = 0.0f;
			std::memcpy(&value, channel + index * sizeof(value), sizeof(value));
			return value;
		}

		// Decodes an OpenEXR image through tinyexr into the bake's input (the class comment: what is supported, and the checks
		// made from the header before any pixel is decoded). Errors: ImportFailed naming what is unsupported or broken, or the
		// first non-finite or negative texel.
		[[nodiscard]] static Result<EnvironmentBakeInput> DecodeOpenExr(std::span<const std::byte> encoded, std::string_view name)
		{
			if (encoded.empty())
				return MakeError(ErrorCode::ImportFailed, "'{}' is empty", name);
			const auto* data = reinterpret_cast<const unsigned char*>(encoded.data());
			EXRVersion version{};
			if (ParseEXRVersionFromMemory(&version, data, encoded.size()) != TINYEXR_SUCCESS)
			{
				return std::unexpected(Error(ErrorCode::ImportFailed, std::format("'{}' is not an OpenEXR image (no OpenEXR magic number and version 2)", name))
						.WithHint("environments import OpenEXR .exr and Radiance .hdr equirectangular images"));
			}
			constexpr std::string_view SinglePartHint = "save the environment as a single-part scanline or tiled OpenEXR image";
			if (version.multipart != 0)
			{
				return std::unexpected(Error(ErrorCode::ImportFailed, std::format("'{}' is a multi-part OpenEXR image, which is not supported", name))
						.WithHint(std::string(SinglePartHint)));
			}
			if (version.non_image != 0)
			{
				return std::unexpected(Error(ErrorCode::ImportFailed, std::format("'{}' is a deep OpenEXR image, which is not supported", name))
						.WithHint(std::string(SinglePartHint)));
			}
			constexpr std::string_view CompressionHint = "save the environment with ZIP, PIZ or no compression";
			if (const std::optional<int> compression = FindExrCompression(encoded); compression.has_value() && !GetExrLinesPerBlock(*compression).has_value())
			{
				return std::unexpected(Error(ErrorCode::ImportFailed,
					std::format("'{}' uses {} compression, which is not supported", name, DescribeExrCompression(*compression)))
						.WithHint(std::string(CompressionHint)));
			}

			ExrHeaderOwner headerOwner;
			ExrErrorOwner headerError;
			if (ParseEXRHeaderFromMemory(&headerOwner.Get(), &version, data, encoded.size(), headerError.Out()) != TINYEXR_SUCCESS)
				return MakeError(ErrorCode::ImportFailed, "'{}' has an invalid OpenEXR header: {}", name, headerError.GetText());
			EXRHeader& header = headerOwner.Get();
			const std::optional<uint32_t> linesPerBlock = GetExrLinesPerBlock(header.compression_type);
			if (!linesPerBlock.has_value())
			{
				return std::unexpected(Error(ErrorCode::ImportFailed,
					std::format("'{}' uses {} compression, which is not supported", name, DescribeExrCompression(header.compression_type)))
						.WithHint(std::string(CompressionHint)));
			}
			const int64_t width = int64_t{ header.data_window.max_x } - header.data_window.min_x + 1;
			const int64_t height = int64_t{ header.data_window.max_y } - header.data_window.min_y + 1;
			ENGINE_TRY(CheckEquirectangularSize(width, height, name));

			// The R, G and B channels, each read as 32-bit values (HALF requested as FLOAT); the decode budget counts every
			// channel as tinyexr allocates it.
			std::array<int, RgbChannels> channelIndices = { -1, -1, -1 };
			std::vector<std::string_view> channelNames;
			uint64_t bytesPerPixel = 0;
			for (int channel = 0; channel < header.num_channels; ++channel)
			{
				const EXRChannelInfo& info = header.channels[channel];
				const char* nameEnd = std::find(std::begin(info.name), std::end(info.name), '\0');
				const std::string_view channelName(info.name, static_cast<size_t>(nameEnd - std::begin(info.name)));
				channelNames.push_back(channelName);
				if (header.pixel_types[channel] < TINYEXR_PIXELTYPE_UINT || header.pixel_types[channel] > TINYEXR_PIXELTYPE_FLOAT)
					return MakeError(ErrorCode::ImportFailed, "'{}' has the channel '{}' of the unknown pixel type {}", name, channelName, header.pixel_types[channel]);
				// tinyexr lays every channel out at full resolution, so a subsampled one, of any name, would misread the chunks.
				if (info.x_sampling != 1 || info.y_sampling != 1)
				{
					return MakeError(ErrorCode::ImportFailed, "'{}' samples its channel '{}' every {}x{} texels; environments read channels of every texel", name,
						channelName, info.x_sampling, info.y_sampling);
				}
				for (int rgb = 0; rgb < RgbChannels; ++rgb)
				{
					if (channelIndices[static_cast<size_t>(rgb)] < 0 && channelName == ExrChannelNames[static_cast<size_t>(rgb)])
					{
						channelIndices[static_cast<size_t>(rgb)] = channel;
						if (header.pixel_types[channel] == TINYEXR_PIXELTYPE_HALF)
							header.requested_pixel_types[channel] = TINYEXR_PIXELTYPE_FLOAT;
					}
				}
				bytesPerPixel += header.requested_pixel_types[channel] == TINYEXR_PIXELTYPE_HALF ? 2 : 4;
			}
			for (int rgb = 0; rgb < RgbChannels; ++rgb)
			{
				const int index = channelIndices[static_cast<size_t>(rgb)];
				if (index < 0)
				{
					std::string found;
					for (const std::string_view channelName : channelNames)
						found += std::format("{}'{}'", found.empty() ? "" : ", ", channelName);
					return std::unexpected(Error(ErrorCode::ImportFailed, std::format("'{}' has no {} channel '{}' (its channels: {})", name, ChannelNames[static_cast<size_t>(rgb)], ExrChannelNames[static_cast<size_t>(rgb)], found.empty() ? std::string("none") : found))
							.WithHint("save the environment as an RGB image whose channels are named R, G and B"));
				}
			}
			ENGINE_TRY(CheckExrChunks(encoded, header, static_cast<uint64_t>(width), static_cast<uint64_t>(height), *linesPerBlock, bytesPerPixel, name));

			ExrImageOwner imageOwner;
			ExrErrorOwner loadError;
			if (LoadEXRImageFromMemory(&imageOwner.Get(), &header, data, encoded.size(), loadError.Out()) != TINYEXR_SUCCESS)
				return MakeError(ErrorCode::ImportFailed, "could not decode the OpenEXR image '{}': {}", name, loadError.GetText());
			const EXRImage& image = imageOwner.Get();

			EnvironmentBakeInput input;
			input.Width = static_cast<uint32_t>(width);
			input.Height = static_cast<uint32_t>(height);
			input.Texels.assign(static_cast<size_t>(input.Width) * input.Height * RgbChannels, 0.0f);
			const auto store = [&input, &header, &channelIndices](unsigned char* const* channels, size_t source, size_t texel) -> bool
			{
				for (size_t rgb = 0; rgb < static_cast<size_t>(RgbChannels); ++rgb)
				{
					const int index = channelIndices[rgb];
					if (channels[index] == nullptr)
						return false;
					input.Texels[texel * RgbChannels + rgb] = ReadExrValue(channels[index], source, header.pixel_types[index]);
				}
				return true;
			};
			if (header.tiled == 0)
			{
				if (image.images == nullptr || image.width != width || image.height != height)
					return MakeError(ErrorCode::ImportFailed, "could not decode the OpenEXR image '{}': tinyexr returned no scanlines", name);
				for (size_t texel = 0; texel < static_cast<size_t>(input.Width) * input.Height; ++texel)
				{
					if (!store(image.images, texel, texel))
						return MakeError(ErrorCode::ImportFailed, "could not decode the OpenEXR image '{}': tinyexr returned no channel data", name);
				}
			}
			else
			{
				if (image.tiles == nullptr || image.level_x != 0 || image.level_y != 0)
					return MakeError(ErrorCode::ImportFailed, "could not decode the OpenEXR image '{}': tinyexr returned no tiles of its top level", name);
				const auto tileWidth = static_cast<size_t>(header.tile_size_x);
				const auto tileHeight = static_cast<size_t>(header.tile_size_y);
				for (int tileIndex = 0; tileIndex < image.num_tiles; ++tileIndex)
				{
					const EXRTile& tile = image.tiles[tileIndex];
					if (tile.images == nullptr || tile.offset_x < 0 || tile.offset_y < 0 || tile.width < 0 || tile.height < 0
						|| static_cast<size_t>(tile.width) > tileWidth || static_cast<size_t>(tile.height) > tileHeight)
					{
						return MakeError(ErrorCode::ImportFailed, "could not decode the OpenEXR image '{}': tinyexr returned the malformed tile {}", name, tileIndex);
					}
					for (size_t row = 0; row < static_cast<size_t>(tile.height); ++row)
					{
						const size_t y = static_cast<size_t>(tile.offset_y) * tileHeight + row;
						for (size_t column = 0; column < static_cast<size_t>(tile.width); ++column)
						{
							const size_t x = static_cast<size_t>(tile.offset_x) * tileWidth + column;
							if (x >= input.Width || y >= input.Height)
								continue;
							if (!store(tile.images, column + row * tileWidth, x + y * input.Width))
								return MakeError(ErrorCode::ImportFailed, "could not decode the OpenEXR image '{}': tinyexr returned no channel data", name);
						}
					}
				}
			}
			ENGINE_TRY(CheckBakeTexels(input, name));
			return input;
		}

		// `cube` (Rgba16Float) as BC6H blocks: every face of every level compressed with CompressTextureBlocks. Errors:
		// those of CompressTextureBlocks, naming the level, face and cube (`cubeName`).
		[[nodiscard]] static Result<CubeMapData> CompressCube(const CubeMapData& cube, std::string_view cubeName)
		{
			CubeMapData compressed{ .FaceSize = cube.FaceSize, .MipCount = cube.MipCount, .Format = CubeMapFormat::BC6HUfloat, .Texels = {} };
			compressed.Texels.reserve(ComputeCubeMapByteSize(cube.FaceSize, cube.MipCount, CubeMapFormat::BC6HUfloat));
			const std::span<const std::byte> texels(cube.Texels);
			size_t offset = 0;
			for (uint32_t level = 0; level < cube.MipCount; ++level)
			{
				const uint32_t edge = std::max(1U, cube.FaceSize >> level);
				const size_t faceBytes = ComputeCubeMapFaceByteSize(cube.FaceSize, level, CubeMapFormat::Rgba16Float);
				for (uint32_t face = 0; face < CubeMapData::FaceCount; ++face)
				{
					Result<Buffer> blocks = CompressTextureBlocks(TextureBlockCodec::BC6H, edge, edge, texels.subspan(offset, faceBytes));
					if (!blocks.has_value())
						return std::unexpected(std::move(blocks).error().WithContext(std::format("while compressing mip {}, face {} of the {}", level, face, cubeName)));
					compressed.Texels.insert(compressed.Texels.end(), blocks->begin(), blocks->end());
					offset += faceBytes;
				}
			}
			return compressed;
		}

	}

	std::span<const std::string_view> EnvironmentImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 2> Extensions = { ".hdr", ".exr" };
		return Extensions;
	}

	Result<ImportResult> EnvironmentImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		const std::string sourcePath = context.GetSourcePath().ToString();
		ENGINE_TRY_ASSIGN(const EnvironmentImportSettings settings, WithContext(Utils::ReadSettings(context.GetSettings(), context.GetRegistry()), std::format("while reading the import settings of '{}'", sourcePath)));
		Result<EnvironmentBakeInput> decoded = Utils::IsOpenExrExtension(context.GetSourcePath().GetExtension())
			? Utils::DecodeOpenExr(context.GetSourceBytes(), sourcePath)
			: Utils::DecodeRadiance(context.GetSourceBytes(), sourcePath);
		if (!decoded.has_value())
			return std::unexpected(std::move(decoded).error());
		EnvironmentBakeInput input = std::move(*decoded);
		input.ClampLuminance = settings.ClampLuminance;
		input.ClampLuminanceMax = settings.MaxLuminance;

		// The bake needs a GPU (§8.6). An editor without one is served a cooked bake under the same cache key before the
		// importer is ever called (EditorAssetManager, §7.4), so reaching this point means no cache holds it.
		IEnvironmentBaker* baker = context.GetEnvironmentBaker();
		if (baker == nullptr)
		{
			return std::unexpected(Error(ErrorCode::Unsupported,
				std::format("'{}' needs a GPU bake, and no cooked bake of it exists: this editor has no graphics device (--renderer none or no Vulkan device)",
					sourcePath))
					.WithHint(std::string(EnvironmentImporter::GpuHint)));
		}
		ENGINE_TRY_ASSIGN(EnvironmentData environment, WithContext(baker->Bake(input), std::format("while baking '{}'", sourcePath)));
		ENGINE_TRY(WithContext(ValidateEnvironmentData(environment), std::format("in the bake of '{}'", sourcePath)));
		// M23 (decision 14): Auto stores both cubes as BC6H; the SH9 irradiance is the bake's either way.
		if (settings.Compression == TextureCompression::Auto)
		{
			ENGINE_TRY_ASSIGN(environment.Skybox, WithContext(Utils::CompressCube(environment.Skybox, "skybox"), std::format("in the import of '{}'", sourcePath)));
			ENGINE_TRY_ASSIGN(environment.Specular, WithContext(Utils::CompressCube(environment.Specular, "specular cube"), std::format("in the import of '{}'", sourcePath)));
			ENGINE_TRY(WithContext(ValidateEnvironmentData(environment), std::format("in the compressed bake of '{}'", sourcePath)));
		}

		ImportResult result;
		result.Artifacts.push_back(ImportedArtifact{
			.Handle = metadata.Handle,
			.Type = AssetType::Environment,
			.SubAssetKey = {},
			.Cooked = CookEnvironment(environment, Version),
		});
		return result;
	}

	void EnvironmentImporter::RegisterTypes(TypeRegistry& registry)
	{
		registry.Struct<EnvironmentImportSettings>("EnvironmentImportSettings", "How a Radiance .hdr or OpenEXR .exr equirectangular image is baked into an environment.")
			.Field("ClampLuminance", &EnvironmentImportSettings::ClampLuminance,
				"Whether texels brighter than MaxLuminance (Rec. 709 luminance) are scaled down to it before baking; off by default, since the bake's "
				"filtered importance sampling already handles bright suns.")
			.Field("MaxLuminance", &EnvironmentImportSettings::MaxLuminance, "The luminance ClampLuminance scales brighter texels down to.",
				{ .Min = Utils::MinClampLuminance, .Max = Utils::MaxClampLuminance })
			// M23: the field's text is frozen by Docs/Decisions/0034-m23-contract.md decision 18.
			.Field("Compression", &EnvironmentImportSettings::Compression, "Whether the environment's cubes are stored as BC6H.");
	}

}
