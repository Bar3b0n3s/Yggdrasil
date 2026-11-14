#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/DebugDrawList.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// §8.3 pass 14, text (Architecture §8.10): signed-distance-field glyph quads from the fonts' R8 atlases (FontData, M6's
// FontImporter: ASCII and Latin-1 at 48 px, spread 8), shaded with a smoothstep over fwidth of the distance, outside the
// outline at least the field's step across one screen pixel (the font's Spread, Passes/Text.slang; M25's friction F01,
// Docs/Decisions/0036-m25-tetris-pixel-art.md: a stroke thinner than a pixel keeps its coverage), batched per font and
// pipeline into one geometrically growing dynamic vertex buffer. Order: world texts (depth-tested against SceneDepth
// without depth writes), then screen texts and the DebugDrawList's labels (DebugText; depth-tested or on top by their mode).
// Layout comes from TextLayout.h:
//   - Screen: Size and Offset scaled by viewport height / TextReferenceHeight, the block placed by PlaceScreenText.
//   - World: an em of Size / WorldTextPixelsPerMetre metres in the entity's local XY plane (+X right, +Y up, facing +Z),
//     the block's Pivot at the entity's origin, transformed by TextItem::World; Billboard keeps the position and scale and
//     faces the camera.
//   - Labels: screen-aligned at the projected Position, centred, Size pixels per em at the 1080p reference; a label behind the
//     camera is not drawn.
// Colours are linear with straight alpha; the shader encodes with the sRGB OETF (the target holds display-encoded values,
// §8.9). Deterministic: items in snapshot order, then the list's labels in list order. An item with a vertex placed beyond
// float's range (finite but huge sizes, offsets or world matrices) is skipped, so no non-finite vertex reaches the GPU.
// At most 2^20 vertices (six per glyph quad, 48 MiB) per record, so text that external input controls (scene files,
// entity.update, scripts) cannot exhaust CPU or GPU memory: an item whose quads would exceed what remains of that budget is
// left out whole (a later, smaller one may still fit), and the renderer logs a warning the first time it leaves one out.
//
// Fonts are resolved through the AssetManager (GetOrPlaceholder<FontData>: a null handle is the Default font, a missing
// font its placeholder) and their atlases mirrored per (handle, version) by the renderer itself: a pass-owned GPU object
// (§8.14 item 1), uploaded through HostImageUpload as R8_UNORM, released by CollectStale. A missing font and a null one
// share the Default font's mirror. Binding layout (set 0): b0 ViewConstants, the push constants (TextConstants: the batch's
// font's Spread, Shared/TextVertex.h), the atlas and its sampler. The pipelines are
// created for GetOverlayFramebufferInfo(). Created once per device at startup and owned by SceneRendererPipelines; the
// renderer keeps no binding sets: each Record takes them (one per atlas) from the view's PassBindingCache, which releases a
// stale atlas's set one render after its last use. Main thread only; not copyable or movable. Frozen by the M8 contract
// (Docs/Decisions/0013-m8-decisions.md decision 11).
//
// Glyph runs (M22, C22.1, R-UI-TEXT; Docs/Decisions/0033-m22-contract.md decision 7): RecordGlyphRuns draws glyphs its caller
// laid out in the framebuffer's pixels (the Ui pass's UiText, through the text-measurement layer, TextMeasurement.h), run by
// run in the order given, each run one font and one colour with an optional scissor (UI clipping), on top (the screen
// vertices, TextVertexScreen, without the depth test), with the same atlases, sampler, shading and vertex budget as Record.
// M22 owns the font and text pipeline in phase 2 (ADR 0028's phase-2 ledger).

namespace Engine {

	struct RenderPassCounters;

	class AssetManager;
	class GraphicsDevice;

	struct TextRenderInputs
	{
		std::span<const TextItem> Texts{};
		const DebugDrawList* DebugDraw = nullptr; // its DebugText commands; may be null
		AssetManager* Assets = nullptr;           // resolves the fonts; never null
		// The final LDR target with SceneDepth (GetOverlayFramebufferInfo); never null.
		nvrhi::IFramebuffer* Framebuffer = nullptr;
		nvrhi::IBuffer* ViewConstants = nullptr; // b0; never null
		// Whether the view has a camera (RenderSnapshot::HasCamera); without one only screen texts are drawn.
		bool HasCamera = true;
		// The camera's world -> view matrix (RenderSnapshot::Camera.View), when the caller has it: a label on or behind the
		// camera plane (view-space z >= 0) is then neither recorded nor counted. Without it every label is recorded and
		// counted, and the vertex shader drops those on or behind the camera plane (the image is the same).
		std::optional<glm::mat4> CameraView{};
	};

	// M22 (C22.1): one glyph laid out in the framebuffer's pixels (x right, y down, origin at its top-left corner) with its
	// rectangle in its font's atlas (normalized, origin top left; FontGlyph::AtlasMin and AtlasMax).
	struct TextPixelGlyph
	{
		glm::vec2 Min = glm::vec2(0.0f);
		glm::vec2 Max = glm::vec2(0.0f);
		glm::vec2 AtlasMin = glm::vec2(0.0f);
		glm::vec2 AtlasMax = glm::vec2(0.0f);

		bool operator==(const TextPixelGlyph&) const = default;
	};

	// M22 (C22.1): a run of glyphs that share a font, a colour and a clip: TextGlyphRenderInputs::Glyphs[FirstGlyph,
	// FirstGlyph + GlyphCount).
	struct TextGlyphRun
	{
		AssetHandle Font{};                // null: the Default font; a missing font draws with its placeholder, as a TextItem's
		glm::vec4 Color = glm::vec4(1.0f); // linear RGB, straight alpha
		uint32_t FirstGlyph = 0;
		uint32_t GlyphCount = 0;
		// The run's scissor in the framebuffer's pixels (nvrhi::Rect: x right, y down, the maxima exclusive), intersected with
		// the framebuffer's extent; empty or wholly outside: the run draws nothing. nullopt: the whole extent.
		std::optional<nvrhi::Rect> Scissor{};
		// C22.3 (Docs/Decisions/0033-m22-contract.md decision 51; stream W): a World canvas's matrix (UiWorldCanvas::World).
		// The run's glyphs are then in that canvas's pixels: they are placed in the world by it and drawn with the
		// depth-tested pipeline of world texts (against SceneDepth, without depth writes), the view-projection from b0, and
		// Scissor must be nullopt (the Ui pass clips a world canvas's glyphs on the CPU). nullopt: the framebuffer's pixels,
		// on top, as before C22.3.
		std::optional<glm::mat4> World{};
	};

	// M22 (C22.1): the inputs of RecordGlyphRuns.
	struct TextGlyphRenderInputs
	{
		std::span<const TextPixelGlyph> Glyphs{};
		std::span<const TextGlyphRun> Runs{}; // in draw order; each run's glyphs lie inside Glyphs (asserted)
		AssetManager* Assets = nullptr;       // resolves the fonts; never null
		// The final LDR target with SceneDepth (GetOverlayFramebufferInfo), whose extent the pixels are in; never null.
		nvrhi::IFramebuffer* Framebuffer = nullptr;
		nvrhi::IBuffer* ViewConstants = nullptr; // b0; never null
	};

	class TextRenderer
	{
	public:
		// Depth-tested (world texts, tested labels) and on top (screen texts, on-top labels).
		static constexpr uint32_t PipelineCount = 2;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class TextRenderer;
		};

		// Use Create.
		explicit TextRenderer(ConstructionKey key);
		~TextRenderer();

		TextRenderer(const TextRenderer&) = delete;
		TextRenderer& operator=(const TextRenderer&) = delete;

		// Creates the two pipelines and the atlas sampler. `device` is a documented back-reference that outlives the renderer.
		// Errors: those of PipelineFactory and the GraphicsDevice wrappers (Gpu: FatalError(OutOfMemory) at startup, §8.14
		// item 7).
		[[nodiscard]] static Result<Scope<TextRenderer>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// Records the texts and labels into `inputs.Framebuffer`, with binding sets from the view's `bindings`, uploading font
		// atlases it has not mirrored yet. Returns the number of text items and labels drawn (labels behind the camera count
		// only without TextRenderInputs::CameraView, see there; items skipped for a vertex beyond float's range or the vertex
		// budget, see the file comment, do not count). A font whose atlas cannot be uploaded skips its texts and records an
		// AssetUploadFailedCode diagnostic once (§8.14 item 7). Asserts the inputs. Errors: Gpu when the vertex buffer or a
		// binding set cannot be created (the texts are skipped).
		[[nodiscard]] Result<uint32_t> Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const TextRenderInputs& inputs);

		// M22 (C22.1): records `inputs.Runs` into `inputs.Framebuffer` (see the file comment's "Glyph runs"), with binding sets
		// from the caller's `bindings`, adding the draw calls and triangles to `counters`, under the caller's stats row (the Ui
		// pass's "Ui"). Returns the number of runs drawn (a run clipped away, or left out for a vertex beyond float's range or
		// the vertex budget, does not count). A font whose atlas cannot be uploaded skips its runs and records an
		// AssetUploadFailedCode diagnostic once, as Record. Asserts the inputs. Errors: Gpu when the vertex buffer or a binding
		// set cannot be created (the runs are skipped).
		[[nodiscard]] Result<uint32_t> RecordGlyphRuns(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const TextGlyphRenderInputs& inputs,
			RenderPassCounters& counters);

		// Releases atlas mirrors whose font has a newer version in `assets`, and with `releaseUnused` those no Record used since
		// the previous call.
		void CollectStale(const AssetManager& assets, bool releaseUnused = false);

		// The font atlases mirrored now.
		[[nodiscard]] size_t GetFontAtlasCount() const;
	private:
		friend class SceneRenderer;
		[[nodiscard]] Result<uint32_t> RecordCounted(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const TextRenderInputs& inputs, RenderPassCounters& counters);
		// The back-reference, the pipelines, the sampler, the vertex buffer and the atlas mirrors (TextRenderer.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
