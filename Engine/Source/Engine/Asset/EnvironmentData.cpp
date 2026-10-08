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

		// Whether a binary16 value is finite and not negative: its exponent is not all ones (infinity, NaN), and its sign bit
		// is clear or the value is zero (-0 equals 0).
		[[nodiscard]] static bool IsFiniteNonNegativeHalf(uint16_t half)
		{
			const bool finite = (half & 0x7C00U) != 0x7C00U;
			const bool negative = (half & 0x8000U) != 0 && (half & 0x7FFFU) != 0;
			return finite && !negative;
		}

		// Where the binary16 value at `valueIndex` of a cube's texels lies, as "mip M, face F, texel (column, row)".
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

		// The size, mip-chain and texel checks of one cube; `name` names it in messages ("skybox", "specular cube").
		[[nodiscard]] static Status ValidateCube(const CubeMapData& cube, std::string_view name)
		{
			const size_t expected = ComputeCubeMapByteSize(cube.FaceSize, cube.MipCount);
			if (cube.Texels.size() != expected)
			{
				return MakeError(ErrorCode::Validation, "the {} of {}x{} with {} mips holds {} texel bytes instead of {}", name, cube.FaceSize, cube.FaceSize, cube.MipCount,
					cube.Texels.size(), expected);
			}
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
		writer.WriteU32(environment.Skybox.FaceSize);
		writer.WriteU32(environment.Skybox.MipCount);
		writer.WriteU32(environment.Specular.FaceSize);
		writer.WriteU32(environment.Specular.MipCount);
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
		ENGINE_TRY_ASSIGN(environment.Skybox.FaceSize, reader.ReadU32());
		ENGINE_TRY_ASSIGN(environment.Skybox.MipCount, reader.ReadU32());
		ENGINE_TRY_ASSIGN(environment.Specular.FaceSize, reader.ReadU32());
		ENGINE_TRY_ASSIGN(environment.Specular.MipCount, reader.ReadU32());
		for (glm::vec3& coefficient : environment.IrradianceSH9)
		{
			ENGINE_TRY_ASSIGN(coefficient.x, reader.ReadF32());
			ENGINE_TRY_ASSIGN(coefficient.y, reader.ReadF32());
			ENGINE_TRY_ASSIGN(coefficient.z, reader.ReadF32());
		}

		// The texel sizes come from untrusted header values: ComputeCubeMapByteSize is total (it saturates instead of
		// overflowing), and both are checked against the bytes left before anything is allocated (§6.8).
		const size_t skyboxBytes = ComputeCubeMapByteSize(environment.Skybox.FaceSize, environment.Skybox.MipCount);
		const size_t specularBytes = ComputeCubeMapByteSize(environment.Specular.FaceSize, environment.Specular.MipCount);
		const size_t remaining = reader.GetRemaining();
		if (skyboxBytes > remaining || specularBytes != remaining - skyboxBytes)
		{
			return MakeError(ErrorCode::Parse,
				"environment payload: the header's {}x{} skybox with {} mips and {}x{} specular cube with {} mips do not match the {} texel bytes that "
				"follow at offset {}",
				environment.Skybox.FaceSize, environment.Skybox.FaceSize, environment.Skybox.MipCount, environment.Specular.FaceSize,
				environment.Specular.FaceSize, environment.Specular.MipCount, remaining, reader.GetPosition());
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
