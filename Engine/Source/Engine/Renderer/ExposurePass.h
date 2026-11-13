#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Shared/ExposureConstants.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

// The Exposure pass of histogram auto-exposure (M26, C26.1, R-AEX; Docs/Decisions/0037-m26-contract.md decisions 10, 13 and
// 15; Architecture §8.3, §8.9): two compute pipelines of the program Exposure (Passes/Exposure.slang) in the HdrPost stage,
// after Bloom, recorded under the stats row Exposure only with PostProcessSettings::ExposureMode Auto in a Lit view outside
// the wireframe and pixel-art mode (where Auto acts as Manual: the 2D colour path keeps exposure 1, decision 10).
//   1. CSHistogram: the histogram of the image Tonemap exposes (SceneColor with the bloom composite, before the exposure),
//      equal to Renderer/AutoExposure.h's ComputeLuminanceHistogram of that image bin for bin (integer atomic adds into the
//      histogram buffer, which Record clears first).
//   2. CSAdapt: from the histogram, the target and the adapted exposure (AutoExposure.h's ComputeExposureTarget and
//      AdaptExposure, from the previous frame's adapted exposure in the state buffer, or the target at once with Reset),
//      written into the state buffer as an ExposureState, whose Multiplier the Tonemap pass multiplies the view's exposure
//      by (TonemapPassInputs::AdaptedExposure, TonemapFlagAutoExposure). The ExposureEV of a view in Auto mode is the
//      compensation: 2^ExposureEV in ViewConstants::Exposure, as in Manual.
// Deterministic on a device: no float sum or order depends on an atomic, and a capture converges at once (its snapshot's
// history is reset), so two captures of a scene with auto-exposure are byte-identical (decision 10).
//
// The pass is created by SceneRendererPipelines::CreatePipelines (the placement rule's place 1, ADR 0029 decision 27); the
// histogram and state buffers are each SceneRenderer's (place 2: SceneRenderer::State's ExposureHistogramBuffer and
// ExposureStateBuffer, from GetHistogramDesc and GetStateDesc), created by its first Render with auto-exposure and dropped
// with its state by a device loss's release, so the first Exposure pass after a recovery starts afresh (a reset). The pass
// keeps no binding sets: each Record takes them from the view's PassBindingCache. Main thread only; not copyable or movable.

namespace Engine {

	class GraphicsDevice;

	// One record's resources and settings, borrowed for the call.
	struct ExposurePassInputs
	{
		nvrhi::ITexture* SceneColor = nullptr; // SceneColorFormat, shader-readable; never null
		// Optional: the texture BloomPass::Record returned (its mip 0 holds the bloom), composited as Tonemap does with
		// BloomIntensity; null meters SceneColor alone.
		nvrhi::ITexture* Bloom = nullptr;
		float BloomIntensity = 0.0f;         // in [0, 1], as TonemapPassInputs::BloomIntensity
		nvrhi::IBuffer* Histogram = nullptr; // from GetHistogramDesc; never null
		nvrhi::IBuffer* State = nullptr;     // from GetStateDesc; never null
		// The adapted exposure's range and speeds: PostProcessSettings' AutoExposureMinEV, AutoExposureMaxEV,
		// AutoExposureSpeedUp and AutoExposureSpeedDown (MakeExposureConstants sanitizes them).
		float MinEV = -6.0f;
		float MaxEV = 12.0f;
		float SpeedUp = 3.0f;
		float SpeedDown = 1.0f;
		float DeltaTime = 0.0f; // CameraData::DeltaTime: seconds since the view's previous frame
		// The adapted exposure is the target at once: the snapshot's history was reset (CameraData::PreviousViewProjection
		// empty), or the state buffer holds no adapted exposure of this view yet.
		bool Reset = true;
	};

	// The push constants of one record of `inputs` over a `width` x `height` SceneColor (Shared/ExposureConstants.h): the
	// image's size and its inverse, Flags with Reset and with Bloom (when inputs.Bloom is set and BloomIntensity, clamped to
	// [0, 1], is above 0), and the range and speeds made usable as AutoExposure.h reads them: a non-finite bound takes its
	// PostProcessSettings default (-6 or 12), and a negative or non-finite speed or delta time is 0. Pure.
	[[nodiscard]] ExposureConstants MakeExposureConstants(const ExposurePassInputs& inputs, uint32_t width, uint32_t height);

	class ExposurePass
	{
	public:
		// CSHistogram and CSAdapt.
		static constexpr uint32_t PipelineCount = 2;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class ExposurePass;
		};

		// Use Create.
		explicit ExposurePass(ConstructionKey key);
		~ExposurePass();

		ExposurePass(const ExposurePass&) = delete;
		ExposurePass& operator=(const ExposurePass&) = delete;

		// Creates the two pipelines (checked against their reflection) and the bloom's sampler. `device` is a documented
		// back-reference that outlives the pass. Errors: those of PipelineFactory and the GraphicsDevice wrappers (a Gpu error
		// is an out-of-memory creation, FatalError(OutOfMemory) for a caller at startup, §8.14 item 7).
		[[nodiscard]] static Result<Scope<ExposurePass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		// The layout descriptions of the two pipelines, CSHistogram's then CSAdapt's, for "Shaders: LayoutsMatchReflection":
		// CSHistogram's set 0 holds the push constants, t0 SceneColor, t1 Bloom, s0 and u0 the histogram; CSAdapt's the push
		// constants, u0 the histogram and u1 the state.
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();

		// The histogram: a structured buffer of ExposureHistogramBinCount uint counts, with UAVs, kept in UnorderedAccess
		// between command lists.
		[[nodiscard]] static nvrhi::BufferDesc GetHistogramDesc();
		// The adapted exposure: a structured buffer of one ExposureState, with UAVs, kept in ShaderResource between command
		// lists (the Tonemap pass reads it).
		[[nodiscard]] static nvrhi::BufferDesc GetStateDesc();

		// The pipelines Create made (PipelineCount).
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// Records the two dispatches over the whole of inputs.SceneColor into `commandList` (open): clears inputs.Histogram,
		// then CSHistogram and CSAdapt, with binding sets from the view's `bindings`, adding 2 to counters.Dispatches. Errors,
		// with nothing recorded: InvalidArgument for a missing SceneColor, Histogram or State, or a histogram or state buffer
		// smaller than its description's; those of creating a binding set for new resources (Gpu). Main thread.
		[[nodiscard]] Status Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const ExposurePassInputs& inputs,
			RenderPassCounters& counters);
	private:
		// The back-reference, the pipelines and the sampler (ExposurePass.cpp).
		struct State;
		Scope<State> m_State;
	};

}
