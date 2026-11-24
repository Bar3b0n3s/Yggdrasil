#include "TestsPCH.h"

#include "Engine/Asset/IrradianceVolumeData.h"

#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Random.h"
#include "Support/DeathTest.h"
#include "Support/Fuzz.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

// Irradiance volumes (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 8, stream Q-A): the format, its
// source file, its validation and its fuzz target; M45's format 2 and its relight block
// (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3).

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

		// MakeVolume with a relight block: probe p's local coefficient k is (k, p, 1) / 32 and its transfer value t is (p + t) / 64.
		IrradianceVolumeData MakeRelightableVolume()
		{
			IrradianceVolumeData volume = MakeVolume();
			volume.Relight.resize(volume.Probes.size());
			for (size_t probe = 0; probe < volume.Relight.size(); ++probe)
			{
				for (size_t k = 0; k < 9; ++k)
					volume.Relight[probe].Local[k] = glm::vec3(static_cast<float>(k), static_cast<float>(probe), 1.0f) / 32.0f;
				for (size_t t = 0; t < IrradianceTransferCount; ++t)
					volume.Relight[probe].Transfer[t] = static_cast<float>(probe + t) / 64.0f;
			}
			return volume;
		}

		// The uint32 version of a source file (its bytes 4 to 7, little-endian).
		uint32_t GetSourceVersion(const Buffer& source)
		{
			REQUIRE(source.size() >= 8);
			uint32_t version = 0;
			for (size_t index = 0; index < 4; ++index)
				version |= std::to_integer<uint32_t>(source[4 + index]) << (8 * index);
			return version;
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
			// M45: format 2, the relight block (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3).
			CHECK(IrradianceVolumeData::FormatVersion == 2);
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
			// 3 counts, the size and 12 probes of 27 floats, little-endian, no padding; M45's format 2 then its relight flag, 0.
			REQUIRE(payload.size() == 3 * 4 + 3 * 4 + 12 * 27 * 4 + 4);
			const Result<IrradianceVolumeData> read = DeserializeIrradianceVolumePayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->CountZ == 3);
			CHECK(read->Size == volume.Size);
			CHECK(read->Probes == volume.Probes);
			CHECK(SerializeIrradianceVolumePayload(*read) == payload);

			// A volume without a relight block writes a version 1 source: version 1's payload, without the flag (M45).
			const Buffer source = WriteIrradianceVolumeSource(volume);
			REQUIRE(source.size() == 8 + payload.size() - 4);
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
			// M45: a relight block is one per probe, every value finite.
			volume = MakeRelightableVolume();
			REQUIRE(ValidateIrradianceVolumeData(volume).has_value());
			volume.Relight.pop_back();
			CHECK(ValidateIrradianceVolumeData(volume).error().GetCode() == ErrorCode::Validation);
			volume = MakeRelightableVolume();
			volume.Relight[3].Local[8].z = std::numeric_limits<float>::infinity();
			CHECK(ValidateIrradianceVolumeData(volume).error().GetCode() == ErrorCode::Validation);
			volume = MakeRelightableVolume();
			volume.Relight[7].Transfer[44] = std::numeric_limits<float>::quiet_NaN();
			CHECK(ValidateIrradianceVolumeData(volume).error().GetCode() == ErrorCode::Validation);
			// A source of a version other than 1 and 2 is refused as such.
			Buffer source = WriteIrradianceVolumeSource(MakeVolume());
			REQUIRE(source.size() > 8);
			source[4] = std::byte{ 3 };
			CHECK(ReadIrradianceVolumeSource(source).error().GetCode() == ErrorCode::UnsupportedVersion);
			source[4] = std::byte{ 0 };
			CHECK(ReadIrradianceVolumeSource(source).error().GetCode() == ErrorCode::UnsupportedVersion);
		}

		// The readers' own refusals (W26.1's review): a count above the limit, refused before anything is allocated however
		// large; a payload shorter or longer than its probes; a grid of one probe along an axis, which the reader reads and
		// the format's rules refuse; and a source whose magic is not "EIRV" or that is shorter than its header.
		TEST_CASE("IrradianceVolumeData: a count past its limit, a truncated payload, trailing bytes and another magic are refused")
		{
			const Buffer payload = SerializeIrradianceVolumePayload(MakeVolume());
			// The payload: CountX, CountY and CountZ (uint32 each), Size (3 x f32), then 2 x 2 x 3 probes of 9 x f32x3, then the
			// relight flag (M45), 0.
			constexpr size_t HeaderBytes = 24;
			constexpr size_t ProbeBytes = 9 * 3 * sizeof(float);
			constexpr size_t FlagBytes = 4;
			REQUIRE(payload.size() == HeaderBytes + 12 * ProbeBytes + FlagBytes);
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
			// One probe along x: the 1 x 2 x 3 grid's six probes and its flag are all there, and the format's minimum of two refuses
			// it.
			Buffer single = withCountX(1);
			single.resize(HeaderBytes + 6 * ProbeBytes);
			single.resize(HeaderBytes + 6 * ProbeBytes + FlagBytes, std::byte{ 0 });
			CHECK(DeserializeIrradianceVolumePayload(single).error().GetCode() == ErrorCode::Validation);
			// M45: a relight flag other than 0 or 1, and a relight block shorter than its probes, are refused before it is read.
			Buffer flag = payload;
			flag[HeaderBytes + 12 * ProbeBytes] = std::byte{ 2 };
			CHECK(DeserializeIrradianceVolumePayload(flag).error().GetCode() == ErrorCode::Parse);
			const Buffer relightable = SerializeIrradianceVolumePayload(MakeRelightableVolume());
			CHECK(DeserializeIrradianceVolumePayload(std::span(relightable).first(relightable.size() - 4)).error().GetCode() == ErrorCode::Parse);
			CHECK(DeserializeIrradianceVolumePayload(std::span(relightable).first(payload.size())).error().GetCode() == ErrorCode::Parse);

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

		// M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3): a relightable volume's payload carries its relight block
		// after the flag 1 and round-trips it; its source is version 2 with the same payload; a version 1 source (M26's bytes, no
		// flag) still reads, as a volume without the block; and a volume without the block writes version 1, byte for byte the
		// layout M26 wrote. The cooked artifact is format 2 either way.
		TEST_CASE("IrradianceVolumeData: a relightable payload round-trips, a version 1 source still reads, and a volume without a relight block writes version 1")
		{
			const IrradianceVolumeData relightable = MakeRelightableVolume();
			const Buffer payload = SerializeIrradianceVolumePayload(relightable);
			constexpr size_t BakedBytes = 3 * 4 + 3 * 4 + 12 * 27 * 4;
			// The flag 1, then 12 probes of 27 local and 45 transfer floats.
			REQUIRE(payload.size() == BakedBytes + 4 + 12 * (27 + 45) * 4);
			CHECK(std::to_integer<uint32_t>(payload[BakedBytes]) == 1);
			const Result<IrradianceVolumeData> read = DeserializeIrradianceVolumePayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Probes == relightable.Probes);
			CHECK(read->Relight == relightable.Relight);
			CHECK(SerializeIrradianceVolumePayload(*read) == payload);

			const Buffer source = WriteIrradianceVolumeSource(relightable);
			CHECK(GetSourceVersion(source) == IrradianceVolumeData::FormatVersion);
			REQUIRE(source.size() == 8 + payload.size());
			CHECK(std::equal(payload.begin(), payload.end(), source.begin() + 8));
			const Result<IrradianceVolumeData> fromSource = ReadIrradianceVolumeSource(source);
			REQUIRE_MESSAGE(fromSource.has_value(), fromSource.error().ToString());
			CHECK(fromSource->Relight == relightable.Relight);
			const Result<AssetRef<IrradianceVolumeData>> cooked = LoadCookedIrradianceVolume(CookIrradianceVolume(relightable, 1));
			REQUIRE_MESSAGE(cooked.has_value(), cooked.error().ToString());
			CHECK((*cooked)->Relight == relightable.Relight);

			// Without a block: a version 1 source of M26's layout, the magic, the version and the payload without the flag.
			const IrradianceVolumeData baked = MakeVolume();
			const Buffer bakedSource = WriteIrradianceVolumeSource(baked);
			CHECK(GetSourceVersion(bakedSource) == IrradianceVolumeSourceVersionWithoutRelight);
			const Buffer bakedPayload = SerializeIrradianceVolumePayload(baked);
			REQUIRE(bakedSource.size() == 8 + BakedBytes);
			CHECK(std::equal(bakedSource.begin() + 8, bakedSource.end(), bakedPayload.begin()));
			const Result<IrradianceVolumeData> version1 = ReadIrradianceVolumeSource(bakedSource);
			REQUIRE_MESSAGE(version1.has_value(), version1.error().ToString());
			CHECK(version1->Probes == baked.Probes);
			CHECK(version1->Relight.empty());
			// A version 1 source with a flag after its probes has trailing bytes, and a version 2 source without one is short.
			Buffer flagged = bakedSource;
			flagged.resize(flagged.size() + 4, std::byte{ 0 });
			CHECK(ReadIrradianceVolumeSource(flagged).error().GetCode() == ErrorCode::Parse);
			Buffer unflagged = bakedSource;
			unflagged[4] = std::byte{ 2 };
			CHECK(ReadIrradianceVolumeSource(unflagged).error().GetCode() == ErrorCode::Parse);
		}

		TEST_CASE("IrradianceVolumeData: 10,000 seeded mutations of a payload never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			// M45: the seed is a relightable volume's format 2 payload, so the mutations reach the relight block's reader.
			const Buffer payload = SerializeIrradianceVolumePayload(MakeRelightableVolume());
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
