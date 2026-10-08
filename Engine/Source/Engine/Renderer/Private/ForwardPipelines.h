#pragma once

#include "Engine/Asset/MaterialData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

// The scene renderer's mesh pipelines (SceneRenderer.h; Architecture §8.4, §8.5; Docs/Decisions/0013-m8-decisions.md
// decision 7): the Scene program's prepass and forward variants, their layout descriptions and their shared binding
// layouts. Every variant draws MeshVertex (§6.8) with frontCounterClockwise = true (§8.3) and reverse-Z depth
// (GreaterOrEqual). A variant is an alpha mode and a cull mode:
//   - prepass (pass 4, SceneNormals with SceneDepth written): {Opaque, Mask} x {CullBack, CullFront, CullNone};
//   - forward opaque (pass 7, SceneColor, depth tested without writes): the same six;
//   - forward transparent (pass 9, SceneColor, depth tested without writes, straight alpha blending): Blend x the three.
// Mask variants use the ALPHA_MASK = 1 permutation. A mirroring draw takes CullFront (its front faces turned clockwise on
// screen) and a double-sided material CullNone. The forward variants exist once per debug view, specialized with the
// view's value (SceneDebugViewConstantId); a debug view shares its Lit variant's layout description.
//
// Binding layouts (§8.4), shared through PipelineFactory::CreateBindingLayouts so one binding set serves every variant:
//   - prepass set 0: b0 ViewConstants, s2 AnisoWrap and the DrawConstants push constants;
//   - forward set 0: b0 ViewConstants, b2 EnvironmentConstants, t0 Lights, t3 EnvSpecular, t5 BRDFLut, s0 LinearClamp,
//     s2 AnisoWrap and the push constants;
//   - set 1 (every variant): b0 MaterialConstants and t0..t4 the base colour, metallic-roughness, normal, occlusion and
//     emissive maps.

namespace Engine {

	namespace Utils {

		// NVRHI's validation counts push constants as a constant-buffer slot (DX12 root constants), so they may not share one
		// with a constant buffer of the layout; Vulkan ignores the slot. b0 to b2 of set 0 are the per-view constants of
		// §8.4, so the draw's push constants take b3 (ADR 0012 decision 22).
		inline constexpr uint32_t DrawConstantsSlot = 3;

		// The registers of set 0 (§8.4) and set 1.
		inline constexpr uint32_t ViewConstantsRegister = 0;
		inline constexpr uint32_t EnvironmentConstantsRegister = 2;
		inline constexpr uint32_t LightsRegister = 0;
		inline constexpr uint32_t EnvSpecularRegister = 3;
		inline constexpr uint32_t BrdfLutRegister = 5;
		inline constexpr uint32_t LinearClampRegister = 0;
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

		// The cull mode of a draw: CullNone for a double-sided material, else CullFront for a mirroring world, else CullBack.
		[[nodiscard]] MeshCullMode SelectCullMode(bool mirrored, bool doubleSided);

		// The prepass variant of an Opaque or Mask draw (asserted).
		[[nodiscard]] uint32_t GetPrepassVariant(AlphaMode alphaMode, MeshCullMode cullMode);

		// The forward variant of a draw: Opaque and Mask in [0, 6), Blend in [6, 9).
		[[nodiscard]] uint32_t GetForwardVariant(AlphaMode alphaMode, MeshCullMode cullMode);

		// The layout descriptions of the 15 startup mesh pipelines: the 6 prepass variants, then the 9 forward variants.
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
		[[nodiscard]] Result<GraphicsPipeline> CreatePrepassPipeline(PipelineFactory& pipelines, const MeshBindingLayouts& layouts, uint32_t variant);

		// Creates forward variant `variant` specialized with `view` (Lit: the shaders as compiled). Errors: those of
		// PipelineFactory.
		[[nodiscard]] Result<GraphicsPipeline> CreateForwardPipeline(PipelineFactory& pipelines, const MeshBindingLayouts& layouts, uint32_t variant,
			RenderDebugView view);

	}

}
