#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// MaterialConstants::AlphaMode values: MaterialData's AlphaMode (glTF 2.0, §8.5) by its enumerator value.
#if defined(ENGINE_SHADER)
	static const uint MaterialAlphaModeOpaque = 0;
	static const uint MaterialAlphaModeMask = 1;
	static const uint MaterialAlphaModeBlend = 2;
#else
inline constexpr uint32_t MaterialAlphaModeOpaque = 0;
inline constexpr uint32_t MaterialAlphaModeMask = 1;
inline constexpr uint32_t MaterialAlphaModeBlend = 2;
#endif

	// MaterialConstants::Flags bits. MaterialFlagEmissiveMap: the material has an emissive map. An empty emissive slot binds
	// Black (§8.4), so without the bit the shader takes the emissive factor alone, as glTF 2.0 defines it.
#if defined(ENGINE_SHADER)
	static const uint MaterialFlagEmissiveMap = 1;
#else
inline constexpr uint32_t MaterialFlagEmissiveMap = 1;
#endif

	// M23 (C23.1, Material version 2 and BC compression; Docs/Decisions/0034-m23-contract.md decisions 10 and 13): a map
	// whose bit is set samples the second texture coordinate set (MaterialData's <Map>UVSet UV1, MeshVertexAttributes'
	// TexCoord1); MaterialFlagNormalMapTwoChannel marks a normal map stored in two channels (BC5: x and y in R and G), whose
	// z the shader reconstructs as sqrt(saturate(1 - x² - y²)), set from the normal map's TextureFormat (GpuResourceCache).
#if defined(ENGINE_SHADER)
	static const uint MaterialFlagBaseColorUV1 = 2;
	static const uint MaterialFlagMetallicRoughnessUV1 = 4;
	static const uint MaterialFlagNormalUV1 = 8;
	static const uint MaterialFlagOcclusionUV1 = 16;
	static const uint MaterialFlagEmissiveUV1 = 32;
	static const uint MaterialFlagNormalMapTwoChannel = 64;
#else
inline constexpr uint32_t MaterialFlagBaseColorUV1 = 2;
inline constexpr uint32_t MaterialFlagMetallicRoughnessUV1 = 4;
inline constexpr uint32_t MaterialFlagNormalUV1 = 8;
inline constexpr uint32_t MaterialFlagOcclusionUV1 = 16;
inline constexpr uint32_t MaterialFlagEmissiveUV1 = 32;
inline constexpr uint32_t MaterialFlagNormalMapTwoChannel = 64;
#endif

	// The per-material constants: b0 of descriptor set 1 (§8.4), one buffer per material mirror (GpuResourceCache::GetMaterial,
	// written once per material version). The factors of the glTF 2.0 metallic-roughness model (§8.5, MaterialData), each
	// multiplied in the shader with its map of set 1 (an empty slot binds White, FlatNormal or Black, so the factor alone
	// remains):
	//   - BaseColor: linear RGBA factor times the sRGB base-colour map (decoded by its SRGBA8 format);
	//   - Emissive: MaterialData::Emissive times EmissiveStrength (linear radiance), times the sRGB emissive map when Flags has
	//     MaterialFlagEmissiveMap;
	//   - Metallic and Roughness (perceptual): times the map's B and G channels; shading clamps the roughness to at least
	//     0.045 (§8.5);
	//   - NormalScale scales the tangent-space normal map's X and Y; OcclusionStrength blends the occlusion map's R towards 1;
	//   - AlphaCutoff: Mask materials discard below it (prepass and forward pass); Blend materials write the base colour's
	//     alpha for the transparent pass's blending, every other mode writes 1;
	//   - UVScale and UVOffset map the mesh's texture coordinates (uv * UVScale + UVOffset) for every map.
	struct MaterialConstants
	{
		Float4 BaseColor;
		Float3 Emissive;
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		float NormalScale = 1.0f;
		float OcclusionStrength = 1.0f;
		float AlphaCutoff = 0.5f;
		Float2 UVScale;
		Float2 UVOffset;
		uint32_t AlphaMode = MaterialAlphaModeOpaque;
		uint32_t Flags = 0;
		uint32_t Padding0 = 0;
		uint32_t Padding1 = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(MaterialConstants) == 80, "MaterialConstants must match its 80-byte constant-buffer layout");
	static_assert(offsetof(MaterialConstants, Emissive) == 16 && offsetof(MaterialConstants, Roughness) == 32
			&& offsetof(MaterialConstants, UVScale) == 48 && offsetof(MaterialConstants, AlphaMode) == 64,
		"MaterialConstants members must sit where the constant-buffer layout puts them");

}
#endif
