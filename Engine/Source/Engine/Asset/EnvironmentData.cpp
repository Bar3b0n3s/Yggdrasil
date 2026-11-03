#include "EnginePCH.h"
#include "Engine/Asset/EnvironmentData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/Error.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace Engine {

	namespace Utils {

		// The full mip chain of a face: floor(log2(faceSize)) + 1 levels (faceSize >= 1).
		[[nodiscard]] static uint32_t GetFullMipCount(uint32_t faceSize)
		{
			return static_cast<uint32_t>(std::bit_width(faceSize));
		}

		[[nodiscard]] static bool IsCubeMapFormat(CubeMapFormat format)
		{
			return format == CubeMapFormat::Rgba16Float || format == CubeMapFormat::BC6HUfloat;
		}

		// Whether a binary16 value is finite and not negative: its exponent is not all ones (infinity, NaN), and its sign bit
		// is clear or the value is zero (-0 equals 0).
		[[nodiscard]] static bool IsFiniteNonNegativeHalf(uint16_t half)
		{
			const bool finite = (half & 0x7C00U) != 0x7C00U;
			const bool negative = (half & 0x8000U) != 0 && (half & 0x7FFFU) != 0;
			return finite && !negative;
		}

		// Where the binary16 value at `valueIndex` of an Rgba16Float cube's texels lies, as "mip M, face F, texel (column,
		// row)".
		[[nodiscard]] static std::string DescribeCubeValue(const CubeMapData& cube, size_t valueIndex)
		{
			size_t texel = valueIndex / 4;
			for (uint32_t level = 0; level < cube.MipCount; ++level)
			{
				const size_t edge = std::max<size_t>(cube.FaceSize >> std::min(level, 31U), 1);
				const size_t faceTexels = edge * edge;
				if (texel < faceTexels * CubeMapData::FaceCount)
				{
					const size_t face = texel / faceTexels;
					const size_t inFace = texel % faceTexels;
					return std::format("mip {}, face {}, texel ({}, {})", level, face, inFace % edge, inFace / edge);
				}
				texel -= faceTexels * CubeMapData::FaceCount;
			}
			return std::format("value {}", valueIndex);
		}

		// The format, size, mip-chain and texel checks of one cube; `name` names it in messages ("skybox", "specular cube").
		[[nodiscard]] static Status ValidateCube(const CubeMapData& cube, std::string_view name)
		{
			if (!IsCubeMapFormat(cube.Format))
				return MakeError(ErrorCode::Validation, "the {} has the unknown format {}", name, std::to_underlying(cube.Format));
			const size_t expected = ComputeCubeMapByteSize(cube.FaceSize, cube.MipCount, cube.Format);
			if (cube.Texels.size() != expected)
			{
				return MakeError(ErrorCode::Validation, "the {} {} of {}x{} with {} mips holds {} texel bytes instead of {}", CubeMapFormatToString(cube.Format), name,
					cube.FaceSize, cube.FaceSize, cube.MipCount, cube.Texels.size(), expected);
			}
			// BC6H's unsigned half floats decode to finite, non-negative values whatever the block bytes (the header).
			if (cube.Format == CubeMapFormat::BC6HUfloat)
				return {};
			const size_t valueCount = cube.Texels.size() / sizeof(uint16_t);
			for (size_t index = 0; index < valueCount; ++index)
			{
				uint16_t half = 0;
				std::memcpy(&half, cube.Texels.data() + index * sizeof(uint16_t), sizeof(half));
				if (!IsFiniteNonNegativeHalf(half))
				{
					return MakeError(ErrorCode::Validation, "the {} holds a non-finite or negative value (binary16 0x{:04X}) at {}", name, half,
						DescribeCubeValue(cube, index));
				}
			}
			return {};
		}

		// One cube's header: its face size, mip count, format byte and three reserved zero bytes.
		static void WriteCubeHeader(BinaryWriter& writer, const CubeMapData& cube)
		{
			writer.WriteU32(cube.FaceSize);
			writer.WriteU32(cube.MipCount);
			writer.WriteU8(std::to_underlying(cube.Format));
			for (int reserved = 0; reserved < 3; ++reserved)
				writer.WriteU8(0);
		}

		// Reads one cube's header into `cube` (its texels stay empty). Errors: Parse for truncation, a format byte outside
		// CubeMapFormat or a reserved byte that is not 0, naming `name`.
		[[nodiscard]] static Status ReadCubeHeader(BinaryReader& reader, CubeMapData& cube, std::string_view name)
		{
			ENGINE_TRY_ASSIGN(cube.FaceSize, reader.ReadU32());
			ENGINE_TRY_ASSIGN(cube.MipCount, reader.ReadU32());
			ENGINE_TRY_ASSIGN(const uint8_t format, reader.ReadU8());
			cube.Format = static_cast<CubeMapFormat>(format);
			if (!IsCubeMapFormat(cube.Format))
				return MakeError(ErrorCode::Parse, "environment payload: the {} has the unknown format {} at offset {}", name, format, reader.GetPosition() - 1);
			for (int index = 0; index < 3; ++index)
			{
				ENGINE_TRY_ASSIGN(const uint8_t reserved, reader.ReadU8());
				if (reserved != 0)
					return MakeError(ErrorCode::Parse, "environment payload: the {}'s reserved byte at offset {} is {}, not 0", name, reader.GetPosition() - 1, reserved);
			}
			return {};
		}

	}

	std::string_view CubeMapFormatToString(CubeMapFormat format)
	{
		switch (format)
		{
			case CubeMapFormat::Rgba16Float: return "Rgba16Float";
			case CubeMapFormat::BC6HUfloat:  return "BC6HUfloat";
		}
		return "Unknown";
	}

	Status ValidateEnvironmentData(const EnvironmentData& environment)
	{
		const CubeMapData& skybox = environment.Skybox;
		if (skybox.FaceSize < 1 || skybox.FaceSize > EnvironmentData::MaxSkyboxFaceSize)
		{
			return MakeError(ErrorCode::Validation, "the skybox face of {} texels is outside 1 to {}", skybox.FaceSize,
				EnvironmentData::MaxSkyboxFaceSize);
		}
		if (skybox.MipCount != Utils::GetFullMipCount(skybox.FaceSize))
		{
			return MakeError(ErrorCode::Validation, "the skybox of {}x{} has {} mips instead of its full chain of {}", skybox.FaceSize, skybox.FaceSize, skybox.MipCount,
				Utils::GetFullMipCount(skybox.FaceSize));
		}
		const CubeMapData& specular = environment.Specular;
		if (specular.FaceSize != EnvironmentData::SpecularFaceSize || specular.MipCount != EnvironmentData::SpecularMipCount)
		{
			return MakeError(ErrorCode::Validation, "the specular cube is {}x{} with {} mips instead of {}x{} with {}", specular.FaceSize, specular.FaceSize,
				specular.MipCount, EnvironmentData::SpecularFaceSize, EnvironmentData::SpecularFaceSize, EnvironmentData::SpecularMipCount);
		}
		ENGINE_TRY(Utils::ValidateCube(skybox, "skybox"));
		ENGINE_TRY(Utils::ValidateCube(specular, "specular cube"));
		for (size_t index = 0; index < environment.IrradianceSH9.size(); ++index)
		{
			const glm::vec3& coefficient = environment.IrradianceSH9[index];
			if (!std::isfinite(coefficient.x) || !std::isfinite(coefficient.y) || !std::isfinite(coefficient.z))
				return MakeError(ErrorCode::Validation, "the SH9 irradiance coefficient {} is not finite", index);
		}
		return {};
	}

	Buffer SerializeEnvironmentPayload(const EnvironmentData& environment)
	{
		ENGINE_CORE_ASSERT(ValidateEnvironmentData(environment).has_value(), "SerializeEnvironmentPayload needs a valid environment");
		BinaryWriter writer;
		Utils::WriteCubeHeader(writer, environment.Skybox);
		Utils::WriteCubeHeader(writer, environment.Specular);
		for (const glm::vec3& coefficient : environment.IrradianceSH9)
		{
			writer.WriteF32(coefficient.x);
			writer.WriteF32(coefficient.y);
			writer.WriteF32(coefficient.z);
		}
		writer.WriteBytes(environment.Skybox.Texels);
		writer.WriteBytes(environment.Specular.Texels);
		return writer.TakeBuffer();
	}

	Result<EnvironmentData> DeserializeEnvironmentPayload(std::span<const std::byte> payload)
	{
		BinaryReader reader(payload);
		EnvironmentData environment;
		ENGINE_TRY(Utils::ReadCubeHeader(reader, environment.Skybox, "skybox"));
		ENGINE_TRY(Utils::ReadCubeHeader(reader, environment.Specular, "specular cube"));
		for (glm::vec3& coefficient : environment.IrradianceSH9)
		{
			ENGINE_TRY_ASSIGN(coefficient.x, reader.ReadF32());
			ENGINE_TRY_ASSIGN(coefficient.y, reader.ReadF32());
			ENGINE_TRY_ASSIGN(coefficient.z, reader.ReadF32());
		}

		// The texel sizes come from untrusted header values: ComputeCubeMapByteSize is total (it saturates instead of
		// overflowing), and both are checked against the bytes left before anything is allocated (§6.8).
		const CubeMapData& skyboxHeader = environment.Skybox;
		const CubeMapData& specularHeader = environment.Specular;
		const size_t skyboxBytes = ComputeCubeMapByteSize(skyboxHeader.FaceSize, skyboxHeader.MipCount, skyboxHeader.Format);
		const size_t specularBytes = ComputeCubeMapByteSize(specularHeader.FaceSize, specularHeader.MipCount, specularHeader.Format);
		const size_t remaining = reader.GetRemaining();
		if (skyboxBytes > remaining || specularBytes != remaining - skyboxBytes)
		{
			return MakeError(ErrorCode::Parse,
				"environment payload: the header's {} {}x{} skybox with {} mips and {} {}x{} specular cube with {} mips do not match the {} texel "
				"bytes that follow at offset {}",
				CubeMapFormatToString(skyboxHeader.Format), skyboxHeader.FaceSize, skyboxHeader.FaceSize, skyboxHeader.MipCount,
				CubeMapFormatToString(specularHeader.Format), specularHeader.FaceSize, specularHeader.FaceSize, specularHeader.MipCount, remaining,
				reader.GetPosition());
		}
		ENGINE_TRY_ASSIGN(const std::span<const std::byte> skybox, reader.ReadBytes(skyboxBytes));
		environment.Skybox.Texels.assign(skybox.begin(), skybox.end());
		ENGINE_TRY_ASSIGN(const std::span<const std::byte> specular, reader.ReadBytes(specularBytes));
		environment.Specular.Texels.assign(specular.begin(), specular.end());
		ENGINE_TRY(ValidateEnvironmentData(environment));
		return environment;
	}

	Buffer CookEnvironment(const EnvironmentData& environment, uint32_t importerVersion)
	{
		return WriteCookedArtifact(AssetType::Environment, EnvironmentData::FormatVersion, importerVersion, SerializeEnvironmentPayload(environment));
	}

	Result<AssetRef<EnvironmentData>> LoadCookedEnvironment(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView view, ReadCookedArtifact(cooked, AssetType::Environment, EnvironmentData::FormatVersion));
		ENGINE_TRY_ASSIGN(EnvironmentData environment, DeserializeEnvironmentPayload(view.Payload));
		return AssetRef<EnvironmentData>(CreateRef<EnvironmentData>(std::move(environment)));
	}

}
