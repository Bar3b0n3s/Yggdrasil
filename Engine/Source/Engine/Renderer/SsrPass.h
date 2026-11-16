#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Shared/SsrConstants.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The SSR pass: glossy screen-space reflections with the probe fallback (M26, C26.1, R-SSR-IN, R-SSR;
// Docs/Decisions/0037-m26-contract.md decisions 13, 14 and 18; Architecture §8.3), the Ssr declaration's three pipelines of
// the program Ssr (Passes/Ssr.slang) in the Forward stage, after the skybox and before the transparent pass, recorded under the
// stats row SSR only with PostProcessSettings::SsrEnabled in a Lit view with a camera outside the wireframe. SSR off records
// nothing (not even the depth pyramid it would trace over without GTAO), so the image is byte-identical to the render without
// the pass. M32 (Docs/Decisions/0047-m32-fog-ssr.md decisions 10 and 11): its cost follows the glossy area. The trace image is
// the scene image, or with SsrPassInputs::HalfResolution its half (GetTraceSize), cut into 8 x 8 tiles:
//   1. CSClassify (compute, one group per tile): a tile with a trace pixel that traces (its scene pixel's surface reflects,
//      SceneRoughness word 0's multiplier not 0, the sky excluded, and its roughness, word 0's a, is at most SsrMaxRoughness)
//      appends itself to the view's tile list (SsrPassInputs::Tiles) and counts its tracing pixels in the view's arguments
//      buffer (SsrPassInputs::Arguments, SsrArgumentsLayout), which Record resets first.
//   2. CSTrace (compute, dispatched indirectly, one group per listed tile): each pixel of the tile traces the reflection of its
//      view ray in screen space, at most SsrMaxDistance metres, over the depth pyramid (DepthPyramidPass: whole cells in front
//      of the ray skipped a mip coarser, the others looked into a mip finer, down to SceneDepth's pixels), and hits a surface
//      it passes within SsrThickness behind its depth that faces it; it writes SsrColor (SsrColorFormat): rgb SceneColor's
//      radiance at the hit, a the confidence, which fades with the distance travelled, the screen's edge and the roughness,
//      and is 0 where the ray missed or the pixel does not trace. At half resolution a trace pixel traces the scene pixel at
//      its footprint's centre (Common/Gtao.slang's GtaoFullPixel) over the full-resolution pyramid.
//   3. VSComposite and PSComposite (graphics, drawn indirectly, one instance per listed tile): two triangles covering the
//      tile's scene pixels, added into SceneColor (One + One, rgb only), that replace, in proportion to the confidence times
//      SsrIntensity, the forward pass's probe or environment specular by the traced radiance: the composite evaluates the
//      forward pass's term (the prefiltered radiance at the reflection of SceneNormals' normal and SceneRoughness' roughness,
//      Common/Probes.slang over the environment's, times word 0's multiplier and SpecularOcclusion with min(word 1's material
//      occlusion, GTAO)) and adds (traced - prefiltered) times that factor, so where a ray missed (confidence 0) the probes'
//      reflection stays (the probe fallback). At half resolution the traced reflection of a scene pixel that traces is
//      reconstructed from SsrColor's four bilateral taps around it with GTAO's weights (ViewDepth and SceneNormals), and a
//      pixel that does not trace adds nothing. M32 (decision 6): in a fogged view the addition is also multiplied by what the
//      fog in front of the pixel leaves of it (1 - the fog's amount, Common/Fog.slang), so the opaque image and its
//      reflection fog as one surface.
// At full resolution every pixel of a listed tile is traced and composited as the M26 pass traced and composited every pixel,
// and a pixel of an unlisted tile traced nothing and added 0, so the image is the same: only the tiles without a glossy pixel
// are no longer dispatched or drawn. Deterministic on a device: each pixel is written once by each step, from a bounded number
// of fixed steps, with no random or temporal term; the atomics decide only the tiles' order in the list and the counts' sums,
// and each tile writes only its own pixels.
//
// The pipelines are SceneRendererPipelines' (the placement rule's place 1, ADR 0029 decision 27), created by CreatePipelines;
// SsrColor and the composite's framebuffer (SceneColor alone) are each SceneRenderer's targets (place 2: SceneRendererTargets'
// SsrColor and SsrFramebuffer, from GetTargetDesc and the target pool), and the tile list and the arguments buffer the
// renderer's view state (place 2, GetTileBufferDesc and GetArgumentsBufferDesc), created by the first render with SSR and
// released with the targets by a resize, a change of the trace's resolution or a device loss's release. The pass keeps no
// binding sets: each Record takes them from the view's PassBindingCache. Main thread only; not copyable or movable.

namespace Engine {

	class GraphicsDevice;

	// The arguments buffer's layout (SsrPassInputs::Arguments, M32), in 32-bit words: CSClassify's counts and the indirect
	// arguments they feed. The renderer reads the traced area back from a copy of it (ReadSsrArea).
	struct SsrArgumentsLayout
	{
		static constexpr uint32_t TraceGroupsOffset = 0;    // CSTrace's dispatch arguments: (listed tiles, 1, 1)
		static constexpr uint32_t CompositeDrawOffset = 12; // the composite's draw arguments: (6, listed tiles, 0, 0)
		static constexpr uint32_t CompositeInstancesOffset = 16;
		static constexpr uint32_t TracedPixelsOffset = 28; // the trace pixels that trace, summed over the listed tiles
		static constexpr uint32_t TilesOffset = 32;        // the trace image's tile count, which Record writes
		static constexpr uint32_t ByteSize = 36;
	};

	// The traced area of one SSR record (M32, decision 13): the trace image's 8 x 8 tiles, the tiles the trace covered and the
	// trace pixels that traced in them.
	struct SsrArea
	{
		uint32_t Tiles = 0;
		uint32_t TracedTiles = 0;
		uint32_t TracedPixels = 0;

		bool operator==(const SsrArea&) const = default;
	};

	// One record's resources and settings, borrowed for the call; every pointer is required.
	struct SsrPassInputs
	{
		// The view's per-view constants: ViewConstants (b0), EnvironmentConstants (b2), ProbeConstants (b4) and, from M32,
		// FogConstants (b5, Shared/FogConstants.h).
		nvrhi::IBuffer* ViewConstants = nullptr;
		nvrhi::IBuffer* EnvironmentConstants = nullptr;
		nvrhi::IBuffer* ProbeConstants = nullptr;
		nvrhi::IBuffer* FogConstants = nullptr;
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
		// The traced reflections (GetTargetDesc at the trace image's size, GetTraceSize).
		nvrhi::ITexture* SsrColor = nullptr;
		// M32: the view's tile list (GetTileBufferDesc for at least the trace image's tiles) and arguments buffer
		// (GetArgumentsBufferDesc).
		nvrhi::IBuffer* Tiles = nullptr;
		nvrhi::IBuffer* Arguments = nullptr;
		// What the forward pass's specular term reads: GTAO's occlusion and its upsample depth (the neutral white AO and far depth
		// when GTAO did not run), the environment's prefiltered specular cube (the black cube without a map) and the view's
		// reflection probe cubes (the neutral array without probes).
		nvrhi::ITexture* AmbientOcclusion = nullptr;
		nvrhi::ITexture* LinearViewDepth = nullptr;
		nvrhi::ITexture* EnvironmentSpecular = nullptr;
		nvrhi::ITexture* ReflectionProbeCubes = nullptr;
		// PostProcessSettings' SsrIntensity, SsrMaxDistance, SsrThickness and SsrMaxRoughness (MakeSsrConstants sanitizes them).
		PostProcessSettings Post{};
		// M32 (decision 11): RenderQualitySettings::SsrHalfResolution, the trace at half resolution.
		bool HalfResolution = false;
	};

	// The push constants of one record of `post` over a `width` x `height` scene image whose depth pyramid has `pyramidLevels`
	// mips, traced at half resolution when `halfResolution` (Shared/SsrConstants.h): the image's size and its inverse, the
	// pyramid's mip count (at least 1), HalfResolution 1 or 0, and the settings made usable: a non-finite SsrMaxDistance or
	// SsrThickness takes its PostProcessSettings default and each is clamped to its registered range (SsrMaxDistance [0.1,
	// 1000] metres, SsrThickness [0.001, 10] metres), and a non-finite SsrIntensity or SsrMaxRoughness is 0, each clamped to
	// [0, 1]. Pure.
	[[nodiscard]] SsrConstants MakeSsrConstants(const PostProcessSettings& post, uint32_t width, uint32_t height, uint32_t pyramidLevels,
		bool halfResolution = false);

	// The traced area an arguments buffer's bytes (SsrArgumentsLayout, at least ByteSize of them, asserted) record: its tile
	// count, listed tiles and traced pixels. Pure.
	[[nodiscard]] SsrArea ReadSsrArea(std::span<const std::byte> arguments);

	class SsrPass
	{
	public:
		// CSClassify, CSTrace and the composite (VSComposite with PSComposite).
		static constexpr uint32_t PipelineCount = 3;
		// The trace image's tiles are TileSize x TileSize trace pixels (CSClassify's and CSTrace's groups).
		static constexpr uint32_t TileSize = 8;

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

		// Creates the three pipelines (checked against their reflection) and the composite's sampler. `device` is a documented
		// back-reference that outlives the pass. Errors: those of PipelineFactory and the GraphicsDevice wrappers (a Gpu error is
		// an out-of-memory creation, FatalError(OutOfMemory) for a caller at startup, §8.14 item 7).
		[[nodiscard]] static Result<Scope<SsrPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		// The layout descriptions of the three pipelines, CSClassify's, CSTrace's then the composite's, for "Shaders:
		// LayoutsMatchReflection": CSClassify's set 0 holds the push constants, t0 SceneDepth, t2 SceneRoughness, u1 the tile
		// list and u2 the arguments; CSTrace's the push constants, b0 ViewConstants, t0 SceneDepth, t1 SceneNormals, t2
		// SceneRoughness, t3 ViewDepth, t4 SceneColor, t15 the tile list and u0 SsrColor; the composite's the push constants, b0
		// ViewConstants, b2 EnvironmentConstants, b4 ProbeConstants, b5 FogConstants (M32), t0 SceneDepth, t1 SceneNormals, t2
		// SceneRoughness, t3 ViewDepth (M32: the half-resolution upsample), t5 SsrColor, t6 the occlusion, t7 its depth, t8 the
		// environment's specular cube, t14 the reflection probe cubes, t15 the tile list and s0 LinearClamp.
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();

		// The trace image's size for a `width` x `height` scene image: the same, or at half resolution (ceil(width / 2),
		// ceil(height / 2)), as GtaoPass's half resolution. Pure.
		[[nodiscard]] static glm::uvec2 GetTraceSize(uint32_t width, uint32_t height, bool halfResolution);
		// The 8 x 8 tiles of a trace image of `traceSize`. Pure.
		[[nodiscard]] static uint32_t GetTileCount(glm::uvec2 traceSize);
		// SsrColor of a `width` x `height` trace image (both >= 1, asserted): SsrColorFormat, shader-readable and a storage
		// image, kept in ShaderResource between command lists.
		[[nodiscard]] static nvrhi::TextureDesc GetTargetDesc(uint32_t width, uint32_t height);
		// M32: the tile list of `tileCount` tiles (at least 1): one uint each, a structured buffer readable and writable by the
		// shaders.
		[[nodiscard]] static nvrhi::BufferDesc GetTileBufferDesc(uint32_t tileCount);
		// M32: the arguments buffer (SsrArgumentsLayout::ByteSize): a raw buffer the shaders write with atomics, read as the
		// indirect arguments of the trace and the composite.
		[[nodiscard]] static nvrhi::BufferDesc GetArgumentsBufferDesc();
		// M32: a CPU-readable copy of the arguments buffer, which the renderer maps once the frame that copied into it completed.
		[[nodiscard]] static nvrhi::BufferDesc GetAreaReadbackDesc();
		// The composite's framebuffer: SceneColor alone (SceneColorFormat).
		[[nodiscard]] static nvrhi::FramebufferInfo GetCompositeFramebufferInfo();

		// The pipelines Create made (PipelineCount).
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// Records the classification, the indirect trace into inputs.SsrColor (cleared to 0 first at half resolution, since the
		// upsample reads tiles that were not traced) and the indirect composite into inputs.Framebuffer, into `commandList`
		// (open), with binding sets from the view's `bindings`, adding 2 to counters.Dispatches and 1 to DrawCalls (the
		// composite's triangles are the GPU's to count: 0 here). Errors, with nothing recorded: InvalidArgument for a missing
		// input, for SceneDepth, SceneNormals, SceneRoughness or ViewDepth of another size than SceneColor, SsrColor of another
		// size than the trace image, a tile list smaller than its tiles or an arguments buffer smaller than its layout, or a
		// framebuffer that is not SceneColor's; those of creating a binding set for new resources (Gpu). Main thread.
		[[nodiscard]] Status Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SsrPassInputs& inputs, RenderPassCounters& counters);
	private:
		// The back-reference, the pipelines and the sampler (SsrPass.cpp).
		struct State;
		Scope<State> m_State;
	};

}
