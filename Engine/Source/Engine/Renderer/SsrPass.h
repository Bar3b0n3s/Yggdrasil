#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Shared/SsrConstants.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

// The SSR pass: glossy screen-space reflections with the probe fallback (M26, C26.1, R-SSR-IN, R-SSR;
// Docs/Decisions/0037-m26-contract.md decisions 13, 14 and 18; Architecture §8.3), the Ssr declaration's two pipelines of the
// program Ssr (Passes/Ssr.slang) in the Forward stage, after the skybox and before the transparent pass, recorded under the
// stats row SSR only with PostProcessSettings::SsrEnabled in a Lit view with a camera outside the wireframe. SSR off records
// nothing (not even the depth pyramid it would trace over without GTAO), so the image is byte-identical to the render without
// the pass.
//   1. CSTrace (compute): each pixel of the opaque image whose surface reflects (SceneRoughness word 0's multiplier not 0) and
//      whose roughness (word 0's a) is at most SsrMaxRoughness traces the reflection of its view ray in screen space, at most
//      SsrMaxDistance metres, over the depth pyramid (DepthPyramidPass: whole cells in front of the ray skipped a mip coarser,
//      the others looked into a mip finer, down to SceneDepth's pixels), and hits a surface it passes within SsrThickness
//      behind its depth that faces it; it writes SsrColor (SsrColorFormat): rgb SceneColor's radiance at the hit, a the
//      confidence, which fades with the distance travelled, the screen's edge and the roughness, and is 0 where the ray
//      missed or the pixel does not trace.
//   2. VSComposite and PSComposite (graphics): one fullscreen triangle added into SceneColor (One + One, rgb only) that
//      replaces, in proportion to the confidence times SsrIntensity, the forward pass's probe or environment specular by the
//      traced radiance: the composite evaluates the forward pass's term (the prefiltered radiance at the reflection of
//      SceneNormals' normal and SceneRoughness' roughness, Common/Probes.slang over the environment's, times word 0's
//      multiplier and SpecularOcclusion with min(word 1's material occlusion, GTAO)) and adds (traced - prefiltered) times
//      that factor, so where a ray missed (confidence 0) the probes' reflection stays (the probe fallback).
// Deterministic on a device: each pixel is written once by each step, from a bounded number of fixed steps, with no random
// or temporal term and no atomic.
//
// The pipelines are SceneRendererPipelines' (the placement rule's place 1, ADR 0029 decision 27), created by CreatePipelines;
// SsrColor and the composite's framebuffer (SceneColor alone) are each SceneRenderer's targets (place 2: SceneRendererTargets'
// SsrColor and SsrFramebuffer, from GetTargetDesc and the target pool), created by the first render with SSR and released
// with the targets by a resize or a device loss's release. The pass keeps no binding sets: each Record takes them from the
// view's PassBindingCache. Main thread only; not copyable or movable.

namespace Engine {

	class GraphicsDevice;

	// One record's resources and settings, borrowed for the call; every pointer is required.
	struct SsrPassInputs
	{
		// The view's per-view constants: ViewConstants (b0), EnvironmentConstants (b2) and ProbeConstants (b4).
		nvrhi::IBuffer* ViewConstants = nullptr;
		nvrhi::IBuffer* EnvironmentConstants = nullptr;
		nvrhi::IBuffer* ProbeConstants = nullptr;
		// The prepass's targets at the scene resolution: SceneDepth, SceneNormals and SceneRoughness (SceneTargetFormats.h).
		nvrhi::ITexture* SceneDepth = nullptr;
		nvrhi::ITexture* SceneNormals = nullptr;
		nvrhi::ITexture* SceneRoughness = nullptr;
		// The depth pyramid of this frame (DepthPyramidPass::GetTargetDesc at the scene resolution), every mip.
		nvrhi::ITexture* ViewDepth = nullptr;
		// The opaque image (SceneColor), which the trace reads, and the composite's framebuffer, SceneColor alone
		// (GetCompositeFramebufferInfo).
		nvrhi::ITexture* SceneColor = nullptr;
		nvrhi::IFramebuffer* Framebuffer = nullptr;
		// The traced reflections (GetTargetDesc at the scene resolution).
		nvrhi::ITexture* SsrColor = nullptr;
		// What the forward pass's specular term reads: GTAO's occlusion and its upsample depth (the neutral white AO and far depth
		// when GTAO did not run), the environment's prefiltered specular cube (the black cube without a map) and the view's
		// reflection probe cubes (the neutral array without probes).
		nvrhi::ITexture* AmbientOcclusion = nullptr;
		nvrhi::ITexture* LinearViewDepth = nullptr;
		nvrhi::ITexture* EnvironmentSpecular = nullptr;
		nvrhi::ITexture* ReflectionProbeCubes = nullptr;
		// PostProcessSettings' SsrIntensity, SsrMaxDistance, SsrThickness and SsrMaxRoughness (MakeSsrConstants sanitizes them).
		PostProcessSettings Post{};
	};

	// The push constants of one record of `post` over a `width` x `height` scene image whose depth pyramid has `pyramidLevels`
	// mips (Shared/SsrConstants.h): the image's size and its inverse, the pyramid's mip count (at least 1), and the settings made
	// usable: a non-finite SsrMaxDistance or SsrThickness takes its PostProcessSettings default and each is clamped to its
	// registered range (SsrMaxDistance [0.1, 1000] metres, SsrThickness [0.001, 10] metres), and a non-finite SsrIntensity or
	// SsrMaxRoughness is 0, each clamped to [0, 1]. Pure.
	[[nodiscard]] SsrConstants MakeSsrConstants(const PostProcessSettings& post, uint32_t width, uint32_t height, uint32_t pyramidLevels);

	class SsrPass
	{
	public:
		// CSTrace and the composite (VSComposite with PSComposite).
		static constexpr uint32_t PipelineCount = 2;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class SsrPass;
		};

		// Use Create.
		explicit SsrPass(ConstructionKey key);
		~SsrPass();

		SsrPass(const SsrPass&) = delete;
		SsrPass& operator=(const SsrPass&) = delete;

		// Creates the two pipelines (checked against their reflection) and the composite's sampler. `device` is a documented
		// back-reference that outlives the pass. Errors: those of PipelineFactory and the GraphicsDevice wrappers (a Gpu error is
		// an out-of-memory creation, FatalError(OutOfMemory) for a caller at startup, §8.14 item 7).
		[[nodiscard]] static Result<Scope<SsrPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		// The layout descriptions of the two pipelines, CSTrace's then the composite's, for "Shaders: LayoutsMatchReflection":
		// CSTrace's set 0 holds the push constants, b0 ViewConstants, t0 SceneDepth, t1 SceneNormals, t2 SceneRoughness, t3
		// ViewDepth, t4 SceneColor and u0 SsrColor; the composite's the push constants, b0 ViewConstants, b2
		// EnvironmentConstants, b4 ProbeConstants, t0 SceneDepth, t1 SceneNormals, t2 SceneRoughness, t5 SsrColor, t6 the
		// occlusion, t7 its depth, t8 the environment's specular cube, t14 the reflection probe cubes and s0 LinearClamp.
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();

		// SsrColor of a `width` x `height` scene (both >= 1, asserted): SsrColorFormat, shader-readable and a storage image,
		// kept in ShaderResource between command lists.
		[[nodiscard]] static nvrhi::TextureDesc GetTargetDesc(uint32_t width, uint32_t height);
		// The composite's framebuffer: SceneColor alone (SceneColorFormat).
		[[nodiscard]] static nvrhi::FramebufferInfo GetCompositeFramebufferInfo();

		// The pipelines Create made (PipelineCount).
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// Records the trace over the whole of inputs.SceneColor into inputs.SsrColor, then the composite into
		// inputs.Framebuffer, into `commandList` (open), with binding sets from the view's `bindings`, adding 1 to
		// counters.Dispatches, 1 to DrawCalls and 1 to Triangles. Errors, with nothing recorded: InvalidArgument for a missing
		// input, for SceneDepth, SceneNormals, SceneRoughness, ViewDepth or SsrColor of another size than SceneColor, or a
		// framebuffer that is not SceneColor's; those of creating a binding set for new resources (Gpu). Main thread.
		[[nodiscard]] Status Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SsrPassInputs& inputs, RenderPassCounters& counters);
	private:
		// The back-reference, the pipelines and the sampler (SsrPass.cpp).
		struct State;
		Scope<State> m_State;
	};

}
