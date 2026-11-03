#include "TestsPCH.h"

#include "Engine/Asset/TextureData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/TextureCompression.h"
#include "Engine/Core/Random.h"
#include "Support/Fuzz.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace Engine {

	namespace {

		// A width x height texture with the full mip chain, byte values a function of position and level (any bytes are
		// valid blocks of a block-compressed format).
		TextureData MakeTexture(uint32_t width, uint32_t height, TextureFormat format = TextureFormat::RGBA8Srgb)
		{
			TextureData texture;
			texture.Format = format;
			texture.Width = width;
			texture.Height = height;
			uint64_t offset = 0;
			for (uint32_t level = 0; level < ComputeFullMipCount(width, height); ++level)
			{
				const uint32_t mipWidth = std::max(1u, width >> level);
				const uint32_t mipHeight = std::max(1u, height >> level);
				const uint64_t size = ComputeTextureLevelSize(format, mipWidth, mipHeight);
				texture.Mips.push_back({ .Width = mipWidth, .Height = mipHeight, .Offset = offset, .Size = size });
				for (uint64_t index = 0; index < size; ++index)
					texture.Pixels.push_back(static_cast<std::byte>((index * 13 + level * 71) & 0xFF));
				offset += size;
			}
			return texture;
		}

		// The error code of a failed result; nullopt for a success.
		template<typename T>
		std::optional<ErrorCode> GetErrorCode(const Result<T>& result)
		{
			if (result.has_value())
				return std::nullopt;
			return result.error().GetCode();
		}

		// The same texture with its base level only.
		TextureData MakeBaseLevel(uint32_t width, uint32_t height, TextureFormat format = TextureFormat::RGBA8Unorm)
		{
			TextureData texture = MakeTexture(width, height, format);
			texture.Pixels.resize(static_cast<size_t>(texture.Mips.front().Size));
			texture.Mips.resize(1);
			return texture;
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("TextureData: mip counts and bytes per pixel follow the base size and format")
		{
			CHECK(ComputeFullMipCount(1, 1) == 1);
			CHECK(ComputeFullMipCount(8, 8) == 4);
			CHECK(ComputeFullMipCount(16, 3) == 5);
			CHECK(ComputeFullMipCount(16384, 1) == 15);
			CHECK(ComputeFullMipCount(0, 7) == 3);
			CHECK(ComputeFullMipCount(0, 0) == 0);
			CHECK(GetTextureBytesPerPixel(TextureFormat::RGBA8Unorm) == 4);
			CHECK(GetTextureBytesPerPixel(TextureFormat::RGBA8Srgb) == 4);
			CHECK(GetTextureBytesPerPixel(TextureFormat::R8Unorm) == 1);
			CHECK(TextureFormatToString(TextureFormat::RGBA8Srgb) == "RGBA8Srgb");
			CHECK(TextureFormatToString(static_cast<TextureFormat>(0)) == "Unknown");
		}

		// M23 (C23.1, R-TEXC-BC; Docs/Decisions/0034-m23-contract.md decision 13): the block-compressed formats, persisted with
		// the values the contract froze, name their codec, have no bytes per pixel and size their levels by whole 4 x 4 blocks.
		TEST_CASE("TextureData: the block-compressed formats name their codec and size their levels by blocks")
		{
			struct BlockFormat
			{
				TextureFormat Format = TextureFormat::RGBA8Unorm;
				uint8_t Value = 0;
				std::string_view Name{};
				TextureBlockCodec Codec = TextureBlockCodec::BC1;
				uint64_t FiveByThreeBytes = 0; // a 5 x 3 level: 2 x 1 blocks
			};
			const std::array<BlockFormat, 6> formats = { {
				{ TextureFormat::BC1Unorm, 4, "BC1Unorm", TextureBlockCodec::BC1, 16 },
				{ TextureFormat::BC1Srgb, 5, "BC1Srgb", TextureBlockCodec::BC1, 16 },
				{ TextureFormat::BC4Unorm, 6, "BC4Unorm", TextureBlockCodec::BC4, 16 },
				{ TextureFormat::BC5Unorm, 7, "BC5Unorm", TextureBlockCodec::BC5, 32 },
				{ TextureFormat::BC7Unorm, 8, "BC7Unorm", TextureBlockCodec::BC7, 32 },
				{ TextureFormat::BC7Srgb, 9, "BC7Srgb", TextureBlockCodec::BC7, 32 },
			} };
			for (const BlockFormat& format : formats)
			{
				CAPTURE(std::string(format.Name));
				CHECK(std::to_underlying(format.Format) == format.Value);
				CHECK(TextureFormatToString(format.Format) == format.Name);
				CHECK(GetTextureBytesPerPixel(format.Format) == 0);
				CHECK(GetTextureBlockCodec(format.Format) == format.Codec);
				CHECK(ComputeTextureLevelSize(format.Format, 5, 3) == format.FiveByThreeBytes);
				CHECK(ComputeTextureLevelSize(format.Format, 1, 1) == GetTextureBlockBytes(format.Codec));
			}
			for (const TextureFormat format : { TextureFormat::RGBA8Unorm, TextureFormat::RGBA8Srgb, TextureFormat::R8Unorm })
				CHECK_FALSE(GetTextureBlockCodec(format).has_value());
			CHECK(ComputeTextureLevelSize(TextureFormat::RGBA8Srgb, 5, 3) == 60);
			CHECK(ComputeTextureLevelSize(TextureFormat::R8Unorm, 5, 3) == 15);
			CHECK_FALSE(GetTextureBlockCodec(static_cast<TextureFormat>(10)).has_value());
			CHECK(ComputeTextureLevelSize(static_cast<TextureFormat>(10), 5, 3) == 0);
		}

		TEST_CASE("TextureData: the payload round-trips with a full mip chain")
		{
			const TextureData texture = MakeTexture(16, 4);
			REQUIRE(ValidateTextureData(texture).has_value());
			const Buffer payload = SerializeTexturePayload(texture);
			Result<TextureData> read = DeserializeTexturePayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Format == texture.Format);
			CHECK(read->Width == 16);
			CHECK(read->Height == 4);
			CHECK(read->Mips == texture.Mips);
			CHECK(read->Pixels == texture.Pixels);
			CHECK(GetMipPixels(*read, 2).size() == 4 * 1 * 4);

			const Buffer cooked = CookTexture(texture, 1);
			Result<AssetRef<TextureData>> loaded = LoadCookedTexture(cooked);
			REQUIRE(loaded.has_value());
			CHECK(SerializeTexturePayload(**loaded) == payload);
		}

		TEST_CASE("TextureData: the payload layout is the documented one")
		{
			const TextureData texture = MakeBaseLevel(2, 1, TextureFormat::R8Unorm);
			const Buffer payload = SerializeTexturePayload(texture);
			// uint8 Format, 3 reserved bytes, Width, Height, MipCount, one { Width, Height, uint64 Size }, 2 texels.
			REQUIRE(payload.size() == 4 + 4 + 4 + 4 + 16 + 2);
			CHECK(std::to_integer<uint8_t>(payload[0]) == 3);
			CHECK(std::to_integer<uint8_t>(payload[1]) == 0);
			CHECK(std::to_integer<uint8_t>(payload[4]) == 2);  // width, little-endian
			CHECK(std::to_integer<uint8_t>(payload[8]) == 1);  // height
			CHECK(std::to_integer<uint8_t>(payload[12]) == 1); // one mip level
			CHECK(std::to_integer<uint8_t>(payload[24]) == 2); // its size
			CHECK(payload[32] == texture.Pixels[0]);
			CHECK(payload[33] == texture.Pixels[1]);
		}

		TEST_CASE("TextureData: every mip level views its own texels")
		{
			const TextureData texture = MakeTexture(8, 2, TextureFormat::RGBA8Unorm);
			REQUIRE(texture.Mips.size() == 4);
			size_t total = 0;
			for (uint32_t level = 0; level < texture.Mips.size(); ++level)
			{
				const std::span<const std::byte> pixels = GetMipPixels(texture, level);
				CHECK(pixels.size() == texture.Mips[level].Size);
				CHECK(pixels.data() == texture.Pixels.data() + texture.Mips[level].Offset);
				total += pixels.size();
			}
			CHECK(total == texture.Pixels.size());

			const TextureData single = MakeBaseLevel(3, 5);
			REQUIRE(ValidateTextureData(single).has_value());
			CHECK(GetMipPixels(single, 0).size() == 3 * 5 * 4);
		}

		TEST_CASE("TextureData: invalid textures fail validation")
		{
			TextureData empty;
			CHECK_FALSE(ValidateTextureData(empty).has_value());
			TextureData tooLarge = MakeTexture(1, 1);
			tooLarge.Width = MaxTextureDimension + 1;
			CHECK_FALSE(ValidateTextureData(tooLarge).has_value());
			TextureData partialChain = MakeTexture(8, 8);
			partialChain.Mips.pop_back();
			CHECK_FALSE(ValidateTextureData(partialChain).has_value());
			TextureData shortPixels = MakeTexture(8, 8);
			shortPixels.Pixels.pop_back();
			CHECK_FALSE(ValidateTextureData(shortPixels).has_value());
			// Values outside the enumeration (9 was one until M23 appended the BC formats, 4 to 9).
			for (const int value : { 0, 10, 255 })
			{
				CAPTURE(value);
				TextureData unknownFormat = MakeTexture(2, 2);
				unknownFormat.Format = static_cast<TextureFormat>(static_cast<uint8_t>(value));
				CHECK(GetErrorCode(ValidateTextureData(unknownFormat)) == ErrorCode::Validation);
			}

			TextureData wrongLevelSize = MakeTexture(4, 4);
			wrongLevelSize.Mips[1].Width = 3;
			CHECK(GetErrorCode(ValidateTextureData(wrongLevelSize)) == ErrorCode::Validation);
			TextureData gap = MakeTexture(4, 4);
			gap.Mips[2].Offset += 1;
			CHECK(GetErrorCode(ValidateTextureData(gap)) == ErrorCode::Validation);
		}

		TEST_CASE("TextureData: reserved bytes, truncation and trailing bytes fail with Parse")
		{
			const Buffer payload = SerializeTexturePayload(MakeTexture(4, 2));
			Buffer reserved = payload;
			reserved[2] = std::byte{ 1 };
			CHECK(GetErrorCode(DeserializeTexturePayload(reserved)) == ErrorCode::Parse);
			const Buffer truncated(payload.begin(), payload.end() - 1);
			CHECK(GetErrorCode(DeserializeTexturePayload(truncated)) == ErrorCode::Parse);
			Buffer trailing = payload;
			trailing.push_back(std::byte{ 0 });
			CHECK(GetErrorCode(DeserializeTexturePayload(trailing)) == ErrorCode::Parse);
			// A mip count far beyond the bytes present is rejected before anything is allocated.
			Buffer hugeCount = payload;
			hugeCount[12] = std::byte{ 0xFF };
			hugeCount[13] = std::byte{ 0xFF };
			hugeCount[14] = std::byte{ 0xFF };
			hugeCount[15] = std::byte{ 0x7F };
			CHECK(GetErrorCode(DeserializeTexturePayload(hugeCount)) == ErrorCode::Parse);

			// A cooked artifact of another type is not a texture.
			const Buffer other = WriteCookedArtifact(AssetType::Font, TextureData::FormatVersion, 1, payload);
			CHECK_FALSE(LoadCookedTexture(other).has_value());
		}

		TEST_CASE("TextureData: 10,000 seeded mutations of a payload never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			// M23 (Docs/Decisions/0034-m23-contract.md decision 19): the seeds hold uncompressed and block-compressed payloads, and
			// every block-compressed level of a payload the reader accepts is decompressed too, so CMP_Core's decoders see every
			// mutation that reaches them.
			const std::array<Buffer, 3> payloads = { SerializeTexturePayload(MakeTexture(8, 8, TextureFormat::R8Unorm)),
				SerializeTexturePayload(MakeTexture(5, 3, TextureFormat::BC7Srgb)), SerializeTexturePayload(MakeTexture(8, 4, TextureFormat::BC4Unorm)) };
			const Test::FuzzRun run = Test::RunFuzzCase(0x7E47, 10000, [&payloads](Random& random, uint64_t /*index*/)
			{
				Buffer mutated = payloads[static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(payloads.size()) - 1))];
				Test::MutateBytes(mutated, random);
				// A payload the reader accepts is a valid texture.
				const Result<TextureData> read = DeserializeTexturePayload(mutated);
				if (!read.has_value())
					return false;
				CHECK(ValidateTextureData(*read).has_value());
				if (const std::optional<TextureBlockCodec> codec = GetTextureBlockCodec(read->Format))
				{
					for (uint32_t level = 0; level < read->Mips.size(); ++level)
					{
						const TextureMip& mip = read->Mips[level];
						const Result<Buffer> texels = DecompressTextureBlocks(*codec, mip.Width, mip.Height, GetMipPixels(*read, level));
						REQUIRE_MESSAGE(texels.has_value(), texels.error().ToString());
						CHECK(texels->size() == uint64_t{ mip.Width } * mip.Height * GetTextureBlockTexelBytes(*codec));
					}
				}
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
			{
				CHECK(run.Counts.Rejected > 0);
				CHECK(run.Counts.Accepted > 0);
			}
		}

		// M23 (C23.1, R-TEXC-BC; Docs/Decisions/0034-m23-contract.md decision 13, stream X): TextureData version 2 takes the BC
		// formats, whose levels are block-sized (Asset/TextureCompression.h: ceil(w / 4) * ceil(h / 4) blocks) and may have
		// any size; a BC level of the wrong size is refused. BC7Unorm's value is the one decision 13 froze (checked with the
		// others in "the block-compressed formats name their codec and size their levels by blocks").
		TEST_CASE("TextureData: a version 2 payload holds BC levels sized by their blocks")
		{
			CHECK(TextureData::FormatVersion == 2);
			TextureData texture;
			texture.Format = TextureFormat::BC7Unorm;
			texture.Width = 5;
			texture.Height = 3;
			texture.Mips = { { .Width = 5, .Height = 3, .Offset = 0, .Size = 32 }, { .Width = 2, .Height = 1, .Offset = 32, .Size = 16 },
				{ .Width = 1, .Height = 1, .Offset = 48, .Size = 16 } };
			texture.Pixels.assign(64, std::byte{ 0x5A });
			REQUIRE_MESSAGE(ValidateTextureData(texture).has_value(), ValidateTextureData(texture).error().ToString());
			const Buffer payload = SerializeTexturePayload(texture);
			const Result<TextureData> read = DeserializeTexturePayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Format == TextureFormat::BC7Unorm);
			CHECK(read->Mips == texture.Mips);
			CHECK(read->Pixels == texture.Pixels);

			TextureData wrong = texture;
			wrong.Mips[0].Size = 5 * 3 * 4;
			CHECK_FALSE(ValidateTextureData(wrong).has_value());
		}
	}

}
