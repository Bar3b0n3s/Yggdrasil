#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/TextRenderer.h"
#include "Engine/Renderer/UiDrawList.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// The Ui pass (M22, C22.1, R-UI-RENDER; Docs/Decisions/0033-m22-contract.md decision 7): the snapshot's UI draw list
// (RenderSnapshot::Ui) drawn in M18's reserved slot, the SceneOverlay stage's second row "Ui", after game Text and before the
// Output stage's upscale (Docs/Decisions/0029-m18-contract.md decisions 1 and 8; ADR 0028's phase-2 ledger), into the scene
// overlay's framebuffer at the scene resolution, so UI is part of the game's image, upscaled with it and under the editor
// overlays. The data views (AO, ShadowCascades, Overdraw) draw no UI, as they draw no Text.
//
// Two halves:
//   - PlanUiPass (CPU, pure but for the asset loads): resolves every item's sheet frame and font through the AssetManager,
//     expands sliced frames into their 9-slice patches (ExpandNineSlice, Renderer/QuadBatcher.h), lays out every text in its
//     rectangle (MeasureText, Renderer/TextMeasurement.h) and splits the result into runs in draw order.
//   - RecordUiPass (GPU): records the runs with M18's QuadBatcher, through its Ldr pipeline and its scissor (images, plain
//     rectangles, annotation outlines), and with the TextRenderer's glyph runs (texts, annotation labels), each run clipped to
//     its items' clip rectangle (UiClipRect, the scissor intersected with the framebuffer).
// The pass creates no GPU object of its own: the QuadBatcher and the TextRenderer are SceneRendererPipelines', the textures
// the GpuResourceCache's mirrors and the font atlases the TextRenderer's, and its binding sets come from the view's
// PassBindingCaches (SceneRenderer's state), so the recreation protocol covers it through those owners (C18.3's placement
// rule; "UiPass: a device loss recreates nothing of the pass's own and the UI draws the same image afterwards").
//
// Deterministic: items in list order; an image's 9-slice patches row-major; a text's glyphs in MeasureText's order (line
// by line, each line in visual order: C22.3's right-to-left runs reversed, a shaped cluster one glyph or several). Main
// thread.
//
// C22.3 (Docs/Decisions/0033-m22-contract.md decisions 42, 48 and 51):
//   - Fallback fonts (stream F): a text lays out with its font's chain (BuildUiFontChain), so a codepoint its font lacks
//     takes the first font of the chain that has it; runs split where a glyph's font changes, as before.
//   - The caret (stream T): an item with HasCaret draws a caret quad after its glyphs (PlanUiPass's comment).
//   - World canvases (stream W): the items of a World canvas (UiDrawItem::WorldCanvas) are planned in its canvas pixels,
//     clipped on the CPU to their clip and to the canvas's rectangle, outside which a World canvas draws nothing (each
//     quad's rectangle and texture coordinates cut to it, exact for these axis-aligned quads; a glyph likewise), and
//     recorded first, canvas by canvas from the farthest to the nearest (the view-space depth of each canvas's centre, ties
//     in list order), transformed by the canvas's World matrix and the view's view-projection and depth-tested against
//     SceneDepth without depth writes: images through the QuadBatcher's depth-tested LDR target (QuadBatchTarget::LdrDepth,
//     decision 57's ledger amendment, granted on main in 1707cf47) and glyphs through the TextRenderer's world pipeline
//     (TextGlyphRun::World); then the Screen canvases' runs as before, on top. A view without a camera draws no World
//     canvas. The world pipeline is SceneRendererPipelines' (the QuadBatcher's), so the placement rule covers it, with its
//     own loss test ("UiPass: a device loss recreates the world canvases' pipeline and a World canvas draws the same image
//     afterwards"). World Text, recorded in the Text row before the Ui row and depth-tested without depth writes like World
//     canvases, never hides a World canvas: where they overlap the canvas draws over it, whichever is nearer (an accepted
//     limit, decision 51, Backlog §6).

namespace Engine {

	struct RenderPassCounters;

	class AssetManager;
	class GpuResourceCache;
	class QuadBatcher;

	// The size of a capture annotation's label (UiAnnotation::Label): scene pixels per em of the Default font.
	inline constexpr float UiAnnotationLabelSize = 13.0f;

	// C22.3 (decision 42): the most fonts a text's chain holds (its own font included).
	inline constexpr size_t MaxUiFontChainLength = 8;

	// One textured quad of a plan, axis-aligned in scene pixels. Texture names the texture asset that RecordUiPass resolves
	// through the GpuResourceCache (a sheet's texture); null samples the QuadBatcher's opaque white texel.
	struct UiPassQuad
	{
		glm::vec2 Min = glm::vec2(0.0f);
		glm::vec2 Max = glm::vec2(0.0f);
		glm::vec2 TexCoordMin = glm::vec2(0.0f);
		glm::vec2 TexCoordMax = glm::vec2(1.0f);
		glm::vec4 Color = glm::vec4(1.0f); // linear RGB, straight alpha
		AssetHandle Texture{};
		UiDrawFilter Filter = UiDrawFilter::Point;
		// C22.3: UiScreenCanvas, or the World canvas (UiDrawList::WorldCanvases) whose pixels Min and Max are in.
		uint32_t WorldCanvas = UiScreenCanvas;

		bool operator==(const UiPassQuad&) const = default;
	};

	// What a run records with.
	enum class UiPassRunKind : uint8_t
	{
		Quads, // QuadBatcher::Record, UiPassPlan::Quads[First, First + Count)
		Glyphs // TextRenderer::RecordGlyphRuns, UiPassPlan::Glyphs[First, First + Count)
	};

	// Consecutive quads or glyphs that one record draws: one kind and one clip, and for glyphs one font and one colour.
	struct UiPassRun
	{
		UiPassRunKind Kind = UiPassRunKind::Quads;
		uint32_t First = 0;
		uint32_t Count = 0;
		bool Clipped = false; // Clip is the run's scissor; false: the whole framebuffer
		UiClipRect Clip{};
		AssetHandle Font{};                // Glyphs: null is the Default font
		glm::vec4 Color = glm::vec4(1.0f); // Glyphs: the text's colour
		// C22.3: UiScreenCanvas, or the World canvas the run's quads or glyphs are in (already clipped on the CPU: Clipped is
		// false); runs also split where it changes.
		uint32_t WorldCanvas = UiScreenCanvas;

		bool operator==(const UiPassRun&) const = default;
	};

	// A UI draw list ready to record (PlanUiPass).
	struct UiPassPlan
	{
		std::vector<UiPassQuad> Quads{};
		std::vector<TextPixelGlyph> Glyphs{};
		std::vector<UiPassRun> Runs{}; // in draw order
		uint32_t DrawnItems = 0;       // items that drew anything (an empty text or a clipped-away item draws nothing)
		// Images whose sheet is missing, failed or of another type, or lacks the item's Frame: they draw nothing. PlanUiPass
		// reports such a sheet through the AssetManager's shared failure policy (PlanUiPass's comment); a frame index out of
		// range is project.validate's UI_IMAGE_FRAME_OUT_OF_RANGE.
		uint32_t SkippedItems = 0;
		// C22.3 (decision 51; stream W): the list's World canvases (UiDrawList::WorldCanvases), which the runs' and quads'
		// WorldCanvas index, so RecordUiPass orders them and places their quads and glyphs with their matrices.
		std::vector<UiWorldCanvas> WorldCanvases{};

		bool operator==(const UiPassPlan&) const = default;
	};

	// Plans `list` (decision 7 has every rule):
	//   - Items in order, then the annotations, unclipped: an Outline as four one-pixel quads of its Color with the white texel
	//     inside its rectangle (its top and bottom rows across its width, then its left and right columns between them), then
	//     a Label, when not empty, in the Default font (a null Font) at UiAnnotationLabelSize in its Color, left-aligned, its
	//     block's bottom-left corner at the rectangle's top-left corner.
	//   - Image: a null Sheet is one quad of Color over [Min, Max] with the white texel. Otherwise the sheet
	//     (AssetManager::Load; there is no placeholder sheet, so an unknown handle, an asset of another type or a failed load
	//     skips the item and is reported once per plan through AssetManager::ReportDiagnostic as GetOrPlaceholder reports
	//     one: ASSET_MISSING, ASSET_TYPE_MISMATCH or ASSET_IMPORT_FAILED, logged once per manager) and its texture's size (the
	//     texture's CPU data, a missing or failed texture its placeholder's) give the frame's texel rectangle: Simple is one
	//     quad with the frame's rectangle as its texture coordinates (texel edges over the texture's size, no half-texel
	//     inset); Sliced is ExpandNineSlice of the frame's Rect and Borders over the rectangle's size with BorderScale, one
	//     quad per patch, offset by Min.
	//   - Text: MeasureText with the item's font (null: the Default font; missing or failed: its placeholder) at FontSize,
	//     LineSpacing and the horizontal alignment, Wrap Word when Wrap is set, and MaxWidth the rectangle's width; the block's
	//     top-left corner at Min plus the vertical alignment's offset (Start 0, Center half, End all of the height the block
	//     leaves), each glyph one TextPixelGlyph.
	//   - Runs split where the kind, the clip, or a glyph run's font or colour changes; an item whose clip is empty draws nothing.
	//   - C22.3: a text's chain is BuildUiFontChain of its font (stream F). A text item with HasCaret (stream T) then draws
	//     its caret, a quad of its Color with the white texel, max(1, floor(FontSize / 16 + 0.5)) scene pixels wide, its
	//     left edge at the x of FindTextCaret(measurement, CaretByte) and spanning that caret's line (its Top to its Top
	//     plus the measurement's LineHeight). Its block is aligned vertically as at least LineHeight high (an empty text,
	//     whose block has no height, places the caret where its first character's line will stand). When the item does not
	//     wrap, the block (glyphs and caret) then moves by the least distance, left or right, that puts the caret's quad
	//     inside [Min.x, Max.x] (none when it is inside; its left edge at Min.x when the quad is wider than the rectangle),
	//     so the caret stays visible in a text wider than its rectangle, whatever its alignment. World canvases' items as
	//     the file comment says (stream W).
	// Main thread (the asset manager).
	[[nodiscard]] UiPassPlan PlanUiPass(const UiDrawList& list, AssetManager& assets);

	// C22.3 (decision 42; stream F): the font chain a text with font `font` lays out with: the font itself (null: the
	// Default font, BuiltinAssetHandles::DefaultFont; missing or failed: its placeholder, as before C22.3), then its
	// FontData::Fallbacks, depth first in their order (a fallback's own fallbacks right after it), each font once, at most
	// MaxUiFontChainLength fonts. A fallback that is unknown, of another type or fails to load is left out (no placeholder
	// in a chain: a placeholder has every glyph a fallback is meant for) and reported once per manager through
	// AssetManager::ReportDiagnostic, as PlanUiPass reports a missing sheet. Main thread (the asset manager).
	[[nodiscard]] std::vector<AssetHandle> BuildUiFontChain(AssetHandle font, AssetManager& assets);

	struct UiPassInputs
	{
		const UiPassPlan* Plan = nullptr;      // never null
		QuadBatcher* Quads = nullptr;          // SceneRendererPipelines' batcher; never null
		TextRenderer* Text = nullptr;          // SceneRendererPipelines' text renderer; never null
		GpuResourceCache* Resources = nullptr; // resolves the quads' textures; never null
		AssetManager* Assets = nullptr;        // resolves the fonts; never null
		// The scene overlay's framebuffer at the scene resolution (GetOverlayFramebufferInfo, the LDR target the SceneOverlay
		// stage draws Text into); never null. Its pixels are the plan's scene pixels.
		nvrhi::IFramebuffer* Framebuffer = nullptr;
		nvrhi::IBuffer* ViewConstants = nullptr; // the Text pipelines' b0; never null
		// C22.3 (decision 51; stream W): the view's camera, for the World canvases (HasCamera false: none is drawn); View
		// orders them back to front and ViewProjection transforms their quads (the Text pipelines read it from b0).
		bool HasCamera = false;
		glm::mat4 View = glm::mat4(1.0f);
		glm::mat4 ViewProjection = glm::mat4(1.0f);
	};

	// Records `inputs.Plan`'s runs in order under the caller's stats row ("Ui"): a Quads run through QuadBatcher::Record
	// (QuadBatchTarget::Ldr, MakeQuadPixelTransform of the framebuffer's size, the run's clip as the scissor, each quad's
	// texture resolved through GpuResourceCache::GetTexture and its sampler from its Filter), a Glyphs run through
	// TextRenderer::RecordGlyphRuns (the run's clip as its scissor), with binding sets from the view's `quadBindings` and
	// `textBindings`, adding the draws and triangles to `counters`. Returns the plan's DrawnItems when every run recorded.
	// C22.3 (stream W): the World canvases' runs first, as the file comment orders them (with a camera only): a Quads run
	// through QuadBatchTarget::LdrDepth, its quads' corners placed by their canvas's World matrix and ViewProjection as the
	// transform, without a scissor; a Glyphs run with TextGlyphRun::World. Asserts the inputs. Errors: the first error of a
	// record (Gpu); the later runs still record.
	[[nodiscard]] Result<uint32_t> RecordUiPass(nvrhi::ICommandList& commandList, PassBindingCache& quadBindings, PassBindingCache& textBindings,
		const UiPassInputs& inputs, RenderPassCounters& counters);

}
