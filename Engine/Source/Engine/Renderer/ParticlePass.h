#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/QuadBatcher.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The Particles pass (M26, C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decision 7, stream P-B; M33's look and its own
// program, Docs/Decisions/0048-m33-particle-look.md decisions 4 to 6): the snapshot's live particles (RenderSnapshot's
// ParticleEmitters and Particles, from render extraction) drawn as textured quads into SceneColor after the sprites, the
// tilemaps and the 2D lights (its stats row "Particles", ADR 0028's phase-3 ledger), so the HDR post chain (TAA, bloom,
// exposure) sees them, and, in a view with TAA, their motion into SceneVelocity. The SceneRenderer records it in a Lit view
// with a camera only: the data views, the material views, the Overdraw view and the Lit view's wireframe draw no particles,
// as they draw no sprites.
//
// Two halves:
//   - PlanParticlePass (CPU, pure): each particle's quad by its emitter's alignment (ParticleEmitterItem::Alignment): Camera
//     a square of its Size centred on its Position in the plane facing the camera (its edges along the camera's right and
//     up axes, the rows of the view matrix, turned counter-clockwise by its Rotation; an angle of exactly 0 takes the axes
//     as they are, as M26 did), Velocity a quad whose up edge runs along the particle's velocity projected on that plane,
//     Size wide and Size plus that projected speed times the emitter's VelocityStretch long (the camera-facing square when
//     the projected speed is not above 1e-6 m/s or not finite, unturned: a Velocity particle's angle is unused), and Flat a
//     square in the emitter's plane (PlaneRight, PlaneUp) turned by its Rotation (the camera-facing square when an axis is
//     zero). Coloured by its Color, the texture's
//     top-left texel at the quad's top-left corner as the camera (or, for Flat, a viewer above the plane) sees it. With a
//     flipbook (the emitter's Sheet) its texture rectangle is the frame of its phase (decision 5: frame floor(FramePhase n)
//     mod n of the range's n frames, and with BlendFrames the next one, blended by the phase's fraction until the last frame
//     of the range's last play of FrameCycles, which is held), else the whole texture. Its previous corners are laid the same
//     way from its Previous draw (its Corners without one) and its inverse soft distance is 1 / SoftDistance (0 for a hard
//     cut). Emitters in the snapshot's order; an Alpha emitter's quads back to front in the
//     view (by view depth, the farthest first; ties in particle order), an Additive emitter's in particle order. Particles
//     wholly outside the clip volume are culled; those with a non-finite position, colour or angle, or a size that is not
//     finite and positive, are skipped (data the schemas accept can overflow a simulation: an unbounded Acceleration),
//     neither of them an error; an emitter with a Sheet whose flipbook did not resolve draws nothing (decision 5).
//   - ParticlePass::Record (GPU): resolves each run's texture through the GpuResourceCache (an emitter without a sheet: its
//     Texture, null being the built-in particle texture, engine://Textures/Particle; with one: the flipbook's; a missing or
//     failed texture draws its placeholder's mirror, as a sprite's), lays out every quad into ParticleVertex
//     (Shared/ParticleVertex.h) and records, per chunk of at most MaxQuadsPerBatch quads, the colour draws (consecutive quads
//     of one texture and blend in one draw: Alpha with the ParticlesAlpha pipeline, straight alpha; Additive with
//     ParticlesAdditive, colour times alpha added, the target's alpha kept) into SceneColor and, with a velocity framebuffer,
//     the same quads again with ParticlesVelocity into SceneVelocity. SceneDepth is bound as a texture, not attached: the
//     fragments test it themselves (Passes/Particles.slang). Emitters draw in snapshot order, so the particles of
//     overlapping emitters do not interleave by depth (a kept limit, Backlog §6). The sampler is the caller's: the
//     SceneRenderer samples Linear, and Point in pixel-art mode (RenderOutputSettings::PixelArt), whose texels stay crisp at
//     any quad size.
// A particle without a look (or with a default one) gives the corners, texture coordinates and colour M26's quads had and is
// tested and blended as they were, so it draws the same image (decision 4).
//
// ParticlePass is created once per device at startup and owned by SceneRendererPipelines (place 1 of ADR 0029 decision 27):
// its three pipelines (the declaration "Particles" in RenderPassDeclaration.h), its two samplers and its vertex and index
// buffers, so the recreation protocol covers it with the pipeline set; it keeps no binding sets (each Record takes them from
// the view's PassBindingCache, place 2) and its textures are the GpuResourceCache's mirrors (place 3) ("Particles: a renderer
// drawing particles recovers from a device loss with the same image").
//
// Deterministic: a plan is a pure function of the snapshot's particles, the flipbooks and the camera, so a capture of a
// deterministic play scene draws the same image. Main thread (the GpuResourceCache, the command list). Not copyable or
// movable.

namespace Engine {

	struct RenderPassCounters;

	class GpuResourceCache;
	class GraphicsDevice;

	// One emitter's flipbook as the renderer resolved it (decision 5): the texture its frames are cut from (the sheet's, or
	// BuiltinAssetHandles::WhiteTexture for a sheet without one) and each frame of the range's texture rectangle, in the
	// range's order, as (u min, v min, u max, v max). No frames: the sheet is missing, failed to load, or its range is empty
	// or ends past its frames, and the emitter draws nothing.
	struct ParticleFlipbook
	{
		AssetHandle Texture{};
		std::vector<glm::vec4> Frames{};

		bool operator==(const ParticleFlipbook&) const = default;
	};

	// One emitter's quads in a plan: ParticlePassPlan::Quads[First, First + Count), drawn with Texture and Blend.
	struct ParticlePassRun
	{
		AssetHandle Texture{}; // the emitter's texture (BuiltinAssetHandles::ParticleTexture for a null one) or its flipbook's
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
		glm::vec4 Color = glm::vec4(1.0f); // linear RGB (above 1 with a look's Intensity), straight alpha; multiplies the texel
		// M33 (decisions 4 to 6), appended: the frame's texture rectangle (u min, v min, u max, v max: the top-left corner's
		// coordinates, then the bottom-right one's), the next frame's, the blend toward it, the inverse soft distance, and the
		// corners as the view's previous render drew them (laid from the particle's Previous draw; the Corners themselves
		// without one), from which the motion vectors are drawn.
		glm::vec4 TexCoords = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
		glm::vec4 NextTexCoords = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
		float FrameBlend = 0.0f;
		float InverseSoftDistance = 0.0f;
		std::array<glm::vec3, 4> PreviousCorners{};

		bool operator==(const ParticlePassQuad&) const = default;
	};

	// The snapshot's particles ready to record (PlanParticlePass).
	struct ParticlePassPlan
	{
		std::vector<ParticlePassQuad> Quads{};
		std::vector<ParticlePassRun> Runs{}; // in the snapshot's emitter order; an emitter without a drawn quad has none
		uint32_t Culled = 0;                 // particles wholly outside the clip volume
		uint32_t Skipped = 0;                // particles with a non-finite position, colour or angle, or a size not finite and positive

		bool operator==(const ParticlePassPlan&) const = default;
	};

	// Plans the particles of `emitters` (each ParticleEmitterItem's range of `particles`) seen through `view` (world to view)
	// and `viewProjection` (world to clip, reverse-Z), as the file comment states, into `plan` (cleared first, its vectors'
	// capacity kept). `flipbooks` holds each emitter's resolved flipbook at the emitter's index (empty when no emitter has a
	// Sheet; an emitter with a Sheet past its end draws nothing). A view matrix whose right or up row is not finite and
	// non-zero plans nothing. Errors (the plan still holds every other emitter's quads): InvalidArgument naming the first
	// emitter whose range does not lie inside `particles` (data no extraction produces; that emitter is skipped). Pure,
	// deterministic and thread-compatible.
	[[nodiscard]] Status PlanParticlePass(std::span<const ParticleEmitterItem> emitters, std::span<const ParticleItem> particles,
		std::span<const ParticleFlipbook> flipbooks, const glm::mat4& view, const glm::mat4& viewProjection, ParticlePassPlan& plan);

	struct ParticlePassInputs
	{
		const ParticlePassPlan* Plan = nullptr; // never null
		GpuResourceCache* Resources = nullptr;  // resolves the runs' textures; never null
		// SceneColor alone (ParticlePass::GetColorFramebufferInfo); never null.
		nvrhi::IFramebuffer* ColorFramebuffer = nullptr;
		// SceneVelocity alone (ParticlePass::GetVelocityFramebufferInfo), for a view with TAA; null draws no motion vectors.
		nvrhi::IFramebuffer* VelocityFramebuffer = nullptr;
		// The view's ViewConstants (b0): ViewProjection and PreviousViewProjection (jittered with TAA), the projection kind,
		// Near and Far for the soft fade, ViewportSize for the motion vectors; never null.
		nvrhi::IBuffer* ViewConstants = nullptr;
		// The view's fog constants (b1, Shared/FogConstants.h; M32's fog at M33's merge,
		// Docs/Decisions/0048-m33-particle-look.md decision 10): with Enabled set, Alpha particles blend toward the fog's
		// colour and Additive ones fade by its amount at each fragment's world position; never null (Enabled 0 draws without
		// fog).
		nvrhi::IBuffer* Fog = nullptr;
		// SceneDepth, shader-readable, ColorFramebuffer's size; never null. Bound as a texture (t1): the fragments test it.
		nvrhi::ITexture* SceneDepth = nullptr;
		QuadSampler Sampler = QuadSampler::Linear;
		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): the rectangle rasterization is limited to (a mirror
		// view's glass rectangle, widened by its blur's footprint); the whole framebuffer when empty.
		std::optional<nvrhi::Rect> Scissor{};
	};

	class ParticlePass
	{
	public:
		// ParticlesAlpha, ParticlesAdditive and ParticlesVelocity.
		static constexpr uint32_t PipelineCount = 3;
		// The vertex buffer's capacity in quads (2^18 vertices, 16 MiB of vertex data): the most one chunk draws. A Record of
		// more draws them all, in their order, chunk by chunk, each written into the vertex buffer and drawn before the next is
		// written (as the QuadBatcher's records).
		static constexpr uint32_t MaxQuadsPerBatch = 1U << 16;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class ParticlePass;
		};

		// Use Create.
		explicit ParticlePass(ConstructionKey key);
		~ParticlePass();

		ParticlePass(const ParticlePass&) = delete;
		ParticlePass& operator=(const ParticlePass&) = delete;

		// Creates the three pipelines (checked against their reflection), the two samplers (Point and Linear, clamp to edge,
		// without mips: QuadSampler's values) and the index buffer. `device` is a documented back-reference that outlives the
		// pass. Errors: those of PipelineFactory and the GraphicsDevice wrappers (Gpu: an out-of-memory creation, §8.14 item 7).
		[[nodiscard]] static Result<Scope<ParticlePass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		// The layout descriptions of the three pipelines, for "Shaders: LayoutsMatchReflection".
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		// SceneColor alone (the colour pipelines' framebuffer) and SceneVelocity alone (the velocity pipeline's).
		[[nodiscard]] static nvrhi::FramebufferInfo GetColorFramebufferInfo();
		[[nodiscard]] static nvrhi::FramebufferInfo GetVelocityFramebufferInfo();
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// Records `inputs.Plan`'s runs under the caller's stats row ("Particles"), as the file comment states, with binding sets
		// from the view's `bindings`, adding the draws and triangles to `counters`. Returns the quads drawn (each once, whether
		// or not its motion was drawn too). A run whose texture has no mirror at all (not even its placeholder, which the cache
		// reported) draws nothing. Asserts the inputs. Errors: InvalidArgument for a missing input (asserted); Gpu when a
		// binding set or a buffer cannot be created (nothing is drawn).
		[[nodiscard]] Result<uint32_t> Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const ParticlePassInputs& inputs,
			RenderPassCounters& counters);
	private:
		// The back-reference, the pipelines, the samplers and the buffers (ParticlePass.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
