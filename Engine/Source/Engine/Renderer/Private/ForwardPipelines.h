#pragma once

#include "Engine/Asset/MaterialData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <vector>

// The scene renderer's mesh pipelines (SceneRenderer.h; Architecture §8.4, §8.5; Docs/Decisions/0013-m8-decisions.md
// decision 7): the Scene program's prepass and forward variants, their layout descriptions and their shared binding
// layouts. Every variant draws MeshVertex (§6.8; the overdraw variants only its position and texture coordinates) from
// vertex binding 0 and MeshVertexAttributes (M23, MeshData version 2) from binding 1 (MakeMeshAttributeStreamAttributes),
// with frontCounterClockwise = true (§8.3) and reverse-Z depth (GreaterOrEqual). A variant is an alpha mode and a cull mode:
//   - prepass (pass 4, SceneNormals, SceneVelocity and SceneRoughness with SceneDepth written; the picking prepass
//     SceneNormals, EntityId, SceneVelocity and SceneRoughness, SceneTargetFormats.h's order since M26):
//     {Opaque, Mask} x {CullBack, CullFront, CullNone};
//   - forward opaque (pass 7, SceneColor, depth tested without writes): the same six;
//   - forward transparent (pass 9, SceneColor, depth tested without writes, straight alpha blending): Blend x the three.
// Mask variants use the ALPHA_MASK = 1 permutation. A mirroring draw takes CullFront (its front faces turned clockwise on
// screen) and a double-sided material CullNone. The forward variants exist once per debug view, specialized with the
// view's value (SceneDebugViewConstantId); a debug view shares its Lit variant's layout description.
//
// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3G): every variant also binds vertex binding 3
// (MeshWindStreamSlot), WIND (R8G8B8A8_UNORM, the mesh's MeshVertexWind stream or the cache's shared zero stream) after the
// attribute stream's locations (6 in VSMain and VSPrepass; the overdraw variants bind NORMAL and WIND at 4 and 5).
// M26 (C26.1, R-TAA-VEL; Docs/Decisions/0037-m26-contract.md decisions 11 and 14; stream X-C): the 12 prepass and picking
// prepass variants also bind vertex binding 2 (PreviousPositionSlot): PREVIOUSPOSITION (RGB32_FLOAT at offset 0, stride 16,
// location 7 since M46's WIND took 6), a deformed item's previous positions (SkinningPass::GetPreviousPositionsDesc's float4 elements), which the
// motion vectors read for an instance with DrawFlagPreviousPositions; any other draw binds its own vertex buffer there,
// which the shaders never read as positions (the flag is clear) and whose every fetch stays inside it (a MeshVertex is 48
// bytes, the binding's stride 16).
//
// Binding layouts (§8.4), shared through PipelineFactory::CreateBindingLayouts so one binding set serves every variant:
//   - prepass set 0: b0 ViewConstants, t5 BRDFLut, t10 Instances, t16 the dissolve noise (M31), b8 WindConstants (M46), s0
//     LinearClamp, s2 AnisoWrap and the DrawConstants push constants (M26: the DFG LUT and its sampler for SceneRoughness's
//     specular inputs);
//   - overdraw set 0 (the Overdraw data view's pipelines, M26: the prepass's before M26): b0 ViewConstants, t10 Instances,
//     b8 WindConstants (M46), s2 AnisoWrap and the push constants;
//   - forward set 0: b0 ViewConstants, b1 ShadowConstants, b2 EnvironmentConstants, b3 LightClusterConstants, b4
//     ProbeConstants (M26), b5 FogConstants (M32, Docs/Decisions/0047-m32-fog-ssr.md decision 5), t0 Lights, t1 ShadowCascades, t2 ShadowAtlas, t3 EnvSpecular, t5 BRDFLut, t6 AmbientOcclusion,
//     t8 ViewDepth, t9 SceneNormals, t10 Instances, t11 the light cluster masks (M23, Renderer/LightClusterPass.h), t12
//     PointShadowMaps, t13 the irradiance volumes' coefficients and t14 the reflection probes' cubes (M26), t15
//     PointShadowMapsHalf (M29), t16 the dissolve noise (M31), b7 PlanarReflectionConstants and t17 to t20 the planar
//     reflections' images (M42, Docs/Decisions/0059-m42-planar-reflections.md decision 3), b8 WindConstants (M46), s0
//     LinearClamp, s1 ShadowCompare, s2 AnisoWrap and the push constants;
//   - set 1 (every variant): b0 MaterialConstants and t0..t4 the base colour, metallic-roughness, normal, occlusion and
//     emissive maps.

namespace Engine {

	namespace Utils {

		// NVRHI's validation counts push constants as a constant-buffer slot (DX12 root constants), so they may not share one
		// with a constant buffer of the layout; Vulkan ignores the slot. b0 to b4 of set 0 are the per-view constants of
		// §8.4, so the draw's push constants take the first slot after them: b3 until M23 (ADR 0012 decision 22), b4 since
		// M23's LightClusterConstants took b3 (Docs/Decisions/0034-m23-contract.md decision 5), b5 since M26's ProbeConstants
		// took b4 (Docs/Decisions/0037-m26-contract.md decision 16), b6 since M32's FogConstants took b5
		// (Docs/Decisions/0047-m32-fog-ssr.md decision 5).
		inline constexpr uint32_t DrawConstantsSlot = 6;

		// The registers of set 0 (§8.4) and set 1.
		inline constexpr uint32_t ViewConstantsRegister = 0;
		inline constexpr uint32_t ShadowConstantsRegister = 1;
		inline constexpr uint32_t EnvironmentConstantsRegister = 2;
		inline constexpr uint32_t LightClusterConstantsRegister = 3; // M23 (stream C): the cluster grid
		inline constexpr uint32_t ProbeConstantsRegister = 4;        // M26 (stream Q-B): the view's probes
		inline constexpr uint32_t FogConstantsRegister = 5;          // M32: the view's fog (Renderer/Fog.h)
		inline constexpr uint32_t LightsRegister = 0;
		inline constexpr uint32_t ShadowCascadesRegister = 1;
		inline constexpr uint32_t ShadowAtlasRegister = 2;
		inline constexpr uint32_t EnvSpecularRegister = 3;
		inline constexpr uint32_t BrdfLutRegister = 5;
		inline constexpr uint32_t AmbientOcclusionRegister = 6;
		inline constexpr uint32_t ViewDepthRegister = 8;
		inline constexpr uint32_t SceneNormalsRegister = 9;
		// M23 (C23.1, R-INST; Docs/Decisions/0034-m23-contract.md decision 2): the view's Instances structured buffer of
		// Shared/InstanceRecord.h, read by every program that draws meshes (Scene, Shadow and SelectionMask) at this register.
		inline constexpr uint32_t InstancesRegister = 10;
		inline constexpr uint32_t LightClusterMasksRegister = 11; // M23 (stream C): the cluster masks
		// M23 (R-PLS, Docs/Decisions/0034-m23-contract.md decision 3): the PointShadowMaps cube array, sampled with
		// ShadowCompareRegister's comparison sampler.
		inline constexpr uint32_t PointShadowMapsRegister = 12;
		// M26 (C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 16, stream Q-B): the view's irradiance volumes'
		// SH9 coefficients (a structured buffer of float4) and its reflection probes' prefiltered specular cubes (a cube
		// array), which Common/Probes.slang reads with ProbeConstantsRegister's constants.
		inline constexpr uint32_t IrradianceProbesRegister = 13;
		inline constexpr uint32_t ReflectionProbeCubesRegister = 14;
		// M29 (Docs/Decisions/0044-m29-light-limits.md decision 2): the PointShadowMapsHalf cube array, the Half tier's point
		// lights' faces, sampled like PointShadowMapsRegister's.
		inline constexpr uint32_t PointShadowMapsHalfRegister = 15;
		// M31 (Docs/Decisions/0046-m31-material-parameters.md decision 6): the dissolve noise (BuiltinAssetHandles::
		// DissolveNoiseTexture), sampled with AnisoWrapRegister's sampler by the prepass and forward variants' fragment shaders
		// for an instance that dissolves, and by the Shadow program's Mask casters (Renderer/ShadowPass.h) at the same
		// register. The Terrain program declares it unread, since it shares the forward set (ADR 0042's Terrain.slang row).
		// t16 since batch 1's landing, after M29's t15 (ADR 0042's ForwardPipelines row: the later lander renumbers its own).
		inline constexpr uint32_t DissolveNoiseRegister = 16;
		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): the view's PlanarReflectionConstants (b7, the first
		// constant-buffer slot after the push constants' b6) and its planar reflections' images, slot k's at
		// PlanarReflectionsRegister + k for k below PlanarReflectionSlotCount (MaxPlanarReflectionsPerView,
		// Renderer/PlanarReflections.h; checked in SceneRenderer.cpp): the forward set's, which the Terrain program declares
		// unread since it shares the set. A mirror view, and a slot without a drawn reflection, binds the pipeline set's
		// neutral 1 x 1 black texture there.
		inline constexpr uint32_t PlanarReflectionConstantsRegister = 7;
		inline constexpr uint32_t PlanarReflectionsRegister = 17;
		inline constexpr uint32_t PlanarReflectionSlotCount = 4;
		// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3): the static set's GPU-resident instance records
		// (GpuResourceCache::GetStaticRecordBuffer) and the view's InstanceIndices (Shared/InstanceRecord.h), which a static
		// draw's instances read (DrawConstantsStaticInstances): in the prepass, overdraw and forward sets and the Shadow
		// program's, after their earlier items; the Terrain program declares them unread, since it shares the forward set.
		inline constexpr uint32_t StaticInstancesRegister = 21;
		inline constexpr uint32_t InstanceIndicesRegister = 22;
		// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3G): the renderer's WindConstants
		// (Shared/WindConstants.h) at b8 of set 0 in the prepass, overdraw and forward layouts, as in the Shadow and
		// SelectionMask programs' (Renderer/ShadowPass.h, Renderer/SelectionPass.h), so Common/Wind.slang declares one register.
		// The Terrain program declares it unread, since it shares the forward set.
		inline constexpr uint32_t WindConstantsRegister = 8;
		inline constexpr uint32_t LinearClampRegister = 0;
		inline constexpr uint32_t ShadowCompareRegister = 1;
		inline constexpr uint32_t AnisoWrapRegister = 2;
		inline constexpr uint32_t MaterialConstantsRegister = 0;
		inline constexpr uint32_t MaterialMapCount = 5; // t0..t4 of set 1

		enum class MeshCullMode : uint8_t
		{
			Back,  // single-sided, ordinary world matrix
			Front, // single-sided, mirroring world matrix
			None   // double-sided
		};

		inline constexpr uint32_t MeshCullModeCount = 3;
		// {Opaque, Mask} x the cull modes.
		inline constexpr uint32_t PrepassVariantCount = 2 * MeshCullModeCount;
		// {Opaque, Mask} x the cull modes, then Blend x the cull modes.
		inline constexpr uint32_t ForwardVariantCount = 3 * MeshCullModeCount;

		// M23 (C23.1, C-MESH2; Docs/Decisions/0034-m23-contract.md decision 9): vertex binding 1 of every mesh-drawing
		// pipeline (the Scene program's 24, the debug views', the shadow casters and the selection mask), the mesh's attribute
		// stream (GpuMesh::AttributeBuffer, 24-byte MeshVertexAttributes): TEXCOORD1 (RG32_FLOAT, offset 0) and COLOR
		// (RGBA32_FLOAT, offset 8), at the two locations after a pipeline's binding 0 attributes (4 and 5 for the Scene
		// program's VSMain and VSPicking), since NVRHI numbers locations in attribute order. Every program that draws meshes
		// reads them: a Mask material's alpha test samples the base colour's UV set and multiplies the vertex colour's alpha.
		inline constexpr uint32_t MeshAttributeStreamSlot = 1;
		[[nodiscard]] std::array<nvrhi::VertexAttributeDesc, 2> MakeMeshAttributeStreamAttributes();
		// M26 (stream X-C): vertex binding 2 of the prepass and picking prepass variants (see the file comment).
		inline constexpr uint32_t PreviousPositionSlot = 2;
		inline constexpr uint32_t PreviousPositionStride = 16;
		// M46 (decision 3G): vertex binding 3 of every mesh-drawing pipeline (the Scene program's 24, the debug views', the
		// six shadow casters and the six selection masks), the mesh's wind stream (GpuMesh::WindBuffer, 4-byte
		// MeshVertexWind): WIND (R8G8B8A8_UNORM, offset 0), after every other attribute of binding 0 and 1 a pipeline reads.
		inline constexpr uint32_t MeshWindStreamSlot = 3;
		[[nodiscard]] nvrhi::VertexAttributeDesc MakeMeshWindStreamAttribute();
		// M46 (decision 3G): MeshVertex's NORMAL (RGB32_FLOAT) from binding 0, which the wind's displacement reads in the
		// pipelines that bind no other normal (the overdraw variants, the shadow casters and the selection masks).
		[[nodiscard]] nvrhi::VertexAttributeDesc MakeMeshNormalAttribute();

		// The cull mode of a draw: CullNone for a double-sided material, else CullFront for a mirroring world, else CullBack.
		[[nodiscard]] MeshCullMode SelectCullMode(bool mirrored, bool doubleSided);

		// The prepass variant of an Opaque or Mask draw (asserted).
		[[nodiscard]] uint32_t GetPrepassVariant(AlphaMode alphaMode, MeshCullMode cullMode);

		// The forward variant of a draw: Opaque and Mask in [0, 6), Blend in [6, 9).
		[[nodiscard]] uint32_t GetForwardVariant(AlphaMode alphaMode, MeshCullMode cullMode);

		// The layout descriptions of the 24 startup mesh pipelines (SceneRendererPipelines::MeshPipelineCount): the 6 prepass
		// variants, the 9 forward variants, the 6 picking prepass variants, then the 3 overdraw variants.
		[[nodiscard]] std::vector<PipelineLayoutDescription> GetMeshLayoutDescriptions();

		// M26 (C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 22, stream T-C): the forward variants' set-0 layout
		// (see the file comment), which the terrain's forward pipeline shares (Renderer/TerrainPass.h), so one forward set of
		// a view serves the meshes and the terrains.
		[[nodiscard]] nvrhi::BindingLayoutDesc GetForwardViewLayout();

		// The binding layouts every mesh pipeline shares (see the file comment).
		struct MeshBindingLayouts
		{
			nvrhi::BindingLayoutHandle PrepassView{};
			nvrhi::BindingLayoutHandle ForwardView{};
			nvrhi::BindingLayoutHandle Material{};
			nvrhi::BindingLayoutHandle OverdrawView{}; // M26 (stream X-C): the overdraw variants' set 0
		};

		// Creates the shared binding layouts after checking the descriptions against the reflection. Errors: those of
		// PipelineFactory::CreateBindingLayouts.
		[[nodiscard]] Result<MeshBindingLayouts> CreateMeshBindingLayouts(PipelineFactory& pipelines);

		// Creates prepass variant `variant` with the shared layouts. Errors: those of PipelineFactory.
		[[nodiscard]] Result<GraphicsPipeline> CreatePrepassPipeline(PipelineFactory& pipelines, const MeshBindingLayouts& layouts, uint32_t variant, bool picking = false);
		[[nodiscard]] Result<GraphicsPipeline> CreateOverdrawPipeline(PipelineFactory& pipelines, const MeshBindingLayouts& layouts, uint32_t cullMode);
		// The data views' composite (Passes/SceneDataView.slang): set 0 with b0 ViewConstants, b1 ShadowConstants, b2
		// LightClusterConstants (M23), t0 SceneDepth, t1 the occlusion, t2 the linear depth, t3 SceneNormals, t4 the overdraw
		// count, t5 the light cluster masks (M23), u0 LdrColor and its push constants (b3).
		inline constexpr uint32_t SceneDataViewClustersRegister = 2;
		inline constexpr uint32_t SceneDataViewMasksRegister = 5;
		[[nodiscard]] PipelineLayoutDescription GetSceneDataViewLayout();

		// Creates forward variant `variant` specialized with `view` (Lit: the shaders as compiled). Errors: those of
		// PipelineFactory.
		[[nodiscard]] Result<GraphicsPipeline> CreateForwardPipeline(PipelineFactory& pipelines, const MeshBindingLayouts& layouts, uint32_t variant,
			RenderDebugView view);

	}

}
