#include "TestsPCH.h"

#include "Engine/Asset/EnvironmentData.h"

#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/CookedFormat.h"
#include "Support/SceneTestFixture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

// The cooked environment payload (Architecture §6.8, §8.6; Asset/EnvironmentData.h) and the Environment loader. Skeletons of
// the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 9); stream B implements the codec and removes the skips.

namespace Engine {

	namespace {

		// A valid environment: a 4² skybox with its full chain of 3 mips and the 256² x 7 specular cube, every texel
		// binary16 0.5 (0x3800), and distinct SH coefficients.
		EnvironmentData MakeEnvironment()
		{
			const auto fill = [](uint32_t faceSize, uint32_t mipCount)
			{
				CubeMapData cube{ .FaceSize = faceSize, .MipCount = mipCount, .Texels = Buffer(ComputeCubeMapByteSize(faceSize, mipCount)) };
				for (size_t offset = 0; offset < cube.Texels.size(); offset += 2)
				{
					const uint16_t half = 0x3800;
					std::memcpy(cube.Texels.data() + offset, &half, sizeof(half));
				}
				return cube;
			};
			EnvironmentData environment;
			environment.Skybox = fill(4, 3);
			environment.Specular = fill(EnvironmentData::SpecularFaceSize, EnvironmentData::SpecularMipCount);
			for (size_t index = 0; index < environment.IrradianceSH9.size(); ++index)
				environment.IrradianceSH9[index] = glm::vec3(static_cast<float>(index), 0.5f, -0.25f);
			return environment;
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
		}

		TEST_CASE("EnvironmentData: the payload round-trips byte for byte" * doctest::skip(true))
		{
			const EnvironmentData environment = MakeEnvironment();
			REQUIRE(ValidateEnvironmentData(environment).has_value());
			const Buffer payload = SerializeEnvironmentPayload(environment);
			CHECK(payload.size() == 16 + 9 * 12 + environment.Skybox.Texels.size() + environment.Specular.Texels.size());
			Result<EnvironmentData> read = DeserializeEnvironmentPayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Skybox.FaceSize == 4);
			CHECK(read->Skybox.MipCount == 3);
			CHECK(read->Skybox.Texels == environment.Skybox.Texels);
			CHECK(read->Specular.Texels == environment.Specular.Texels);
			CHECK(read->IrradianceSH9 == environment.IrradianceSH9);
			CHECK(SerializeEnvironmentPayload(*read) == payload);
		}

		TEST_CASE("EnvironmentData: invalid environments name the first violation" * doctest::skip(true))
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

		TEST_CASE("EnvironmentData: truncated and padded payloads are Parse errors, never asserts" * doctest::skip(true))
		{
			const Buffer payload = SerializeEnvironmentPayload(MakeEnvironment());
			const std::array<size_t, 4> sizes = { 0, 15, 16 + 9 * 12, payload.size() - 1 };
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

		TEST_CASE("EnvironmentData: the Environment loader reads a cooked environment" * doctest::skip(true))
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
