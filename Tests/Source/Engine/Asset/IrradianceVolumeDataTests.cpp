#include "TestsPCH.h"

#include "Engine/Asset/IrradianceVolumeData.h"

#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Random.h"
#include "Support/DeathTest.h"
#include "Support/Fuzz.h"

#include <cstddef>
#include <cstdint>
#include <limits>

// Irradiance volumes (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 8, stream Q-A): the format version
// 1, its source file, its validation and its fuzz target. The artifact wrappers and the loader's registration are the
// contract's (live); the rest is skipped until stream Q-A implements it.

namespace Engine {

	namespace {

		// A 2 x 2 x 3 volume whose probes hold distinct coefficients (probe p's coefficient k is (p, k, p + k) / 16).
		IrradianceVolumeData MakeVolume()
		{
			IrradianceVolumeData volume;
			volume.CountX = 2;
			volume.CountY = 2;
			volume.CountZ = 3;
			volume.Size = glm::vec3(4.0f, 2.0f, 6.0f);
			volume.Probes.resize(12);
			for (size_t probe = 0; probe < volume.Probes.size(); ++probe)
			{
				for (size_t k = 0; k < 9; ++k)
					volume.Probes[probe].SH9[k] = glm::vec3(static_cast<float>(probe), static_cast<float>(k), static_cast<float>(probe + k)) / 16.0f;
			}
			return volume;
		}

	}

	ENGINE_DEATH_TEST("Asset/IrradianceProbeIndexPastItsCount")
	{
		const IrradianceVolumeData volume = MakeVolume();
		static_cast<void>(GetIrradianceProbeIndex(volume, 0, 2, 0));
	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("IrradianceVolumeData: the artifact names its type and format version, and its loader is registered")
		{
			CHECK(IrradianceVolumeData::StaticType == AssetType::IrradianceVolume);
			CHECK(IrradianceVolumeData::FormatVersion == 1);
			CHECK(GetCookedFormatVersion(AssetType::IrradianceVolume) == IrradianceVolumeData::FormatVersion);
			AssetLoaderRegistry loaders;
			RegisterBuiltinLoaders(loaders);
			REQUIRE(loaders.Find(AssetType::IrradianceVolume) != nullptr);
			const std::byte payload[1] = {};
			const Result<AssetRef<IrradianceVolumeData>> newer = LoadCookedIrradianceVolume(
				WriteCookedArtifact(AssetType::IrradianceVolume, IrradianceVolumeData::FormatVersion + 1, 1, payload));
			REQUIRE_FALSE(newer.has_value());
			CHECK(newer.error().GetCode() == ErrorCode::UnsupportedVersion);
			// Probe (x, y, z) is x fastest, then y, then z.
			const IrradianceVolumeData volume = MakeVolume();
			CHECK(GetIrradianceProbeIndex(volume, 1, 0, 0) == 1);
			CHECK(GetIrradianceProbeIndex(volume, 0, 1, 0) == 2);
			CHECK(GetIrradianceProbeIndex(volume, 1, 1, 2) == 11);
		}

		// The index's coordinates are asserted below their counts (C26.1's review), a programmer error the format's readers
		// never reach: they validate the counts and index within them.
		TEST_CASE("IrradianceVolumeData: a probe index past its count asserts")
		{
			ENGINE_CHECK_DEATH("Asset/IrradianceProbeIndexPastItsCount", "Probe (0, 2, 0)");
		}

		TEST_CASE("IrradianceVolumeData: a payload and a source file round-trip the probes byte for byte" * doctest::skip(true))
		{
			const IrradianceVolumeData volume = MakeVolume();
			REQUIRE(ValidateIrradianceVolumeData(volume).has_value());
			const Buffer payload = SerializeIrradianceVolumePayload(volume);
			// 3 counts, the size and 12 probes of 27 floats, little-endian, no padding.
			REQUIRE(payload.size() == 3 * 4 + 3 * 4 + 12 * 27 * 4);
			const Result<IrradianceVolumeData> read = DeserializeIrradianceVolumePayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->CountZ == 3);
			CHECK(read->Size == volume.Size);
			CHECK(read->Probes == volume.Probes);
			CHECK(SerializeIrradianceVolumePayload(*read) == payload);

			const Buffer source = WriteIrradianceVolumeSource(volume);
			REQUIRE(source.size() == 8 + payload.size());
			CHECK(static_cast<char>(source[0]) == 'E');
			CHECK(static_cast<char>(source[3]) == 'V');
			const Result<IrradianceVolumeData> fromSource = ReadIrradianceVolumeSource(source);
			REQUIRE_MESSAGE(fromSource.has_value(), fromSource.error().ToString());
			CHECK(fromSource->Probes == volume.Probes);
		}

		TEST_CASE("IrradianceVolumeData: counts, sizes and coefficients outside the format's rules are refused" * doctest::skip(true))
		{
			IrradianceVolumeData volume = MakeVolume();
			volume.CountX = MaxIrradianceProbesPerAxis + 1;
			CHECK(ValidateIrradianceVolumeData(volume).error().GetCode() == ErrorCode::Validation);
			volume = MakeVolume();
			volume.Probes.pop_back();
			CHECK(ValidateIrradianceVolumeData(volume).error().GetCode() == ErrorCode::Validation);
			volume = MakeVolume();
			volume.Size.y = 0.0f;
			CHECK(ValidateIrradianceVolumeData(volume).error().GetCode() == ErrorCode::Validation);
			volume = MakeVolume();
			volume.Probes[5].SH9[2].x = std::numeric_limits<float>::quiet_NaN();
			CHECK(ValidateIrradianceVolumeData(volume).error().GetCode() == ErrorCode::Validation);
			// A source of another version is refused as such.
			Buffer source = WriteIrradianceVolumeSource(MakeVolume());
			REQUIRE(source.size() > 8);
			source[4] = std::byte{ 2 };
			CHECK(ReadIrradianceVolumeSource(source).error().GetCode() == ErrorCode::UnsupportedVersion);
		}

		TEST_CASE("IrradianceVolumeData: 10,000 seeded mutations of a payload never crash" * doctest::test_suite(Test::FuzzSuite) * doctest::skip(true))
		{
			const Buffer payload = SerializeIrradianceVolumePayload(MakeVolume());
			REQUIRE(DeserializeIrradianceVolumePayload(payload).has_value());
			const Test::FuzzRun run = Test::RunFuzzCase(0x1BB0, 10000, [&payload](Random& random, uint64_t /*index*/)
			{
				Buffer mutated = payload;
				Test::MutateBytes(mutated, random);
				// A payload the reader accepts is a valid volume.
				const Result<IrradianceVolumeData> read = DeserializeIrradianceVolumePayload(mutated);
				if (!read.has_value())
					return false;
				CHECK(ValidateIrradianceVolumeData(*read).has_value());
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
				CHECK(run.Counts.Rejected > 0);
		}
	}

}
