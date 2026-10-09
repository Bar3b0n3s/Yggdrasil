#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// viewport.screenshot (Architecture §8.13, §13.4 "Screenshots", §13.5), the screenshot method the Editor and the Runtime
// share (§13.5 "Runtime subset: viewport.screenshot (game view)"). Its params and result moved here from
// EditorCore/Automation/ScreenshotMethods.h with the M7 contract, unchanged in name and wire format apart from the M7
// additions marked below; editor.screenshot stays in EditorCore (Docs/Decisions/0012-m7-decisions.md decision 9).
//
// Every screenshot is re-rendered for the call (§8.13): the handler extracts a snapshot of the requested view at the
// requested size (Scene/RenderExtraction.h ExtractRenderSnapshot for the edit scene; PlaySession::ExtractView for the play
// scene, which first brings the scene's transforms and interpolation tags up to date, so a write made since the last tick
// shows at once) and the host renders it through its ViewportCapture
// (AutomationMethodContext::CaptureView), never taking the last presented image. The image is downscaled so that its larger
// side is at most maxDimension (default 1024, §13.4), encoded as PNG and written to the host's output directory
// (AutomationMethodContext::WriteOutputFile); the result names the file's absolute path, which the MCP bridge returns as
// image content (§13.8), and stays below the offload threshold (§13.4): inline data is left out of a PNG larger than
// MaxInlineScreenshotPngBytes.
//
// Views (M7, ADR 0009 decision 33 deferred them here):
//   - "game": the target scene through its primary camera (FindPrimaryCamera): the play scene while playing, else the edit
//     scene (§13.11 step 7 looks at the game view while building). Without a primary camera: InvalidState naming
//     SCENE_NO_PRIMARY_CAMERA.
//   - "scene": the target scene through the editor's scene-view camera (AutomationMethodContext::GetSceneViewCamera: a
//     fixed default until the editor camera of M10). The Runtime has no scene view: Unsupported located at /view.
//   - "camera": an EntityRef of a camera entity in the view's target scene, rendered through that camera instead
//     (InvalidArgument at /camera when the entity has no CameraComponent; NotFound when it names none).
// The interpolation alpha is PlaySession::GetViewAlpha for the play scene (1 while paused, in lockstep or after a
// ManualClock frame, otherwise the last frame's Alpha; RenderSnapshot::Alpha), 1 for the edit scene.
//
// Debug views (M8, §8.5; ADR 0009 decision 33 deferred them to M8, Docs/Decisions/0013-m8-decisions.md decision 12):
// "debugView" names a RenderDebugView ("Lit", "Albedo", "Normals", "Roughness", "Metallic", "Emissive"; ParseRenderDebugView,
// ignoring ASCII case), which the handler sets on the extracted snapshot (RenderSnapshot::DebugView); absent or empty is
// Lit. M9's views ("AO", "ShadowCascades", "Overdraw") are Unsupported located at /debugView, naming M9; any other name is
// InvalidParams at /debugView listing the valid names. "annotate" (the overlay pass, M9) stays refused with Unsupported
// located at /annotate whenever it is present; nothing is ever ignored.

namespace Engine {

	class Scene;
	class AutomationMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// The largest PNG viewport.screenshot returns inline (base64 "data"): 30 KB, 40 KB of base64, which leaves the result's
	// other members (the path among them) 8 KB below the 48 KB offload threshold (DefaultOffloadThresholdBytes). A larger
	// PNG is only written, and the result reports inlineOmitted.
	inline constexpr size_t MaxInlineScreenshotPngBytes = 30 * 1024;

	// Which view viewport.screenshot renders (§13.5): registry enum "ViewportView".
	enum class ViewportView : uint8_t
	{
		Scene, // the target scene through the editor's scene-view camera (editor only)
		Game   // the target scene through its primary camera (M7)
	};

	// viewport.screenshot {view, target?, width?, height?, camera?, debugView?, annotate?, maxDimension?, inline?} (§13.5).
	struct ViewportScreenshotParams
	{
		ViewportView View = ViewportView::Scene; // required
		// M7: the scene to render (§13.4 "Target"); absent: the play scene while playing, else the edit scene.
		SceneTarget Target = SceneTarget::Edit;
		uint32_t Width = 640; // 1 to MaxViewportScreenshotDimension; the rendered size before maxDimension
		uint32_t Height = 360;
		std::string Camera{};         // an EntityRef of a camera entity of the target scene (M7)
		std::string DebugView{};      // a debug view name (M8; see the file comment); empty: Lit
		VariantValue Annotate{};      // {labels, colliders, bounds, axes} (M9)
		uint32_t MaxDimension = 1024; // 1 to MaxViewportScreenshotDimension
		bool Inline = false;          // also return the PNG as base64 in "data", when it fits MaxInlineScreenshotPngBytes
	};

	struct ViewportScreenshotResult
	{
		ViewportView View = ViewportView::Scene;
		// M7: the scene that was rendered.
		SceneTarget Target = SceneTarget::Edit;
		// M7: the camera entity it was rendered through; every member empty for the editor's scene-view camera.
		EntitySummary Camera{};
		std::string Path{};                 // the PNG's absolute native path
		std::string MimeType = "image/png"; // "mimeType"
		uint32_t Width = 0;                 // the PNG's size, after maxDimension
		uint32_t Height = 0;
		// With inline: the PNG, base64 (RFC 4648, with padding), when it is at most MaxInlineScreenshotPngBytes; empty
		// otherwise. Inline images suit a small maxDimension.
		std::string Data{};
		// With inline and a PNG larger than MaxInlineScreenshotPngBytes: true, and Data is empty (the PNG is at Path).
		bool InlineOmitted = false;
	};

	struct ViewportAnnotationOptions
	{
		RenderAnnotations Annotations{};
		std::vector<std::string> LabelReferences{}; // unresolved EntityRefs for labels:[ids], in request order
		bool Colliders = false;
	};

	// Strict object: labels = "all"|"selection"|[EntityRefs]; "selected" aliases "selection", "none" disables labels.
	// Strings ignore ASCII case; array strings are exact EntityRefs (UUID/prefix/path), never enum spellings. Up to 1000
	// nonempty refs; [] selects Explicit with no labels. colliders/bounds/axes booleans default false. Absent labels=None.
	// Pure parse only: array refs stay owned in LabelReferences; no lookup/selection mutation. Unknown members and wrong
	// types/count/values return InvalidArgument at /annotate/member[/index]. No partial options returned on failure.
	[[nodiscard]] Result<ViewportAnnotationOptions> ParseViewportAnnotations(const VariantValue& value);

	// Main thread. Resolve every LabelReference against the screenshot's already resolved target scene through the
	// context's normal EntityRef resolver; NotFound/ambiguity/invalid refs retain /annotate/labels/index locations.
	// All references must succeed, then sort/deduplicate copied UUIDs into Annotations.LabelEntities. Explicit IDs work
	// in Runtime. All/None/Selected retain empty LabelEntities: the screenshot host separately fills SelectedEntities
	// from context.GetSelectedEntities, filtered to target (empty in Runtime), before extraction. Reject inconsistent
	// options with InvalidArgument; borrow inputs for this call only, return owns values. No scene/editor mutation.
	[[nodiscard]] Result<RenderAnnotations> ResolveViewportAnnotations(AutomationMethodContext& context, Scene& scene,
		const ViewportAnnotationOptions& options);

	namespace Automation {

		// viewport.screenshot on any host. Errors: InvalidParams for the ranges and an unknown debug view (at /debugView);
		// Unsupported at /debugView for M9's debug views, at /annotate when present, at /view for "scene" in the Runtime, and
		// without a device (AutomationMethodContext::CaptureView);
		// InvalidState "not playing" or "no scene open" from the target, and naming SCENE_NO_PRIMARY_CAMERA for a game view
		// without a primary camera; NotFound and InvalidArgument at /camera; those of ExtractRenderSnapshot; the capture's
		// errors (a Gpu error is Internal) with the context "while rendering the viewport"; those of DownscaleImage, EncodePng
		// and AutomationMethodContext::WriteOutputFile.
		[[nodiscard]] Result<ViewportScreenshotResult> ViewportScreenshot(AutomationMethodContext& context, const ViewportScreenshotParams& params);

	}

	// Registers ViewportView, ViewportScreenshotParams and ViewportScreenshotResult. Both hosts call it through
	// RegisterSharedMethodTypes (the editor from RegisterEditorMethodTypes).
	void RegisterViewportScreenshotMethodTypes(TypeRegistry& registry);

	// Registers viewport.screenshot typed on AutomationMethodContext: a read-only tool (§13.8), AvailableInRuntime, not
	// available in the launcher state, no dry run (it renders, it changes nothing) and not a batch op (it writes a file
	// outside a command).
	void RegisterViewportScreenshotMethods(MethodRegistry& methods);

}
