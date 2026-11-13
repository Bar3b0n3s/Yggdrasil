#include "EnginePCH.h"
#include "Engine/Asset/HeightfieldData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"

#include <algorithm>
#include <utility>

// The heightfield's formats (HeightfieldData.h; Docs/Decisions/0037-m26-contract.md decision 20): the artifact wrappers and
// the flat heightfield are the contract's, the payload, the validation and the source reader and writer stream T-A's.

namespace Engine {

	namespace Utils {

		// The source header's size: the magic and the uint32 version.
		constexpr size_t HeightfieldSourceHeaderBytes = HeightfieldSourceMagic.size() + sizeof(uint32_t);

		[[nodiscard]] static constexpr bool IsHeightfieldResolutionInRange(uint32_t resolution)
		{
			return resolution >= MinHeightfieldResolution && resolution <= MaxHeightfieldResolution;
		}

	}

	HeightfieldData MakeFlatHeightfield(uint32_t resolution, uint16_t height)
	{
		ENGINE_CORE_ASSERT(resolution >= MinHeightfieldResolution && resolution <= MaxHeightfieldResolution, "A heightfield of {} samples a side",
			resolution);
		HeightfieldData heightfield;
		heightfield.Resolution = resolution;
		heightfield.Heights.assign(static_cast<size_t>(resolution) * resolution, height);
		return heightfield;
	}

	Status ValidateHeightfieldData(const HeightfieldData& heightfield)
	{
		if (!Utils::IsHeightfieldResolutionInRange(heightfield.Resolution))
		{
			return MakeError(ErrorCode::Validation, "the heightfield is {} samples a side, outside [{}, {}]", heightfield.Resolution, MinHeightfieldResolution,
				MaxHeightfieldResolution);
		}
		const size_t expected = static_cast<size_t>(heightfield.Resolution) * heightfield.Resolution;
		if (heightfield.Heights.size() != expected)
		{
			return MakeError(ErrorCode::Validation, "the heightfield holds {} samples; {} samples a side need {}", heightfield.Heights.size(),
				heightfield.Resolution, expected);
		}
		return {};
	}

	Buffer SerializeHeightfieldPayload(const HeightfieldData& heightfield)
	{
		ENGINE_CORE_ASSERT(ValidateHeightfieldData(heightfield).has_value(), "SerializeHeightfieldPayload needs a heightfield that passes ValidateHeightfieldData");

		BinaryWriter writer;
		writer.WriteU32(heightfield.Resolution);
		writer.WriteArray(std::span<const uint16_t>(heightfield.Heights));
		return writer.TakeBuffer();
	}

	Result<HeightfieldData> DeserializeHeightfieldPayload(std::span<const std::byte> payload)
	{
		BinaryReader reader(payload);
		ENGINE_TRY_ASSIGN(const uint32_t resolution, reader.ReadU32());
		if (!Utils::IsHeightfieldResolutionInRange(resolution))
		{
			return MakeError(ErrorCode::Parse, "heightfield payload: the resolution {} is outside [{}, {}]", resolution, MinHeightfieldResolution,
				MaxHeightfieldResolution);
		}
		// At most 1025^2 samples of 2 bytes: the product cannot overflow, and it is checked against the bytes left before the
		// samples are allocated.
		const uint64_t sampleCount = static_cast<uint64_t>(resolution) * resolution;
		if (sampleCount * sizeof(uint16_t) > reader.GetRemaining())
		{
			return MakeError(ErrorCode::Parse, "heightfield payload: {} samples a side need {} bytes, but {} follow at offset {}", resolution,
				sampleCount * sizeof(uint16_t), reader.GetRemaining(), reader.GetPosition());
		}
		HeightfieldData heightfield;
		heightfield.Resolution = resolution;
		ENGINE_TRY_ASSIGN(heightfield.Heights, reader.ReadArray<uint16_t>(static_cast<size_t>(sampleCount)));
		if (!reader.IsAtEnd())
			return MakeError(ErrorCode::Parse, "heightfield payload has {} trailing bytes after offset {}", reader.GetRemaining(), reader.GetPosition());
		ENGINE_TRY(WithContext(ValidateHeightfieldData(heightfield), "in a heightfield payload"));
		return heightfield;
	}

	Buffer WriteHeightfieldSource(const HeightfieldData& heightfield)
	{
		const Buffer payload = SerializeHeightfieldPayload(heightfield);
		BinaryWriter writer;
		writer.WriteBytes(std::as_bytes(std::span(HeightfieldSourceMagic)));
		writer.WriteU32(HeightfieldData::FormatVersion);
		writer.WriteBytes(payload);
		return writer.TakeBuffer();
	}

	Result<HeightfieldData> ReadHeightfieldSource(std::span<const std::byte> source)
	{
		BinaryReader reader(source);
		const Result<std::span<const std::byte>> magic = reader.ReadBytes(HeightfieldSourceMagic.size());
		if (!magic.has_value() || !std::ranges::equal(*magic, std::as_bytes(std::span(HeightfieldSourceMagic))))
		{
			return MakeError(ErrorCode::Parse, "not a heightfield source: it does not start with the magic \"EHFD\" ({} bytes)", source.size());
		}
		ENGINE_TRY_ASSIGN(const uint32_t version, WithContext(reader.ReadU32(), "in the heightfield source's header"));
		if (version != HeightfieldData::FormatVersion)
		{
			return std::unexpected(Error(ErrorCode::UnsupportedVersion,
				std::format("heightfield source version {}; this build reads version {}", version, HeightfieldData::FormatVersion))
					.WithHint("write the heightfield again with this engine (terrain.importHeightmap or asset.create)"));
		}
		return DeserializeHeightfieldPayload(source.subspan(Utils::HeightfieldSourceHeaderBytes));
	}

	Buffer CookHeightfield(const HeightfieldData& heightfield, uint32_t importerVersion)
	{
		const Buffer payload = SerializeHeightfieldPayload(heightfield);
		return WriteCookedArtifact(AssetType::Heightfield, HeightfieldData::FormatVersion, importerVersion, payload);
	}

	Result<AssetRef<HeightfieldData>> LoadCookedHeightfield(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView artifact, ReadCookedArtifact(cooked, AssetType::Heightfield, HeightfieldData::FormatVersion));
		ENGINE_TRY_ASSIGN(HeightfieldData heightfield, DeserializeHeightfieldPayload(artifact.Payload));
		return AssetRef<HeightfieldData>(CreateRef<HeightfieldData>(std::move(heightfield)));
	}

}
