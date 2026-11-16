#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderStats.h"
#include "Shared/TaaConstants.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

// The Taa pass of temporal anti-aliasing (M26, C26.1, R-TAA; Docs/Decisions/0037-m26-contract.md decisions 10, 13, 17 and
// 29; Architecture §8.3): one compute pipeline of the program Taa (Passes/Taa.slang), first in the HdrPost stage, before
// Bloom, recorded under the stats row TAA only with PostProcessSettings::AntiAliasing Taa in a Lit view with a camera,
// outside the wireframe and pixel-art mode (where Taa acts as None, as FXAA does: the 2D colour path). The view's scene
// passes rasterize with the snapshot's jitter (CameraData::Jitter, which the view's RenderViewHistory sets from the Halton
// (2, 3) sequence: Renderer/TemporalHistory.h); the pass resolves that jittered HDR SceneColor against the view's previous
// resolved image (the declaration's TaaHistory), reprojected by the prepass's motion vectors (SceneVelocity), clamped to the
// current neighbourhood in YCoCg and blended with the fixed TaaFeedback, with no random term, into the other history image,
// which it then copies into SceneColor, so bloom, exposure and the tonemap read the resolved image and the next render's
// pass reads it as its history. A reset (the snapshot's history reset, the view's first pass, history images created
// again) resolves to the current image exactly. Particles write their motion in the Particles pass (M33,
// Docs/Decisions/0048-m33-particle-look.md decision 6) where their alpha is at least half; fainter ones ghost slightly (a kept
// limit, Backlog §6).
//
// The pass is created by SceneRendererPipelines::CreatePipelines (the placement rule's place 1, ADR 0029 decision 27); the
// two history images are each SceneRenderer's (place 2: SceneRendererTargets::TaaHistory, from GetHistoryDesc), created by
// its first render with TAA at a size and dropped with its targets (a resize, ReleaseTargets, a device loss's release), so
// the first pass after a recovery starts afresh (a reset), as decision 29 accepts. The pass keeps no binding sets: each
// Record takes them from the view's PassBindingCache. Main thread only; not copyable or movable.

namespace Engine {

	class GraphicsDevice;

	// The clamped history's weight in each resolved pixel: a fixed exponential feedback, so a still image converges to the
	// average of its jittered renders (TemporalHistory.h's sequence) in about 1 / (1 - TaaFeedback) renders.
	inline constexpr float TaaFeedback = 0.9f;

	// One record's resources, borrowed for the call.
	struct TaaPassInputs
	{
		// The jittered HDR image (SceneColorFormat, shader-readable and a copy destination): the pass reads it and copies the
		// resolved image back into it. Never null.
		nvrhi::ITexture* SceneColor = nullptr;
		nvrhi::ITexture* Velocity = nullptr; // SceneVelocityFormat, SceneColor's size; never null
		// The view's previous resolved image (GetHistoryDesc at SceneColor's size), read unless Reset; null with Reset.
		nvrhi::ITexture* History = nullptr;
		// The image the resolve writes (GetHistoryDesc at SceneColor's size), the next render's History; never null, never
		// History.
		nvrhi::ITexture* Resolved = nullptr;
		// The resolved image is the current one: the snapshot's history was reset (CameraData::PreviousViewProjection empty)
		// or the view has no previous resolved image.
		bool Reset = true;
	};

	// The push constants of one record of `inputs` over a `width` x `height` SceneColor (Shared/TaaConstants.h): the image's
	// size and its inverse, TaaFeedback, and TaaFlagReset with inputs.Reset. Pure.
	[[nodiscard]] TaaConstants MakeTaaConstants(const TaaPassInputs& inputs, uint32_t width, uint32_t height);

	class TaaPass
	{
	public:
		// CSMain.
		static constexpr uint32_t PipelineCount = 1;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class TaaPass;
		};

		// Use Create.
		explicit TaaPass(ConstructionKey key);
		~TaaPass();

		TaaPass(const TaaPass&) = delete;
		TaaPass& operator=(const TaaPass&) = delete;

		// Creates the pipeline (checked against its reflection) and the history's sampler. `device` is a documented
		// back-reference that outlives the pass. Errors: those of PipelineFactory and the GraphicsDevice wrappers (a Gpu error
		// is an out-of-memory creation, FatalError(OutOfMemory) for a caller at startup, §8.14 item 7).
		[[nodiscard]] static Result<Scope<TaaPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		// The layout description of the pipeline, for "Shaders: LayoutsMatchReflection": set 0 holds the push constants, t0
		// SceneColor, t1 SceneVelocity, t2 the history, s0 its linear clamp sampler and u0 the resolved image
		// (TaaHistoryFormat).
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();

		// One TaaHistory image of `width` x `height`: TaaHistoryFormat, shader-readable, a storage image and a copy source,
		// kept in ShaderResource between command lists.
		[[nodiscard]] static nvrhi::TextureDesc GetHistoryDesc(uint32_t width, uint32_t height);

		// The pipelines Create made (PipelineCount).
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// Records the resolve over the whole of inputs.SceneColor into `commandList` (open), with its binding set from the
		// view's `bindings`, then the copy of inputs.Resolved into inputs.SceneColor, adding 1 to counters.Dispatches. Errors,
		// with nothing recorded: InvalidArgument for a missing SceneColor, Velocity or Resolved, a missing History without
		// Reset, History equal to Resolved, or a Velocity, History or Resolved whose size differs from SceneColor's; those of
		// creating a binding set for new resources (Gpu). Main thread.
		[[nodiscard]] Status Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const TaaPassInputs& inputs, RenderPassCounters& counters);
	private:
		// The back-reference, the pipeline and the sampler (TaaPass.cpp).
		struct State;
		Scope<State> m_State;
	};

}
