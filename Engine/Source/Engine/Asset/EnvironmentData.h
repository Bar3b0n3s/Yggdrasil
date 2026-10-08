#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

// Baked image-based lighting (Architecture §6.8 "environment: skybox cube RGBA16F with mips, specular cube 256² x 7 mips,
// SH9 coefficients", §8.6). M6 defined the CPU shape because IEnvironmentBaker, an M6 interface, produces it; the M8 contract
// froze the conventions below and the cooked payload (Docs/Decisions/0013-m8-decisions.md decision 9), which
// EnvironmentImporter (AssetPipeline) writes and the Environment loader reads.
//
// Conventions, shared by the baker (Renderer/EnvironmentBaker), the shaders (Shared/EnvironmentConstants.h,
// Common/Environment.slang) and the CPU references (Tests/Source/Support/RenderReference.h):
//   - World space is right-handed with +Y up (§5.2); a cube is sampled with the world direction (rotated by the
//     environment's Rotation, EnvironmentConstants.h).
//   - Equirectangular sources: the texel at (u, v) (u = (column + 0.5) / width, v = (row + 0.5) / height, row 0 at the top)
//     is the radiance arriving from direction (sin(theta) sin(phi), cos(theta), -sin(theta) cos(phi)) with theta = v * pi
//     and phi = (u - 0.5) * 2 pi: the image centre is -Z (the default camera's view), u = 0.75 is +X, the top row is +Y.
//   - Cube faces follow Vulkan's cube-map face selection (Vulkan specification, "Cube Map Face Selection"): face f in the
//     order +X, -X, +Y, -Y, +Z, -Z, and the texel (column i, row j) of a face of size n holds the radiance arriving from the
//     direction whose major axis is f and whose (s, t) = ((i + 0.5) / n, (j + 0.5) / n) per that table (row 0 at t = 0).
//   - IrradianceSH9 holds the coefficients of E(n) / pi, the cosine-convolved irradiance divided by pi (so the diffuse
//     radiance of a Lambertian surface is albedo * the SH evaluated at its normal, and a constant environment of radiance L
//     evaluates to L everywhere), with the Hanning window of EnvironmentData::IrradianceShWindow applied, over the real
//     SH basis Y00 = 0.282095, Y1-1 = 0.488603 y, Y10 = 0.488603 z, Y11 = 0.488603 x, Y2-2 = 1.092548 xy,
//     Y2-1 = 1.092548 yz, Y20 = 0.315392 (3z² - 1), Y21 = 1.092548 xz, Y22 = 0.546274 (x² - y²), with (x, y, z) the world
//     direction.

namespace Engine {

	// A cube map with a mip chain of RGBA16_FLOAT texels (8 bytes each: four IEEE 754 binary16 values, little-endian).
	// Texels are laid out level by level from mip 0; each level holds the six faces in the order +X, -X, +Y, -Y, +Z, -Z
	// (Vulkan's layer order); each face holds max(1, FaceSize >> level) rows of as many texels, top row first.
	struct CubeMapData
	{
		static constexpr uint32_t BytesPerTexel = 8;
		static constexpr uint32_t FaceCount = 6;

		uint32_t FaceSize = 0; // edge length of mip 0 in texels
		uint32_t MipCount = 0; // 1 to the full chain
		Buffer Texels{};
	};

	// The bytes of a cube's texels: BytesPerTexel * FaceCount * the sum over its mips of max(1, floor(faceSize / 2^level))².
	// 0 for a zero size or mip count. Total over every input, because the payload reader and ValidateEnvironmentData apply it
	// to untrusted header values: at most 32 loop iterations (the levels past the last halving are 1x1 and counted at once),
	// no shift by 32 or more, and a size that does not fit in size_t saturates at its maximum (no payload is that long, so
	// it is rejected) instead of overflowing. Pure.
	[[nodiscard]] constexpr size_t ComputeCubeMapByteSize(uint32_t faceSize, uint32_t mipCount)
	{
		if (faceSize == 0 || mipCount == 0)
			return 0;
		constexpr size_t MaxBytes = std::numeric_limits<size_t>::max();
		constexpr size_t BytesPerSquare = size_t{ CubeMapData::FaceCount } * CubeMapData::BytesPerTexel; // one texel per face
		size_t bytes = 0;
		uint64_t edge = faceSize;
		uint32_t level = 0;
		for (; level < mipCount && edge > 1; ++level)
		{
			const uint64_t texels = edge * edge; // at most (2^32 - 1)², which fits
			if (texels > (MaxBytes - bytes) / BytesPerSquare)
				return MaxBytes;
			bytes += static_cast<size_t>(texels) * BytesPerSquare;
			edge /= 2;
		}
		const uint64_t remaining = mipCount - level; // the 1x1 levels
		if (remaining > (MaxBytes - bytes) / BytesPerSquare)
			return MaxBytes;
		return bytes + static_cast<size_t>(remaining) * BytesPerSquare;
	}

	// A loaded environment (AssetType::Environment). Immutable once loaded (AssetRef<EnvironmentData>).
	struct EnvironmentData : Asset
	{
		static constexpr AssetType StaticType = AssetType::Environment;
		// The specular cube's size and mip count (§8.6 step 3: 256² x 7 mips, mip i = perceptual roughness i / 6).
		static constexpr uint32_t SpecularFaceSize = 256;
		static constexpr uint32_t SpecularMipCount = 7;
		// The largest skybox face (§8.6 step 2: min(1024, width / 4)).
		static constexpr uint32_t MaxSkyboxFaceSize = 1024;
		// The Hanning window of the SH9 irradiance (§8.6 step 4): band l is scaled by (1 + cos(pi l / w)) / 2 with w =
		// IrradianceShWindow, so 1, 0.854 and 0.5 for l = 0, 1, 2.
		static constexpr float IrradianceShWindow = 4.0f;
		// The cooked payload layout below (CookedHeader::FormatVersion).
		static constexpr uint16_t FormatVersion = 1;

		EnvironmentData()
			: Asset(StaticType)
		{
		}

		CubeMapData Skybox{};   // face max(1, min(1024, equirect width / 4)), full mip chain (§8.6 step 2)
		CubeMapData Specular{}; // SpecularFaceSize, SpecularMipCount levels
		// Diffuse irradiance as L2 spherical harmonics (§8.6 step 4; the convention of the file comment), RGB per
		// coefficient, in the order (l, m) = (0, 0), (1, -1), (1, 0), (1, 1), (2, -2), (2, -1), (2, 0), (2, 1), (2, 2).
		std::array<glm::vec3, 9> IrradianceSH9{};
	};

	// Checks: Skybox FaceSize 1 to MaxSkyboxFaceSize with a full mip chain (floor(log2(FaceSize)) + 1 mips); Specular exactly
	// SpecularFaceSize with SpecularMipCount mips; each cube's Texels exactly ComputeCubeMapByteSize bytes, every texel
	// finite and non-negative (binary16 infinities, NaNs and negative values rejected); every SH coefficient finite. Pure.
	// Errors: Validation naming the first violation.
	[[nodiscard]] Status ValidateEnvironmentData(const EnvironmentData& environment);

	// The cooked environment payload (FormatVersion 1), little-endian:
	//     uint32 SkyboxFaceSize; uint32 SkyboxMipCount; uint32 SpecularFaceSize; uint32 SpecularMipCount;
	//     9 x f32x3 IrradianceSH9;
	//     the skybox texels (CubeMapData layout), then the specular texels.
	// Asserts ValidateEnvironmentData. Pure; identical environments give identical bytes.
	[[nodiscard]] Buffer SerializeEnvironmentPayload(const EnvironmentData& environment);

	// Reads a payload written by SerializeEnvironmentPayload, then ValidateEnvironmentData. Never asserts on data; sizes
	// are checked before anything is allocated. Errors: Parse for truncation or trailing bytes; Validation from
	// ValidateEnvironmentData.
	[[nodiscard]] Result<EnvironmentData> DeserializeEnvironmentPayload(std::span<const std::byte> payload);

	// The complete cooked artifact (CookedHeader + payload) of `environment`.
	[[nodiscard]] Buffer CookEnvironment(const EnvironmentData& environment, uint32_t importerVersion);

	// The environment of a cooked artifact (the Environment loader of both asset managers, AssetLoaderRegistry). Errors: as
	// ReadCookedArtifact and DeserializeEnvironmentPayload.
	[[nodiscard]] Result<AssetRef<EnvironmentData>> LoadCookedEnvironment(std::span<const std::byte> cooked);

}
