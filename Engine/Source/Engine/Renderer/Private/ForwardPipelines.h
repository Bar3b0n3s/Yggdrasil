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
//   - prepass (pass 4, SceneNormals with SceneDepth written): {Opaque, Mask} x {CullBack, CullFront, CullNone};
//   - forward opaque (pass 7, SceneColor, depth tested without writes): the same six;
//   - forward transparent (pass 9, SceneColor, depth tested without writes, straight alpha blending): Blend x the three.
// Mask variants use the ALPHA_MASK = 1 permutation. A mirroring draw takes CullFront (its front faces turned clockwise on
// screen) and a double-sided material CullNone. The forward variants exist once per debug view, specialized with the
// view's value (SceneDebugViewConstantId); a debug view shares its Lit variant's layout description.
//
// Binding layouts (§8.4), shared through PipelineFactory::CreateBindingLayouts so one binding set serves every variant:
//   - prepass set 0: b0 ViewConstants, t10 Instances, s2 AnisoWrap and the DrawConstants push constants;
//   - forward set 0: b0 ViewConstants, b1 ShadowConstants, b2 EnvironmentConstants, b3 LightClusterConstants, t0 Lights,
//     t1 ShadowCascades, t2 ShadowAtlas, t3 EnvSpecular, t5 BRDFLut, t6 AmbientOcclusion, t8 ViewDepth, t9 SceneNormals,
//     t10 Instances, t11 the light cluster masks (M23, Renderer/LightClusterPass.h), t12 PointShadowMaps, s0 LinearClamp,
//     s1 ShadowCompare, s2 AnisoWrap and the push constants;
//   - set 1 (every variant): b0 MaterialConstants and t0..t4 the base colour, metallic-roughness, normal, occlusion and
//     emissive maps.

namespace Engine {

	namespace Utils {

		// NVRHI's validation counts push constants as a constant-buffer slot (DX12 root constants), so they may not share one
		// with a constant buffer of the layout; Vulkan ignores the slot. b0 to b3 of set 0 are the per-view constants of
		// §8.4, so the draw's push constants take the first slot after them: b3 until M23 (ADR 0012 decision 22), b4 since
		// M23's LightClusterConstants took b3 (Docs/Decisions/0034-m23-contract.md decision 5).
		inline constexpr uint32_t DrawConstantsSlot = 4;

		// The registers of set 0 (§8.4) and set 1.
		inline constexpr uint32_t ViewConstantsRegister = 0;
		inline constexpr uint32_t ShadowConstantsRegister = 1;
		inline constexpr uint32_t EnvironmentConstantsRegister = 2;
		inline constexpr uint32_t LightClusterConstantsRegister = 3; // M23 (stream C): the cluster grid
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

		// The cull mode of a draw: CullNone for a double-sided material, else CullFront for a mirroring world, else CullBack.
		[[nodiscard]] MeshCullMode SelectCullMode(bool mirrored, bool doubleSided);

		// The prepass variant of an Opaque or Mask draw (asserted).
		[[nodiscard]] uint32_t GetPrepassVariant(AlphaMode alphaMode, MeshCullMode cullMode);

		// The forward variant of a draw: Opaque and Mask in [0, 6), Blend in [6, 9).
		[[nodiscard]] uint32_t GetForwardVariant(AlphaMode alphaMode, MeshCullMode cullMode);

		// The layout descriptions of the 24 startup mesh pipelines (SceneRendererPipelines::MeshPipelineCount): the 6 prepass
		// variants, the 9 forward variants, the 6 picking prepass variants, then the 3 overdraw variants.
		[[nodiscard]] std::vector<PipelineLayoutDescription> GetMeshLayoutDescriptions();

		// The binding layouts every mesh pipeline shares (see the file comment).
		struct MeshBindingLayouts
		{
			nvrhi::BindingLayoutHandle PrepassView{};
			nvrhi::BindingLayoutHandle ForwardView{};
			nvrhi::BindingLayoutHandle Material{};
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
