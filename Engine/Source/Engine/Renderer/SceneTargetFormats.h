#pragma once

#include "Engine/Core/Base.h"

#include <nvrhi/nvrhi.h>

// The formats of the scene renderer's per-view targets (Architecture §8.3), which several passes create pipelines for: the
// depth/normal prepass and the forward passes (SceneRenderer), the skybox (SkyboxPass, which draws into SceneColor and tests
// against SceneDepth without writing it), the tonemap (TonemapPass, SceneColor to LdrColor), FXAA (FxaaPass, LdrColor
// ping-pong), the debug lines (DebugRenderer) and the text (TextRenderer), both of which draw into LdrColor and test against
// SceneDepth without writing it (the attachment itself is not marked read-only, Docs/Decisions/0013-m8-decisions.md
// decision 20).
// Frozen by the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 4); a pass never assumes another format.

namespace Engine {

	// SceneDepth: reverse-Z (cleared to 0, compared GreaterOrEqual, §8.3).
	inline constexpr nvrhi::Format SceneDepthFormat = nvrhi::Format::D32;
	// SceneNormals: octahedral view-space normals of the prepass (normal-mapped from M8).
	inline constexpr nvrhi::Format SceneNormalsFormat = nvrhi::Format::RG16_FLOAT;
	// SceneColor: linear HDR radiance (§8.9).
	inline constexpr nvrhi::Format SceneColorFormat = nvrhi::Format::RGBA16_FLOAT;
	// LdrColor (and its FXAA ping-pong partner): display-encoded values (§8.9), written by compute passes as a storage image
	// and drawn into by the overlay and text passes.
	inline constexpr nvrhi::Format LdrColorFormat = nvrhi::Format::RGBA8_UNORM;

	// The framebuffer of the forward and skybox passes: SceneColor with SceneDepth.
	[[nodiscard]] inline nvrhi::FramebufferInfo GetSceneColorFramebufferInfo()
	{
		nvrhi::FramebufferInfo info;
		info.addColorFormat(SceneColorFormat);
		info.setDepthFormat(SceneDepthFormat);
		return info;
	}

	// The framebuffer of the overlay (debug lines) and text passes: an LdrColor target with SceneDepth (§8.3 passes 13 and 14:
	// "LdrColor (+ SceneDepth read)").
	[[nodiscard]] inline nvrhi::FramebufferInfo GetOverlayFramebufferInfo()
	{
		nvrhi::FramebufferInfo info;
		info.addColorFormat(LdrColorFormat);
		info.setDepthFormat(SceneDepthFormat);
		return info;
	}

}
