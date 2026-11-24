#include "EnginePCH.h"
#include "Engine/Asset/IrradianceVolumeData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"

#include <array>
#include <cmath>
#include <string_view>
#include <utility>

// The irradiance volume's formats (IrradianceVolumeData.h; Docs/Decisions/0037-m26-contract.md decision 8; M45's format 2,
// Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3): the payload, its validation and the `.irradiance` source file
// that lighting.bake writes and IrradianceVolumeImporter reads.

namespace Engine {

	namespace Utils {

		// The bytes of one probe in the payload: nine RGB coefficients of four bytes each.
		constexpr size_t IrradianceProbeBytes = 9 * 3 * sizeof(float);
		// M45: the bytes of one probe's relight block: nine RGB coefficients and the transfer's values.
		constexpr size_t IrradianceRelightBytes = IrradianceProbeBytes + IrradianceTransferCount * sizeof(float);
		// The source header: the magic and the uint32 version.
		constexpr size_t IrradianceSourceHeaderBytes = IrradianceVolumeSourceMagic.size() + sizeof(uint32_t);

		[[nodiscard]] static bool IsValidProbeCount(uint32_t count)
		{
			return count >= MinIrradianceProbesPerAxis && count <= MaxIrradianceProbesPerAxis;
		}

		[[nodiscard]] static size_t GetProbeCount(const IrradianceVolumeData& volume)
		{
			return static_cast<size_t>(volume.CountX) * volume.CountY * volume.CountZ;
		}

		[[nodiscard]] static bool IsFiniteVector(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		static void WriteCoefficients(BinaryWriter& writer, const std::array<glm::vec3, 9>& coefficients)
		{
			for (const glm::vec3& coefficient : coefficients)
			{
				for (glm::length_t channel = 0; channel < 3; ++channel)
					writer.WriteF32(coefficient[channel]);
			}
		}

		[[nodiscard]] static Status ReadCoefficients(BinaryReader& reader, std::array<glm::vec3, 9>& coefficients)
		{
			for (glm::vec3& coefficient : coefficients)
			{
				for (glm::length_t channel = 0; channel < 3; ++channel)
				{
					ENGINE_TRY_ASSIGN(coefficient[channel], reader.ReadF32());
				}
			}
			return {};
		}

		// The payload of `version` (1 or FormatVersion) of a valid volume (no assertion: the callers assert).
		[[nodiscard]] static Buffer WritePayload(BinaryWriter& writer, const IrradianceVolumeData& volume, uint32_t version)
		{
			writer.WriteU32(volume.CountX);
			writer.WriteU32(volume.CountY);
			writer.WriteU32(volume.CountZ);
			for (glm::length_t axis = 0; axis < 3; ++axis)
				writer.WriteF32(volume.Size[axis]);
			for (const IrradianceProbe& probe : volume.Probes)
				WriteCoefficients(writer, probe.SH9);
			if (version >= 2)
			{
				writer.WriteU32(volume.Relight.empty() ? 0U : 1U);
				for (const IrradianceProbeRelight& relight : volume.Relight)
				{
					WriteCoefficients(writer, relight.Local);
					for (const float value : relight.Transfer)
						writer.WriteF32(value);
				}
			}
			return writer.TakeBuffer();
		}

		// Reads the payload of `version` (1 or FormatVersion) from `reader` to its end, then validates it.
		[[nodiscard]] static Result<IrradianceVolumeData> ReadPayload(BinaryReader& reader, uint32_t version)
		{
			IrradianceVolumeData volume;
			ENGINE_TRY_ASSIGN(volume.CountX, reader.ReadU32());
			ENGINE_TRY_ASSIGN(volume.CountY, reader.ReadU32());
			ENGINE_TRY_ASSIGN(volume.CountZ, reader.ReadU32());
			// Each count against its limit before any probe is allocated, so a corrupt count cannot ask for a huge grid.
			for (const uint32_t count : { volume.CountX, volume.CountY, volume.CountZ })
			{
				if (count > MaxIrradianceProbesPerAxis)
				{
					return MakeError(ErrorCode::Parse, "an irradiance volume payload names {} probes along an axis; at most {} are allowed", count,
						MaxIrradianceProbesPerAxis);
				}
			}
			for (glm::length_t axis = 0; axis < 3; ++axis)
			{
				ENGINE_TRY_ASSIGN(volume.Size[axis], reader.ReadF32());
			}
			const size_t probeCount = GetProbeCount(volume);
			const size_t probeBytes = probeCount * IrradianceProbeBytes;
			if (reader.GetRemaining() < probeBytes)
			{
				return MakeError(ErrorCode::Parse, "an irradiance volume payload of {} probes needs {} bytes of coefficients, {} are left", probeCount, probeBytes,
					reader.GetRemaining());
			}
			volume.Probes.resize(probeCount);
			for (IrradianceProbe& probe : volume.Probes)
			{
				ENGINE_TRY(ReadCoefficients(reader, probe.SH9));
			}
			if (version >= 2)
			{
				ENGINE_TRY_ASSIGN(const uint32_t hasRelight, reader.ReadU32());
				if (hasRelight > 1)
					return MakeError(ErrorCode::Parse, "an irradiance volume payload's relight flag is {}; it is 0 or 1", hasRelight);
				if (hasRelight == 1)
				{
					const size_t relightBytes = probeCount * IrradianceRelightBytes;
					if (reader.GetRemaining() < relightBytes)
					{
						return MakeError(ErrorCode::Parse, "an irradiance volume payload's relight block of {} probes needs {} bytes, {} are left", probeCount,
							relightBytes, reader.GetRemaining());
					}
					volume.Relight.resize(probeCount);
					for (IrradianceProbeRelight& relight : volume.Relight)
					{
						ENGINE_TRY(ReadCoefficients(reader, relight.Local));
						for (float& value : relight.Transfer)
						{
							ENGINE_TRY_ASSIGN(value, reader.ReadF32());
						}
					}
				}
			}
			if (!reader.IsAtEnd())
				return MakeError(ErrorCode::Parse, "an irradiance volume payload has {} trailing bytes", reader.GetRemaining());
			ENGINE_TRY(ValidateIrradianceVolumeData(volume));
			return volume;
		}

	}

	size_t GetIrradianceProbeIndex(const IrradianceVolumeData& volume, uint32_t x, uint32_t y, uint32_t z)
	{
		ENGINE_CORE_ASSERT(x < volume.CountX && y < volume.CountY && z < volume.CountZ, "Probe ({}, {}, {}) of a {} x {} x {} volume", x, y, z,
			volume.CountX, volume.CountY, volume.CountZ);
		return static_cast<size_t>(x) + static_cast<size_t>(volume.CountX) * (static_cast<size_t>(y) + static_cast<size_t>(volume.CountY) * z);
	}

	Status ValidateIrradianceVolumeData(const IrradianceVolumeData& volume)
	{
		constexpr std::array<std::string_view, 3> AxisNames = { "x", "y", "z" };
		const std::array<uint32_t, 3> counts = { volume.CountX, volume.CountY, volume.CountZ };
		for (size_t axis = 0; axis < counts.size(); ++axis)
		{
			if (!Utils::IsValidProbeCount(counts[axis]))
			{
				return MakeError(ErrorCode::Validation, "the volume has {} probes along {}; a volume has {} to {} along each axis", counts[axis],
					AxisNames[axis], MinIrradianceProbesPerAxis, MaxIrradianceProbesPerAxis);
			}
		}
		for (glm::length_t axis = 0; axis < 3; ++axis)
		{
			const float side = volume.Size[axis];
			if (!std::isfinite(side) || side <= 0.0f || side > MaxIrradianceVolumeSize)
			{
				return MakeError(ErrorCode::Validation, "the volume's size along {} is {}; each side is finite and in (0, {}] metres", AxisNames[static_cast<size_t>(axis)],
					side, MaxIrradianceVolumeSize);
			}
		}
		const size_t expected = Utils::GetProbeCount(volume);
		if (volume.Probes.size() != expected)
		{
			return MakeError(ErrorCode::Validation, "the volume holds {} probes; its {} x {} x {} grid has {}", volume.Probes.size(), volume.CountX,
				volume.CountY, volume.CountZ, expected);
		}
		for (size_t probe = 0; probe < volume.Probes.size(); ++probe)
		{
			for (size_t coefficient = 0; coefficient < volume.Probes[probe].SH9.size(); ++coefficient)
			{
				if (!Utils::IsFiniteVector(volume.Probes[probe].SH9[coefficient]))
					return MakeError(ErrorCode::Validation, "probe {}'s coefficient {} is not finite", probe, coefficient);
			}
		}
		if (!volume.Relight.empty() && volume.Relight.size() != expected)
			return MakeError(ErrorCode::Validation, "the volume holds {} relight blocks for its {} probes; a relightable volume has one per probe", volume.Relight.size(), expected);
		for (size_t probe = 0; probe < volume.Relight.size(); ++probe)
		{
			const IrradianceProbeRelight& relight = volume.Relight[probe];
			for (size_t coefficient = 0; coefficient < relight.Local.size(); ++coefficient)
			{
				if (!Utils::IsFiniteVector(relight.Local[coefficient]))
					return MakeError(ErrorCode::Validation, "probe {}'s local coefficient {} is not finite", probe, coefficient);
			}
			for (size_t index = 0; index < relight.Transfer.size(); ++index)
			{
				if (!std::isfinite(relight.Transfer[index]))
					return MakeError(ErrorCode::Validation, "probe {}'s sky transfer value {} is not finite", probe, index);
			}
		}
		return {};
	}

	Buffer SerializeIrradianceVolumePayload(const IrradianceVolumeData& volume)
	{
		ENGINE_CORE_ASSERT(ValidateIrradianceVolumeData(volume).has_value(), "Serializing an invalid irradiance volume");
		BinaryWriter writer;
		return Utils::WritePayload(writer, volume, IrradianceVolumeData::FormatVersion);
	}

	Result<IrradianceVolumeData> DeserializeIrradianceVolumePayload(std::span<const std::byte> payload)
	{
		BinaryReader reader(payload);
		return Utils::ReadPayload(reader, IrradianceVolumeData::FormatVersion);
	}

	Buffer WriteIrradianceVolumeSource(const IrradianceVolumeData& volume)
	{
		ENGINE_CORE_ASSERT(ValidateIrradianceVolumeData(volume).has_value(), "Writing the source of an invalid irradiance volume");
		// M45: a volume without a relight block keeps M26's version 1, byte for byte.
		const uint32_t version = volume.Relight.empty() ? IrradianceVolumeSourceVersionWithoutRelight : IrradianceVolumeData::FormatVersion;
		BinaryWriter writer;
		for (const char character : IrradianceVolumeSourceMagic)
			writer.WriteU8(static_cast<uint8_t>(character));
		writer.WriteU32(version);
		return Utils::WritePayload(writer, volume, version);
	}

	Result<IrradianceVolumeData> ReadIrradianceVolumeSource(std::span<const std::byte> source)
	{
		BinaryReader reader(source);
		for (const char expected : IrradianceVolumeSourceMagic)
		{
			const Result<uint8_t> character = reader.ReadU8();
			if (!character.has_value() || *character != static_cast<uint8_t>(expected))
				return MakeError(ErrorCode::Parse, "not an irradiance volume source: the file does not start with \"EIRV\"");
		}
		ENGINE_TRY_ASSIGN(const uint32_t version, reader.ReadU32());
		if (version != IrradianceVolumeSourceVersionWithoutRelight && version != IrradianceVolumeData::FormatVersion)
		{
			return MakeError(ErrorCode::UnsupportedVersion, "irradiance volume source version {}; this build reads versions {} and {}", version,
				IrradianceVolumeSourceVersionWithoutRelight, IrradianceVolumeData::FormatVersion);
		}
		BinaryReader payload(source.subspan(Utils::IrradianceSourceHeaderBytes));
		return Utils::ReadPayload(payload, version);
	}

	Buffer CookIrradianceVolume(const IrradianceVolumeData& volume, uint32_t importerVersion)
	{
		const Buffer payload = SerializeIrradianceVolumePayload(volume);
		return WriteCookedArtifact(AssetType::IrradianceVolume, IrradianceVolumeData::FormatVersion, importerVersion, payload);
	}

	Result<AssetRef<IrradianceVolumeData>> LoadCookedIrradianceVolume(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView artifact, ReadCookedArtifact(cooked, AssetType::IrradianceVolume, IrradianceVolumeData::FormatVersion));
		ENGINE_TRY_ASSIGN(IrradianceVolumeData volume, DeserializeIrradianceVolumePayload(artifact.Payload));
		return AssetRef<IrradianceVolumeData>(CreateRef<IrradianceVolumeData>(std::move(volume)));
	}

}
