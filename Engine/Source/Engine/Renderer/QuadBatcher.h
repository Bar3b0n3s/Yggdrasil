#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The QuadBatcher (M18, C18.1, R-2D-BATCH; Docs/Decisions/0029-m18-contract.md decision 10): batched textured quads, the
// shared 2D primitive of M21's sprites and tilemaps, M22's UI images and M26's particles. A record draws its quads in the
// order given (painter's order: the caller sorts, so a deterministic caller draws a deterministic image), batched into one
// geometrically growing dynamic vertex buffer of QuadVertex (Shared/QuadVertex.h, four vertices per quad) and one static
// index buffer, split into draws only where the texture or the sampler changes.
//
// Pipelines (two, for QuadBatchTarget): straight-alpha blending (colour SrcAlpha / InvSrcAlpha, alpha One / InvSrcAlpha,
// as TextRenderer), no culling, no depth writes.
//   - Hdr: into SceneColor with SceneDepth (GetSceneColorFramebufferInfo), depth-tested GreaterOrEqual; the fragment is
//     the sampled texel times the vertex colour, in linear radiance (the Sprites stage).
//   - Ldr: into an LDR target with SceneDepth (GetOverlayFramebufferInfo, the scene overlay's framebuffer that Text uses),
//     without the depth test; the product is encoded with the sRGB OETF in the shader (the target holds display-encoded
//     values, §8.9), as Text encodes its colour (the SceneOverlay stage's UI).
// Colour textures are sampled through their sRGB views, so the shader sees linear values; an 8-bit sRGB texel drawn with a
// white colour reaches an Ldr target, or an Hdr one through the 2D colour path (RenderOutputSettings::PixelArt), unchanged.
//
// Samplers: Point (nearest) and Linear (bilinear), both clamp to edge, without mips (GetSampler); a quad picks one. With
// Point, a quad whose screen size is an integer multiple of its texel rectangle and whose corners lie on pixel corners
// reproduces every source texel exactly ("QuadBatcher: an integer-scale draw reads back its source texels").
//
// 9-slice (ExpandNineSlice): a frame's texel rectangle with left, top, right and bottom borders expands into up to nine
// quads over a destination rectangle: the corners keep their texel size times the border scale, the edges stretch along
// one axis and the centre along both.
//
// Created once per device at startup and owned by SceneRendererPipelines, which creates it through CreatePipelines (its
// declaration "QuadBatcher" in RenderPassDeclaration.h); the batcher keeps no binding sets (each Record takes them, one per
// texture and sampler, from the caller's PassBindingCache) and records under its caller's stats row. At most
// MaxQuadsPerRecord quads per record, so quads that external input controls cannot exhaust memory: the rest are skipped and
// counted, and the batcher logs a warning the first time it skips one. A quad with a non-finite corner, texture coordinate
// or colour is skipped. Main thread only; not copyable or movable.

namespace Engine {

	struct RenderPassCounters;

	class GraphicsDevice;

	// How a quad samples its texture; both clamp to edge.
	enum class QuadSampler : uint8_t
	{
		Point, // nearest texel: pixel art, integer upscales
		Linear // bilinear
	};

	// The pipeline (and framebuffer format) a batch records with (see the file comment).
	enum class QuadBatchTarget : uint8_t
	{
		Hdr,
		Ldr
	};

	// One textured quad. Corners are in the batch's space; TexCoordMin lands on the top-left corner and TexCoordMax on the
	// bottom-right one.
	struct QuadDraw
	{
		// Top-left, top-right, bottom-right, bottom-left (as the texture's top-left texel appears at the top-left corner).
		std::array<glm::vec3, 4> Corners{};
		glm::vec2 TexCoordMin = glm::vec2(0.0f);
		glm::vec2 TexCoordMax = glm::vec2(1.0f);
		glm::vec4 Color = glm::vec4(1.0f); // linear RGB, straight alpha; multiplies the texel
		// Shader-readable, in ShaderResource between command lists; null samples an opaque white texel.
		nvrhi::ITexture* Texture = nullptr;
		QuadSampler Sampler = QuadSampler::Point;
	};

	// One record's quads and target.
	struct QuadBatchInputs
	{
		std::span<const QuadDraw> Quads{};
		QuadBatchTarget Target = QuadBatchTarget::Ldr;
		// Matches GetFramebufferInfo(Target); never null. The viewport is its whole extent.
		nvrhi::IFramebuffer* Framebuffer = nullptr;
		// The clip rectangle of the record's draws, in the framebuffer's pixels (nvrhi::Rect's minX, maxX, minY, maxY: x
		// right, y down, the maxima exclusive): applied as their scissor, intersected with the framebuffer's extent, so a rectangle partly or wholly outside it is
		// valid (wholly outside, or empty: the record draws nothing and returns 0). nullopt: the whole extent. The clipping of
		// M22's scroll views and masked UI.
		std::optional<nvrhi::Rect> Scissor{};
		// The batch's space to clip space (QuadConstants::Transform): MakeQuadPixelTransform for scene pixels, a camera's
		// view-projection for world space.
		glm::mat4 Transform = glm::mat4(1.0f);
	};

	// A frame of a texture for ExpandNineSlice: its texel rectangle and its borders (the SpriteSheet frame's 9-slice borders).
	struct NineSliceSource
	{
		glm::uvec2 TextureSize = glm::uvec2(1); // texels, both >= 1
		glm::uvec2 RectMin = glm::uvec2(0);     // the frame's top-left texel
		glm::uvec2 RectSize = glm::uvec2(1);    // the frame's size in texels, inside the texture
		// Left, top, right, bottom, in texels; left + right <= RectSize.x and top + bottom <= RectSize.y.
		glm::uvec4 Borders = glm::uvec4(0);
	};

	// One patch of a 9-slice: a destination rectangle (x right, y down, origin at the destination's top-left corner) and the
	// normalized texture rectangle it shows.
	struct NineSlicePatch
	{
		glm::vec2 Min = glm::vec2(0.0f);
		glm::vec2 Max = glm::vec2(0.0f);
		glm::vec2 TexCoordMin = glm::vec2(0.0f);
		glm::vec2 TexCoordMax = glm::vec2(0.0f);

		bool operator==(const NineSlicePatch&) const = default;
	};

	// ExpandNineSlice's patches: Patches[0, Count) in row-major order, top row first, left to right.
	struct NineSliceExpansion
	{
		std::array<NineSlicePatch, 9> Patches{};
		uint32_t Count = 0;
	};

	// The 9-slice of `source` over a destination of `size` (destination units, both >= 0), its borders `borderScale`
	// destination units per texel (> 0). Columns: the left border's width is Borders.x * borderScale, the right one's
	// Borders.z * borderScale and the middle column the rest; when the two borders are wider than the destination they shrink
	// in proportion to fill it exactly and the middle column vanishes. Rows likewise with Borders.y and Borders.w. Texture
	// coordinates are texel edges over TextureSize, without a half-texel inset. A patch of zero width or height (a zero
	// border, a vanished middle) is left out. Pure, thread-safe, deterministic. Asserts the documented ranges.
	[[nodiscard]] NineSliceExpansion ExpandNineSlice(const NineSliceSource& source, glm::vec2 size, float borderScale = 1.0f);

	// The transform of scene-pixel space (x right, y down, origin at the top-left corner of a `width` x `height` target, both
	// >= 1) to clip space (+y up, §8.3): pixel corner (0, 0) to (-1, 1) and (width, height) to (1, -1), z unchanged.
	[[nodiscard]] glm::mat4 MakeQuadPixelTransform(uint32_t width, uint32_t height);

	class QuadBatcher
	{
	public:
		// Hdr and Ldr.
		static constexpr uint32_t PipelineCount = 2;
		// The most quads one Record draws (2^20 vertices, 36 MiB of vertex data).
		static constexpr uint32_t MaxQuadsPerRecord = 1U << 18;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class QuadBatcher;
		};

		// Use Create.
		explicit QuadBatcher(ConstructionKey key);
		~QuadBatcher();

		QuadBatcher(const QuadBatcher&) = delete;
		QuadBatcher& operator=(const QuadBatcher&) = delete;

		// Creates the two pipelines (checked against their reflection), the two samplers, the white texture and the index
		// buffer. `device` is a documented back-reference that outlives the batcher. Errors: those of PipelineFactory and the
		// GraphicsDevice wrappers (Gpu: an out-of-memory creation, §8.14 item 7).
		[[nodiscard]] static Result<Scope<QuadBatcher>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		// The layout descriptions of both pipelines, for "Shaders: LayoutsMatchReflection".
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		// The framebuffer a batch of `target` records into (see the file comment).
		[[nodiscard]] static nvrhi::FramebufferInfo GetFramebufferInfo(QuadBatchTarget target);
		[[nodiscard]] uint32_t GetPipelineCount() const;
		// The batcher's sampler of `sampler`, for passes that sample like it (M21's tilemaps).
		[[nodiscard]] nvrhi::ISampler* GetSampler(QuadSampler sampler) const;

		// Records `inputs.Quads` into `inputs.Framebuffer` (asserted non-null), clipped to `inputs.Scissor`, with binding sets
		// from the caller's `bindings`, adding the draw calls and triangles to `counters`. Returns the number of quads drawn
		// (skipped ones, see the file comment, do not count). Records nothing for no quads or an empty clip rectangle.
		// Errors: Gpu when the vertex buffer or a binding set cannot be created (the quads are skipped).
		[[nodiscard]] Result<uint32_t> Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const QuadBatchInputs& inputs,
			RenderPassCounters& counters);
	private:
		// The back-reference, the pipelines, the samplers, the white texture and the buffers (QuadBatcher.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
