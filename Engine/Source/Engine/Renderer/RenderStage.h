#pragma once

#include <cstdint>
#include <string_view>

// The stages of the scene renderer's pass list (Architecture §8.3; M18, C18.1, R-PASS; Docs/Decisions/0029-m18-contract.md
// decisions 1 and 4). SceneRenderer::Render runs one stage function per stage, in this order, each recording its declared
// passes (RenderPassDeclaration.h) in their declared order or none, between the GPU-zone hook's
// RenderRecordingContext::BeginStage and EndStage. A fixed list of stages, not a render graph: a later milestone appends
// passes to a stage by appending declarations, and adds a stage only by a frozen-header change.
//
// Resolutions: every stage up to SceneOverlay records at the scene resolution (RenderOutputSettings, the view's size by
// default); Output upscales the scene image to the view; EditorOverlay records at the view's resolution, over the upscaled
// image. With the default scene resolution Output records nothing and every stage draws at the view's size.

namespace Engine {

	enum class RenderStage : uint8_t
	{
		// CPU only: culling, sorting, light packing, shadow planning, the per-view constants (§8.3 pass 1).
		Prepare,
		// Reserved for M23 (phase 2's ledger): the Skinning compute pass, which deforms the skinned and morphed meshes'
		// vertices before every later stage reads them (the shadow casters first), so its time is never part of another
		// stage's zone. Records nothing in M18.
		Deform,
		// The directional cascades and the spot shadow atlas (passes 2 and 3); M23's PointShadows join them.
		Shadows,
		// The depth/normal prepass, or the picking prepass with EntityId (pass 4).
		DepthNormal,
		// The linear view-depth pyramid and GTAO with its denoise (passes 5 and 6); M23's LightClusters at its end.
		Ambient,
		// Forward opaque, skybox, forward transparent, or the Overdraw data view's pass (passes 7 to 9).
		Forward,
		// Reserved for M21 (phase 2's ledger): the Sprites and Lights2D passes, drawn into SceneColor at the scene resolution
		// before the HDR post chain. Records nothing in M18.
		Sprites,
		// Bloom (pass 10); later HDR post passes join it.
		HdrPost,
		// Tonemap and encode into LdrColor, or the data views' composite (pass 11).
		Tonemap,
		// FXAA into LdrColor's ping-pong partner (pass 12).
		LdrPost,
		// The scene-resolution overlays drawn before the upscale: game Text (TextItem, world then screen), then the slot M22's
		// Ui pass takes (reserved, records nothing in M18). Part of the game's image, so upscaled with it.
		SceneOverlay,
		// The output stage's integer upscale of the scene image into the view-resolution target, with point sampling and the
		// letterbox (UpscalePass.h); also the point-sampled view-resolution depth the editor overlays test against. Records
		// nothing when the scene resolution is the view's.
		Output,
		// The view-resolution editor overlays drawn after the upscale: wireframe edges, the selection outline, the debug lines,
		// annotations, grid and icons, then the debug and annotation labels.
		EditorOverlay
	};

	// The number of stages: RenderStage's last enumerator plus one.
	inline constexpr uint32_t RenderStageCount = static_cast<uint32_t>(RenderStage::EditorOverlay) + 1;

	// The stage's name, its enumerator spelled as above ("Prepare", "Deform", ..., "EditorOverlay"). The GPU and CPU
	// profiler zones of a stage are named "Stage:" followed by it (RenderRecordingContext::BeginStage). A string literal
	// (static storage).
	[[nodiscard]] std::string_view RenderStageToString(RenderStage stage);

}
