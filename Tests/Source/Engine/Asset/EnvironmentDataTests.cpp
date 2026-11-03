#include "TestsPCH.h"

#include "Engine/Asset/EnvironmentData.h"

#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/TextureCompression.h"
#include "Engine/Core/BinaryWriter.h"
#include "Support/Fuzz.h"
#include "Support/RenderReference.h"
#include "Support/SceneTestFixture.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

// The cooked environment payload (Architecture §6.8, §8.6; Asset/EnvironmentData.h; Docs/Decisions/0013-m8-decisions.md decision 9;
// version 2's cube formats, Docs/Decisions/0034-m23-contract.md decision 14) and the Environment loader.

namespace Engine {

	namespace {

		// A cube of `faceSize` with `mipCount` levels, every texel binary16 0.5 (0x3800).
		CubeMapData MakeCube(uint32_t faceSize, uint32_t mipCount)
		{
			CubeMapData cube{ .FaceSize = faceSize, .MipCount = mipCount, .Texels = Buffer(ComputeCubeMapByteSize(faceSize, mipCount)) };
			for (size_t offset = 0; offset < cube.Texels.size(); offset += 2)
			{
				const uint16_t half = 0x3800;
				std::memcpy(cube.Texels.data() + offset, &half, sizeof(half));
			}
			return cube;
		}

		// A valid environment: a 4² skybox with its full chain of 3 mips and the 256² x 7 specular cube, every texel
		// binary16 0.5 (0x3800), and distinct SH coefficients.
		EnvironmentData MakeEnvironment()
		{
			EnvironmentData environment;
			environment.Skybox = MakeCube(4, 3);
			environment.Specular = MakeCube(EnvironmentData::SpecularFaceSize, EnvironmentData::SpecularMipCount);
			for (size_t index = 0; index < environment.IrradianceSH9.size(); ++index)
				environment.IrradianceSH9[index] = glm::vec3(static_cast<float>(index), 0.5f, -0.25f);
			return environment;
		}

		// `cube` (Rgba16Float) as BC6H blocks, every face of every level compressed as EnvironmentImporter compresses it.
		CubeMapData CompressCube(const CubeMapData& cube)
		{
			CubeMapData compressed{ .FaceSize = cube.FaceSize, .MipCount = cube.MipCount, .Format = CubeMapFormat::BC6HUfloat, .Texels = {} };
			size_t offset = 0;
			for (uint32_t level = 0; level < cube.MipCount; ++level)
			{
				const uint32_t edge = std::max(1U, cube.FaceSize >> level);
				const size_t faceBytes = ComputeCubeMapFaceByteSize(cube.FaceSize, level, CubeMapFormat::Rgba16Float);
				for (uint32_t face = 0; face < CubeMapData::FaceCount; ++face)
				{
					const Result<Buffer> blocks = CompressTextureBlocks(TextureBlockCodec::BC6H, edge, edge, std::span<const std::byte>(cube.Texels).subspan(offset, faceBytes));
					REQUIRE_MESSAGE(blocks.has_value(), blocks.error().ToString());
					compressed.Texels.insert(compressed.Texels.end(), blocks->begin(), blocks->end());
					offset += faceBytes;
				}
			}
			return compressed;
		}

		// Every face level of a BC6HUfloat cube decompresses (DecompressTextureBlocks) to finite, non-negative binary16
		// values: the property ValidateEnvironmentData relies on to check such a cube by its size alone.
		void CheckBC6HCubeDecodes(const CubeMapData& cube)
		{
			REQUIRE(cube.Format == CubeMapFormat::BC6HUfloat);
			size_t offset = 0;
			for (uint32_t level = 0; level < cube.MipCount; ++level)
			{
				const uint32_t edge = std::max(1U, cube.FaceSize >> level);
				const size_t faceBytes = ComputeCubeMapFaceByteSize(cube.FaceSize, level, cube.Format);
				for (uint32_t face = 0; face < CubeMapData::FaceCount; ++face)
				{
					const Result<Buffer> texels = DecompressTextureBlocks(TextureBlockCodec::BC6H, edge, edge, std::span<const std::byte>(cube.Texels).subspan(offset, faceBytes));
					REQUIRE_MESSAGE(texels.has_value(), texels.error().ToString());
					for (size_t index = 0; index + 1 < texels->size(); index += 2)
					{
						const auto half = static_cast<uint16_t>(std::to_integer<uint32_t>((*texels)[index]) | (std::to_integer<uint32_t>((*texels)[index + 1]) << 8));
						const double value = Test::HalfToDouble(half);
						CHECK(std::isfinite(value));
						CHECK(value >= 0.0);
					}
					offset += faceBytes;
				}
			}
		}

		// One cube's version 2 header: face size, mip count, format byte and three reserved bytes.
		void WriteCubeHeader(BinaryWriter& writer, uint32_t faceSize, uint32_t mipCount, CubeMapFormat format)
		{
			writer.WriteU32(faceSize);
			writer.WriteU32(mipCount);
			writer.WriteU8(std::to_underlying(format));
			for (int reserved = 0; reserved < 3; ++reserved)
				writer.WriteU8(0);
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("EnvironmentData: cube byte sizes count every face of every mip")
		{
			CHECK(ComputeCubeMapByteSize(0, 3) == 0);
			CHECK(ComputeCubeMapByteSize(4, 0) == 0);
			CHECK(ComputeCubeMapByteSize(1, 1) == 6 * 8);
			CHECK(ComputeCubeMapByteSize(4, 3) == (16 + 4 + 1) * 6 * 8);
			CHECK(ComputeCubeMapByteSize(2, 4) == (4 + 1 + 1 + 1) * 6 * 8);
			// Header values of an untrusted payload: more mips than halvings (every further level is 1x1), large faces, and a
			// size that does not fit in size_t, which saturates instead of overflowing.
			CHECK(ComputeCubeMapByteSize(1, 40) == size_t{ 40 } * 6 * 8);
			CHECK(ComputeCubeMapByteSize(4, 100) == size_t{ 16 + 4 + 98 } * 6 * 8);
			CHECK(ComputeCubeMapByteSize(65536, 17) == size_t{ 5726623061 } * 6 * 8); // the sum of 4^k for k = 0..16
			CHECK(ComputeCubeMapByteSize(0xFFFFFFFFU, 0xFFFFFFFFU) == std::numeric_limits<size_t>::max());
			static_assert(ComputeCubeMapByteSize(4, 3) == (16 + 4 + 1) * 6 * 8);
			CHECK(ComputeCubeMapByteSize(4, 3, CubeMapFormat::Rgba16Float) == ComputeCubeMapByteSize(4, 3));
		}

		// M23 (R-TEXC-HDR, decision 14): a BC6H face level is ceil(edge / 4)² blocks of 16 bytes, a level below 4 texels one
		// block, with the same saturation for untrusted header values.
		TEST_CASE("EnvironmentData: BC6H cube sizes count the blocks of every face of every mip")
		{
			CHECK(ComputeCubeMapByteSize(1, 1, CubeMapFormat::BC6HUfloat) == 6 * 16);
			CHECK(ComputeCubeMapByteSize(4, 3, CubeMapFormat::BC6HUfloat) == (1 + 1 + 1) * 6 * 16);
			CHECK(ComputeCubeMapByteSize(16, 5, CubeMapFormat::BC6HUfloat) == (16 + 4 + 1 + 1 + 1) * 6 * 16);
			CHECK(ComputeCubeMapByteSize(6, 3, CubeMapFormat::BC6HUfloat) == (4 + 1 + 1) * 6 * 16); // 6 texels are 2 x 2 blocks
			CHECK(ComputeCubeMapByteSize(4, 100, CubeMapFormat::BC6HUfloat) == size_t{ 100 } * 6 * 16);
			CHECK(ComputeCubeMapByteSize(0, 3, CubeMapFormat::BC6HUfloat) == 0);
			CHECK(ComputeCubeMapByteSize(0xFFFFFFFFU, 0xFFFFFFFFU, CubeMapFormat::BC6HUfloat) == std::numeric_limits<size_t>::max());
			static_assert(ComputeCubeMapByteSize(256, 7, CubeMapFormat::BC6HUfloat) == size_t{ 4096 + 1024 + 256 + 64 + 16 + 4 + 1 } * 6 * 16);

			CHECK(ComputeCubeMapFaceByteSize(256, 0, CubeMapFormat::Rgba16Float) == 256 * 256 * 8);
			CHECK(ComputeCubeMapFaceByteSize(256, 0, CubeMapFormat::BC6HUfloat) == 64 * 64 * 16);
			CHECK(ComputeCubeMapFaceByteSize(256, 7, CubeMapFormat::BC6HUfloat) == 16);  // 2 x 2 texels
			CHECK(ComputeCubeMapFaceByteSize(256, 40, CubeMapFormat::Rgba16Float) == 8); // past the chain: 1 x 1
			CHECK(ComputeCubeMapFaceByteSize(0, 0, CubeMapFormat::BC6HUfloat) == 0);
			CHECK(CubeMapFormatToString(CubeMapFormat::Rgba16Float) == "Rgba16Float");
			CHECK(CubeMapFormatToString(CubeMapFormat::BC6HUfloat) == "BC6HUfloat");
			CHECK(CubeMapFormatToString(static_cast<CubeMapFormat>(7)) == "Unknown");
		}

		TEST_CASE("EnvironmentData: the payload round-trips byte for byte")
		{
			const EnvironmentData environment = MakeEnvironment();
			REQUIRE(ValidateEnvironmentData(environment).has_value());
			const Buffer payload = SerializeEnvironmentPayload(environment);
			CHECK(payload.size() == 24 + 9 * 12 + environment.Skybox.Texels.size() + environment.Specular.Texels.size());
			Result<EnvironmentData> read = DeserializeEnvironmentPayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Skybox.FaceSize == 4);
			CHECK(read->Skybox.MipCount == 3);
			CHECK(read->Skybox.Format == CubeMapFormat::Rgba16Float);
			CHECK(read->Skybox.Texels == environment.Skybox.Texels);
			CHECK(read->Specular.Texels == environment.Specular.Texels);
			CHECK(read->IrradianceSH9 == environment.IrradianceSH9);
			CHECK(SerializeEnvironmentPayload(*read) == payload);
		}

		// M23 (decision 14, the data format of EnvironmentData version 2): each cube's format byte and three reserved zero
		// bytes after its mip count; a BC6H cube holds its blocks, which validate by their size alone (BC6H's unsigned half
		// floats decode to finite, non-negative values whatever the bytes); the two cubes' formats are independent.
		TEST_CASE("EnvironmentData: a version 2 payload holds BC6H cubes sized by their blocks")
		{
			EnvironmentData environment = MakeEnvironment();
			environment.Skybox = CompressCube(environment.Skybox);
			REQUIRE_MESSAGE(ValidateEnvironmentData(environment).has_value(), ValidateEnvironmentData(environment).error().ToString());
			CHECK(environment.Skybox.Texels.size() == 3 * 6 * 16);
			const Buffer payload = SerializeEnvironmentPayload(environment);
			CHECK(std::to_integer<int>(payload[8]) == 1);  // the skybox's format byte: BC6HUfloat
			CHECK(std::to_integer<int>(payload[20]) == 0); // the specular cube's: Rgba16Float
			Result<EnvironmentData> read = DeserializeEnvironmentPayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Skybox.Format == CubeMapFormat::BC6HUfloat);
			CHECK(read->Specular.Format == CubeMapFormat::Rgba16Float);
			CHECK(read->Skybox.Texels == environment.Skybox.Texels);
			CHECK(SerializeEnvironmentPayload(*read) == payload);
			const Result<AssetRef<EnvironmentData>> loaded = LoadCookedEnvironment(CookEnvironment(environment, 2));
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			CHECK((*loaded)->Skybox.Format == CubeMapFormat::BC6HUfloat);

			// Any block bytes are a valid BC6H cube, and decode to finite, non-negative values.
			EnvironmentData anyBlocks = environment;
			std::ranges::fill(anyBlocks.Skybox.Texels, std::byte{ 0xFF });
			CHECK(ValidateEnvironmentData(anyBlocks).has_value());
			CheckBC6HCubeDecodes(anyBlocks.Skybox);
			// A BC6H cube sized as RGBA16F, an unknown format, and the specular cube in BC6H with a texel short.
			EnvironmentData wrongSize = environment;
			wrongSize.Skybox.Texels.resize(ComputeCubeMapByteSize(4, 3));
			EnvironmentData unknown = MakeEnvironment();
			unknown.Skybox.Format = static_cast<CubeMapFormat>(2);
			EnvironmentData shortSpecular = MakeEnvironment();
			shortSpecular.Specular = CompressCube(shortSpecular.Specular);
			REQUIRE(ValidateEnvironmentData(shortSpecular).has_value());
			shortSpecular.Specular.Texels.pop_back();
			for (const EnvironmentData* invalid : { &wrongSize, &unknown, &shortSpecular })
			{
				const Status valid = ValidateEnvironmentData(*invalid);
				REQUIRE_FALSE(valid.has_value());
				CHECK(valid.error().GetCode() == ErrorCode::Validation);
			}

			// An unknown format byte and a reserved byte that is not 0 are Parse errors of the payload.
			for (const size_t offset : { size_t{ 8 }, size_t{ 9 }, size_t{ 11 }, size_t{ 20 }, size_t{ 23 } })
			{
				CAPTURE(offset);
				Buffer broken = payload;
				broken[offset] = std::byte{ 2 };
				const Result<EnvironmentData> refused = DeserializeEnvironmentPayload(broken);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::Parse);
			}
			// Cooked environments are cache artifacts: version 1's are never read, and rebuilt by EnvironmentImporter 2.
			const Result<AssetRef<EnvironmentData>> versionOne = LoadCookedEnvironment(WriteCookedArtifact(AssetType::Environment, 1, 1, payload));
			REQUIRE_FALSE(versionOne.has_value());
			CHECK(versionOne.error().GetCode() == ErrorCode::UnsupportedVersion);
			CHECK(EnvironmentData::FormatVersion == 2);
		}

		TEST_CASE("EnvironmentData: invalid environments name the first violation")
		{
			EnvironmentData partialChain = MakeEnvironment();
			partialChain.Skybox.MipCount = 2;
			partialChain.Skybox.Texels.resize(ComputeCubeMapByteSize(4, 2));
			EnvironmentData wrongSpecular = MakeEnvironment();
			wrongSpecular.Specular.FaceSize = 128;
			EnvironmentData shortTexels = MakeEnvironment();
			shortTexels.Skybox.Texels.pop_back();
			EnvironmentData infinite = MakeEnvironment();
			const uint16_t infinity = 0x7C00;
			std::memcpy(infinite.Skybox.Texels.data(), &infinity, sizeof(infinity));
			EnvironmentData negative = MakeEnvironment();
			const uint16_t minusOne = 0xBC00;
			std::memcpy(negative.Specular.Texels.data(), &minusOne, sizeof(minusOne));
			EnvironmentData nanSh = MakeEnvironment();
			nanSh.IrradianceSH9[4].y = std::numeric_limits<float>::quiet_NaN();
			for (const EnvironmentData* invalid : { &partialChain, &wrongSpecular, &shortTexels, &infinite, &negative, &nanSh })
			{
				const Status valid = ValidateEnvironmentData(*invalid);
				REQUIRE_FALSE(valid.has_value());
				CHECK(valid.error().GetCode() == ErrorCode::Validation);
			}
		}

		TEST_CASE("EnvironmentData: truncated and padded payloads are Parse errors, never asserts")
		{
			const Buffer payload = SerializeEnvironmentPayload(MakeEnvironment());
			const std::array<size_t, 4> sizes = { 0, 23, 24 + 9 * 12, payload.size() - 1 };
			for (const size_t size : sizes)
			{
				const Result<EnvironmentData> read = DeserializeEnvironmentPayload(std::span(payload.data(), size));
				REQUIRE_FALSE(read.has_value());
				CHECK(read.error().GetCode() == ErrorCode::Parse);
			}
			Buffer padded = payload;
			padded.push_back(std::byte{ 0 });
			CHECK(DeserializeEnvironmentPayload(padded).error().GetCode() == ErrorCode::Parse);
			// A header claiming a huge cube is rejected before anything is allocated.
			Buffer huge = payload;
			const uint32_t faceSize = 1U << 30;
			std::memcpy(huge.data(), &faceSize, sizeof(faceSize));
			CHECK_FALSE(DeserializeEnvironmentPayload(huge).has_value());
		}

		TEST_CASE("EnvironmentData: 1,000 seeded mutations of a payload never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			// Three seeds: a small payload (1x1 RGBA16F cubes of one mip, whose specular cube Validate refuses, so the parser's
			// header and size checks see every mutation cheaply), a small one whose cubes are BC6H (a 4² skybox of three mips,
			// a 1x1 specular cube), and on every 64th iteration the full valid payload with a BC6H skybox (4 MB, the accepting
			// path). The cooked form goes through LoadCookedEnvironment with the payload hash sealed again. Every BC6H cube of
			// an accepted payload is decompressed, so CMP_Core's BC6H decoder sees the mutations (M23, decision 19).
			BinaryWriter small;
			WriteCubeHeader(small, 1, 1, CubeMapFormat::Rgba16Float);
			WriteCubeHeader(small, 1, 1, CubeMapFormat::Rgba16Float);
			for (size_t index = 0; index < 27; ++index)
				small.WriteF32(0.25f * static_cast<float>(index));
			const std::array<uint16_t, 2 * 6 * 4> texels{};
			small.WriteArray(std::span<const uint16_t>(texels));
			const Buffer smallPayload = small.TakeBuffer();
			EnvironmentData blockEnvironment = MakeEnvironment();
			blockEnvironment.Skybox = CompressCube(blockEnvironment.Skybox);
			BinaryWriter smallBlocks;
			WriteCubeHeader(smallBlocks, 4, 3, CubeMapFormat::BC6HUfloat);
			WriteCubeHeader(smallBlocks, 1, 1, CubeMapFormat::BC6HUfloat);
			for (size_t index = 0; index < 27; ++index)
				smallBlocks.WriteF32(0.5f);
			smallBlocks.WriteBytes(blockEnvironment.Skybox.Texels);
			smallBlocks.WriteBytes(std::span<const std::byte>(blockEnvironment.Skybox.Texels).first(6 * 16));
			const Buffer smallBlockPayload = smallBlocks.TakeBuffer();
			const Buffer fullPayload = SerializeEnvironmentPayload(blockEnvironment);
			REQUIRE(DeserializeEnvironmentPayload(fullPayload).has_value());
			const Test::FuzzRun run = Test::RunFuzzCase(0xE4F1, 1000, [&smallPayload, &smallBlockPayload, &fullPayload](Random& random, uint64_t index)
			{
				Buffer mutated = index % 64 == 0 ? fullPayload : (index % 2 == 0 ? smallPayload : smallBlockPayload);
				Test::MutateBytes(mutated, random);
				const Result<EnvironmentData> read = random.NextBool() ? DeserializeEnvironmentPayload(mutated)
																	   : LoadCookedEnvironment(WriteCookedArtifact(AssetType::Environment, EnvironmentData::FormatVersion, 2, mutated))
																			 .transform([](const AssetRef<EnvironmentData>& loaded)
				{
					return *loaded;
				});
				if (!read.has_value())
				{
					const ErrorCode code = read.error().GetCode();
					CHECK((code == ErrorCode::Parse || code == ErrorCode::Validation));
					return false;
				}
				CHECK(ValidateEnvironmentData(*read).has_value());
				for (const CubeMapData* cube : { &read->Skybox, &read->Specular })
				{
					if (cube->Format == CubeMapFormat::BC6HUfloat)
						CheckBC6HCubeDecodes(*cube);
				}
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
				CHECK(run.Counts.Rejected > 0);
		}

		TEST_CASE("EnvironmentData: the Environment loader reads a cooked environment")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			AssetLoaderRegistry loaders;
			RegisterBuiltinLoaders(loaders);
			REQUIRE(loaders.Find(AssetType::Environment) != nullptr);
			const Buffer cooked = CookEnvironment(MakeEnvironment(), 1);
			Result<AssetRef<Asset>> loaded = loaders.Load(cooked, { .Registry = registry.get(), .Handle = AssetHandle(0x201) });
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			const AssetRef<EnvironmentData> environment = AssetCast<EnvironmentData>(*loaded);
			REQUIRE(environment != nullptr);
			CHECK(environment->Specular.MipCount == EnvironmentData::SpecularMipCount);
			Result<AssetRef<EnvironmentData>> direct = LoadCookedEnvironment(cooked);
			REQUIRE(direct.has_value());
			CHECK((*direct)->IrradianceSH9 == environment->IrradianceSH9);
		}
	}

}
