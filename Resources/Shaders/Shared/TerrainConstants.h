#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// The terrain passes' shared structs (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 22, stream T-C;
	// Renderer/TerrainPass.h, Passes/Terrain.slang): one TerrainConstants per drawn terrain and view (b0 of the terrain's set
	// 1), and one TerrainDrawConstants per chunk draw (the push constants, at the mesh pipelines' DrawConstants slot and size,
	// so the forward pipeline binds the mesh forward passes' set 0 as it is).
	//
	// TerrainConstants: World places the terrain's local space (Asset/TerrainGeometry.h: sample (x, z) at (x CellSize.x,
	// texel HeightScale, z CellSize.y)); the prepass writes the camera's motion alone to SceneVelocity from ViewConstants'
	// view-projections (a terrain has no previous world, RenderSnapshot.h's TerrainItem). Layer i (below LayerCount; the
	// others weigh nothing): LayerTints[i] its linear tint (rgb) and the inverse of its Tiling (a: local metres to its maps'
	// coordinates), LayerSurfaces[i] its perceptual Roughness (x) and Metallic (y), and z 1 when its normal map holds x and y
	// only (a BC5 map: z is reconstructed, as MaterialFlagNormalMapTwoChannel does for meshes). InverseSize maps local XZ to
	// the splat map's coordinates (1 / Size).
	// Flags: TerrainFlag*. PickId: the picking prepass's EntityId. DebugView: RenderDebugView's value, Lit or a material
	// view (the forward pass outputs that view's quantity, as the Scene program's specialization does).
#if defined(ENGINE_SHADER)
	static const uint MaxTerrainShaderLayers = 4;
	static const uint TerrainFlagReceiveShadows = 1;
	static const uint TerrainFlagSplatMap = 2;
	static const uint TerrainFlagMirrored = 4;
#else
inline constexpr uint32_t MaxTerrainShaderLayers = 4;
inline constexpr uint32_t TerrainFlagReceiveShadows = 1; // the forward pass samples the view's shadow maps
inline constexpr uint32_t TerrainFlagSplatMap = 2;       // the splat map weighs the layers; without it layer 0 alone
inline constexpr uint32_t TerrainFlagMirrored = 4;       // World's determinant is negative: normals and the bitangent flip
#endif

	struct TerrainConstants
	{
		Float4x4 World;
		Float4 LayerTints[4];
		Float4 LayerSurfaces[4];
		Float2 CellSize; // metres per cell along local X and Z: Size / (Resolution - 1)
		float HeightScale = 16.0f;
		uint32_t Resolution = 0;
		Float2 InverseSize;
		uint32_t LayerCount = 1;
		uint32_t Flags = 0;
		uint32_t PickId = 0;
		uint32_t DebugView = 0;
		uint32_t Padding0 = 0;
		uint32_t Padding1 = 0;
	};

	// The per-chunk push constants: the sample its local vertices (Renderer/TerrainLod.h's samples less the chunk's first)
	// start from.
	struct TerrainDrawConstants
	{
		UInt2 ChunkOrigin;
		uint32_t Padding0 = 0;
		uint32_t Padding1 = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(TerrainConstants) == 240, "TerrainConstants must match its 240-byte constant-buffer layout");
	static_assert(offsetof(TerrainConstants, LayerTints) == 64 && offsetof(TerrainConstants, LayerSurfaces) == 128
			&& offsetof(TerrainConstants, CellSize) == 192 && offsetof(TerrainConstants, InverseSize) == 208
			&& offsetof(TerrainConstants, PickId) == 224,
		"TerrainConstants members must sit where the constant-buffer layout puts them");
	static_assert(sizeof(TerrainDrawConstants) == 16, "TerrainDrawConstants must match the 16-byte push-constant range of DrawConstants");

}
#endif
