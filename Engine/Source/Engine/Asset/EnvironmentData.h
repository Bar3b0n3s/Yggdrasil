#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

// Baked image-based lighting (Architecture §6.8 "environment: skybox cube RGBA16F with mips, specular cube 256² x 7 mips,
// SH9 coefficients", §8.6). M6 defines the CPU shape because IEnvironmentBaker, an M6 interface, produces it. The cooked
// payload, its FormatVersion and the loader arrive with EnvironmentImporter in M8, which owns their layout.

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

	// A loaded environment (AssetType::Environment). Immutable once loaded (AssetRef<EnvironmentData>).
	struct EnvironmentData : Asset
	{
		static constexpr AssetType StaticType = AssetType::Environment;
		// The specular cube's size and mip count (§8.6 step 3: 256² x 7 mips, mip i = perceptual roughness i / 6).
		static constexpr uint32_t SpecularFaceSize = 256;
		static constexpr uint32_t SpecularMipCount = 7;

		EnvironmentData()
			: Asset(StaticType)
		{
		}

		CubeMapData Skybox{};   // face min(1024, equirect width / 4), full mip chain (§8.6 step 2)
		CubeMapData Specular{}; // SpecularFaceSize, SpecularMipCount levels
		// Diffuse irradiance as L2 spherical harmonics (§8.6 step 4), RGB per coefficient, in the order
		// (l, m) = (0, 0), (1, -1), (1, 0), (1, 1), (2, -2), (2, -1), (2, 0), (2, 1), (2, 2).
		std::array<glm::vec3, 9> IrradianceSH9{};
	};

}
