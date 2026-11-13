#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/QuadBatcher.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// The Particles pass (M26, C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decision 7, stream P-B): the snapshot's live
// particles (RenderSnapshot::ParticleEmitters and Particles, from render extraction) drawn as camera-facing QuadBatcher quads
// into SceneColor, depth-tested against SceneDepth without writing it, in the Sprites stage after the sprites, the tilemaps and
// the 2D lights (its stats row "Particles", ADR 0028's phase-3 ledger), so the HDR post chain (TAA, bloom, exposure) sees
// them. The SceneRenderer records it in a Lit view with a camera only: the data views, the material views, the Overdraw view
// and the Lit view's wireframe draw no particles, as they draw no sprites.
//
// Two halves:
//   - PlanParticlePass (CPU, pure): each particle's quad, a square of its Size centred on its Position in the plane facing the
//     camera (its edges along the camera's right and up axes, the rows of the view matrix, so an orthographic view sees
//     squares of the particle's size and a perspective view squares that shrink with distance), coloured by its Color, the
//     texture's top-left texel at the quad's top-left corner as the camera sees it. Emitters in the snapshot's order; an
//     Alpha emitter's quads back to front in the view (by view depth, the farthest first; ties in particle order), an
//     Additive emitter's in particle order. Particles wholly outside the clip volume are culled; those with a non-finite
//     position or colour, or a size that is not finite and positive, are skipped (data the schemas accept can overflow a
//     simulation: an unbounded Acceleration), neither of them an error.
//   - RecordParticlePass (GPU): resolves each emitter's texture through the GpuResourceCache (a null Texture is the built-in
//     particle texture, engine://Textures/Particle; a missing or failed one draws its placeholder whole over each quad, as a
//     sprite's), and records consecutive emitters of one blend as one QuadBatcher record: Alpha through the Hdr pipeline
//     (straight alpha), Additive through HdrAdditive (colour times alpha added, the target's alpha kept), both depth-tested
//     GreaterOrEqual without depth writes. Emitters draw in snapshot order, so the particles of overlapping emitters do not
//     interleave by depth (a kept limit, Backlog §6). The sampler is the caller's: the SceneRenderer samples Linear, and Point
//     in pixel-art mode (RenderOutputSettings::PixelArt), whose texels stay crisp at any quad size.
// The pass creates no GPU object of its own: the pipelines are the QuadBatcher's (SceneRendererPipelines', place 1 of ADR 0029
// decision 27), the textures the GpuResourceCache's mirrors (place 3), and its binding sets come from the view's
// PassBindingCache in the SceneRenderer's state (place 2), so the recreation protocol covers it through those owners
// ("Particles: a renderer drawing particles recovers from a device loss with the same image").
//
// Deterministic: a plan is a pure function of the snapshot's particles and the camera, so a capture of a deterministic play
// scene draws the same image. Particles write no velocity, so they ghost slightly under M26's TAA (a kept limit, Backlog §6).
// Main thread (the GpuResourceCache, the command list).

namespace Engine {

	struct RenderPassCounters;

	class GpuResourceCache;

	// One emitter's quads in a plan: ParticlePassPlan::Quads[First, First + Count), drawn with Texture and Blend.
	struct ParticlePassRun
	{
		AssetHandle Texture{}; // the emitter's texture; BuiltinAssetHandles::ParticleTexture for a null one
		RenderParticleBlend Blend = RenderParticleBlend::Alpha;
		uint32_t First = 0;
		uint32_t Count = 0;
		UUID Entity{};

		bool operator==(const ParticlePassRun&) const = default;
	};

	// One particle's quad, in world space.
	struct ParticlePassQuad
	{
		// Top-left, top-right, bottom-right, bottom-left as the camera sees them (QuadDraw's order).
		std::array<glm::vec3, 4> Corners{};
		glm::vec4 Color = glm::vec4(1.0f); // linear RGB, straight alpha; multiplies the texel

		bool operator==(const ParticlePassQuad&) const = default;
	};

	// The snapshot's particles ready to record (PlanParticlePass).
	struct ParticlePassPlan
	{
		std::vector<ParticlePassQuad> Quads{};
		std::vector<ParticlePassRun> Runs{}; // in the snapshot's emitter order; an emitter without a drawn quad has none
		uint32_t Culled = 0;                 // particles wholly outside the clip volume
		uint32_t Skipped = 0;                // particles with a non-finite position or colour, or a size not finite and positive

		bool operator==(const ParticlePassPlan&) const = default;
	};

	// Plans the particles of `emitters` (each ParticleEmitterItem's range of `particles`) seen through `view` (world to view)
	// and `viewProjection` (world to clip, reverse-Z), as the file comment states, into `plan` (cleared first, its vectors'
	// capacity kept). A view matrix whose right or up row is not finite and non-zero plans nothing. Errors (the plan still
	// holds every other emitter's quads): InvalidArgument naming the first emitter whose range does not lie inside `particles`
	// (data no extraction produces; that emitter is skipped). Pure, deterministic and thread-compatible.
	[[nodiscard]] Status PlanParticlePass(std::span<const ParticleEmitterItem> emitters, std::span<const ParticleItem> particles, const glm::mat4& view,
		const glm::mat4& viewProjection, ParticlePassPlan& plan);

	struct ParticlePassInputs
	{
		const ParticlePassPlan* Plan = nullptr; // never null
		QuadBatcher* Quads = nullptr;           // SceneRendererPipelines' batcher; never null
		GpuResourceCache* Resources = nullptr;  // resolves the runs' textures; never null
		// SceneColor with SceneDepth (QuadBatcher::GetFramebufferInfo of Hdr and HdrAdditive, the forward passes' framebuffer);
		// never null.
		nvrhi::IFramebuffer* Framebuffer = nullptr;
		glm::mat4 ViewProjection = glm::mat4(1.0f); // the quads' world space to clip space
		QuadSampler Sampler = QuadSampler::Linear;
	};

	// Records `inputs.Plan`'s runs under the caller's stats row ("Particles"), consecutive runs of one blend as one
	// QuadBatcher::Record (Hdr for Alpha, HdrAdditive for Additive) with binding sets from the view's `bindings`, adding the
	// draws and triangles to `counters`. `quads` is the caller's scratch (cleared first and last, its capacity kept): the
	// records' QuadDraws, which borrow the cache's textures for the call only. A run whose texture has no mirror at all (not
	// even its placeholder, which the cache reported) draws nothing. Returns the quads drawn. Asserts the inputs. Errors: the
	// first record's error (Gpu: a binding set or the vertex buffer could not be created; its quads are not drawn), the later
	// records still recorded.
	[[nodiscard]] Result<uint32_t> RecordParticlePass(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const ParticlePassInputs& inputs,
		std::vector<QuadDraw>& quads, RenderPassCounters& counters);

}
