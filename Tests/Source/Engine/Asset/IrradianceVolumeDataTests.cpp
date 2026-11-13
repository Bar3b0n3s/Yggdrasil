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
#include <span>

// Irradiance volumes (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 8, stream Q-A): the format version
// 1, its source file, its validation and its fuzz target.

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

		TEST_CASE("IrradianceVolumeData: a payload and a source file round-trip the probes byte for byte")
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

		TEST_CASE("IrradianceVolumeData: counts, sizes and coefficients outside the format's rules are refused")
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

		// The readers' own refusals (W26.1's review): a count above the limit, refused before anything is allocated however
		// large; a payload shorter or longer than its probes; a grid of one probe along an axis, which the reader reads and
		// the format's rules refuse; and a source whose magic is not "EIRV" or that is shorter than its header.
		TEST_CASE("IrradianceVolumeData: a count past its limit, a truncated payload, trailing bytes and another magic are refused")
		{
			const Buffer payload = SerializeIrradianceVolumePayload(MakeVolume());
			// The payload: CountX, CountY and CountZ (uint32 each), Size (3 x f32), then 2 x 2 x 3 probes of 9 x f32x3.
			constexpr size_t HeaderBytes = 24;
			constexpr size_t ProbeBytes = 9 * 3 * sizeof(float);
			REQUIRE(payload.size() == HeaderBytes + 12 * ProbeBytes);
			const auto withCountX = [&payload](uint32_t count)
			{
				Buffer changed = payload;
				for (size_t index = 0; index < 4; ++index)
					changed[index] = static_cast<std::byte>((count >> (8 * index)) & 0xFFU);
				return changed;
			};
			for (const uint32_t count : { MaxIrradianceProbesPerAxis + 1, 0xFFFFFFFFU })
			{
				CAPTURE(count);
				const Result<IrradianceVolumeData> large = DeserializeIrradianceVolumePayload(withCountX(count));
				REQUIRE_FALSE(large.has_value());
				CHECK(large.error().GetCode() == ErrorCode::Parse);
			}
			Buffer truncated = payload;
			truncated.pop_back();
			CHECK(DeserializeIrradianceVolumePayload(truncated).error().GetCode() == ErrorCode::Parse);
			CHECK(DeserializeIrradianceVolumePayload(std::span(payload).first(HeaderBytes - 1)).error().GetCode() == ErrorCode::Parse);
			Buffer trailing = payload;
			trailing.push_back(std::byte{ 0 });
			CHECK(DeserializeIrradianceVolumePayload(trailing).error().GetCode() == ErrorCode::Parse);
			// One probe along x: the 1 x 2 x 3 grid's six probes are all there, and the format's minimum of two refuses it.
			Buffer single = withCountX(1);
			single.resize(HeaderBytes + 6 * ProbeBytes);
			CHECK(DeserializeIrradianceVolumePayload(single).error().GetCode() == ErrorCode::Validation);

			Buffer magic = WriteIrradianceVolumeSource(MakeVolume());
			magic[0] = std::byte{ 'X' };
			CHECK(ReadIrradianceVolumeSource(magic).error().GetCode() == ErrorCode::Parse);
			const Buffer source = WriteIrradianceVolumeSource(MakeVolume());
			CHECK(ReadIrradianceVolumeSource(std::span(source).first(3)).error().GetCode() == ErrorCode::Parse);
			// The source's payload follows the same rules.
			Buffer sourceTrailing = source;
			sourceTrailing.push_back(std::byte{ 0 });
			CHECK(ReadIrradianceVolumeSource(sourceTrailing).error().GetCode() == ErrorCode::Parse);
		}

		TEST_CASE("IrradianceVolumeData: 10,000 seeded mutations of a payload never crash" * doctest::test_suite(Test::FuzzSuite))
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
