#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/TextRenderer.h"
#include "Engine/Renderer/UiDrawList.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

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
// Deterministic: items in list order; an image's 9-slice patches row-major; a text's glyphs in text order. Main thread.

namespace Engine {

	struct RenderPassCounters;

	class AssetManager;
	class GpuResourceCache;
	class QuadBatcher;

	// The size of a capture annotation's label (UiAnnotation::Label): scene pixels per em of the Default font.
	inline constexpr float UiAnnotationLabelSize = 13.0f;

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

		bool operator==(const UiPassRun&) const = default;
	};

	// A UI draw list ready to record (PlanUiPass).
	struct UiPassPlan
	{
		std::vector<UiPassQuad> Quads{};
		std::vector<TextPixelGlyph> Glyphs{};
		std::vector<UiPassRun> Runs{}; // in draw order
		uint32_t DrawnItems = 0;       // items that drew anything (an empty text or a clipped-away item draws nothing)
		// Images whose sheet is missing or failed or lacks the item's Frame: they draw nothing (the AssetManager records the
		// missing or failed sheet's diagnostic; a frame index out of range is project.validate's UI_IMAGE_FRAME_OUT_OF_RANGE).
		uint32_t SkippedItems = 0;

		bool operator==(const UiPassPlan&) const = default;
	};

	// Plans `list` (decision 7 has every rule):
	//   - Items in order, then the annotations, unclipped: an Outline as four one-pixel quads of its Color with the white texel
	//     inside its rectangle (its top and bottom rows across its width, then its left and right columns between them), then
	//     a Label, when not empty, in the Default font (a null Font) at UiAnnotationLabelSize in its Color, left-aligned, its
	//     block's bottom-left corner at the rectangle's top-left corner.
	//   - Image: a null Sheet is one quad of Color over [Min, Max] with the white texel. Otherwise the sheet
	//     (AssetManager::Load; there is no placeholder sheet) and its texture's size (the texture's CPU data, a missing or failed
	//     texture its placeholder's) give the frame's texel rectangle: Simple is one quad with the frame's rectangle as its
	//     texture coordinates (texel edges over the texture's size, no half-texel inset); Sliced is ExpandNineSlice of the
	//     frame's Rect and Borders over the rectangle's size with BorderScale, one quad per patch, offset by Min.
	//   - Text: MeasureText with the item's font (null: the Default font; missing or failed: its placeholder) at FontSize,
	//     LineSpacing and the horizontal alignment, Wrap Word when Wrap is set, and MaxWidth the rectangle's width; the block's
	//     top-left corner at Min plus the vertical alignment's offset (Start 0, Center half, End all of the height the block
	//     leaves), each glyph one TextPixelGlyph.
	//   - Runs split where the kind, the clip, or a glyph run's font or colour changes; an item whose clip is empty draws nothing.
	// Main thread (the asset manager).
	[[nodiscard]] UiPassPlan PlanUiPass(const UiDrawList& list, AssetManager& assets);

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
	};

	// Records `inputs.Plan`'s runs in order under the caller's stats row ("Ui"): a Quads run through QuadBatcher::Record
	// (QuadBatchTarget::Ldr, MakeQuadPixelTransform of the framebuffer's size, the run's clip as the scissor, each quad's
	// texture resolved through GpuResourceCache::GetTexture and its sampler from its Filter), a Glyphs run through
	// TextRenderer::RecordGlyphRuns (the run's clip as its scissor), with binding sets from the view's `quadBindings` and
	// `textBindings`, adding the draws and triangles to `counters`. Returns the plan's DrawnItems when every run recorded.
	// Asserts the inputs. Errors: the first error of a record (Gpu); the later runs still record.
	[[nodiscard]] Result<uint32_t> RecordUiPass(nvrhi::ICommandList& commandList, PassBindingCache& quadBindings, PassBindingCache& textBindings,
		const UiPassInputs& inputs, RenderPassCounters& counters);

}
