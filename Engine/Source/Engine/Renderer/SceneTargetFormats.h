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

	// M9 per-view targets. Shadow depth uses the same reverse-Z convention as SceneDepth.
	inline constexpr nvrhi::Format EntityIdFormat = nvrhi::Format::R32_UINT;

	// M26 (C26.1, R-TAA-VEL, R-SSR-IN; Docs/Decisions/0037-m26-contract.md decision 14): the prepass's two new per-view targets
	// at the scene resolution, after EntityId among DepthNormal's targets (ADR 0028's phase-3 ledger), written by the mesh
	// prepass and TerrainDepthNormal for every drawn surface and cleared to zero (the sky: no motion, no reflection):
	//   - SceneVelocity: the pixel's screen-space motion since the previous rendered frame, in scene pixels, current minus
	//     previous position (+x right, +y down), both unjittered: the instance record's PreviousWorld (World when the item has
	//     none, and always for a terrain, which has none: RenderSnapshot.h's TerrainItem), a deformed item's previous positions
	//     (M23's Skinning pass) and CameraData::PreviousViewProjection (the current view-projection when it is empty), so a
	//     static scene seen by a still camera has zero velocity everywhere;
	//   - SceneRoughness: what SSR's composite needs to replace the forward pass's probe or environment specular by the traced
	//     radiance exactly (decision 18), computed by the prepass with the forward pass's own expressions (Scene.slang's
	//     ShadeForward: the same normal-mapped normal, view direction, n.v and alpha), as two words of four unorm8 values each
	//     (x in the lowest byte):
	//       word 0: rgb the prefiltered radiance's multiplier before its specular occlusion, multiScatter (the DFG split sum's
	//               specular albedo lerp(DFG1, DFG2, F0) times its multi-scatter energy compensation) times
	//               HorizonOcclusion(reflection, geometric normal), linear, each in [0, 1]; a the roughness the forward pass
	//               sampled the prefiltered cube and the DFG LUT with (sqrt of alpha after geometric specular anti-aliasing),
	//               which SSR also compares with SsrMaxRoughness;
	//       word 1: x the material's occlusion (Surface::Occlusion: its occlusion map, 1 without one); y, z and w 0.
	//     The forward pass's specular occlusion SpecularOcclusion(n.v, min(material occlusion, GTAO), alpha) reads GTAO, which
	//     runs after the prepass, so the composite evaluates it itself (n.v from SceneNormals, alpha the roughness squared, GTAO
	//     sampled at the pixel as the forward pass samples it) and removes word 0's rgb times it times the prefiltered radiance
	//     at word 0's a: the forward pass's term, up to the 8-bit storage's and SceneNormals' precision. Cleared to zero: no
	//     specular to replace.
	// The attachments' order: the prepass writes SceneNormals (SV_Target0), SceneVelocity (1), SceneRoughness (2); the picking
	// prepass SceneNormals (0), EntityId (1), SceneVelocity (2), SceneRoughness (3); stream X-C adds them to the framebuffers
	// and the 12 prepass pipelines, so the mesh pipelines stay 24 (each prepass and picking pipeline gains attachments, none is
	// added), and T-C's TerrainDepthNormal pipelines are made for the same framebuffers.
	inline constexpr nvrhi::Format SceneVelocityFormat = nvrhi::Format::RG16_FLOAT;
	inline constexpr nvrhi::Format SceneRoughnessFormat = nvrhi::Format::RG32_UINT;
	// M26 (decisions 17 and 18): TAA's history (the previous resolved HDR image, SceneColor's format, kept per view across
	// renders) and SSR's traced reflections (rgb the reflected radiance, a its confidence: 0 where the ray missed or the
	// surface is rougher than SsrMaxRoughness).
	inline constexpr nvrhi::Format TaaHistoryFormat = SceneColorFormat;
	inline constexpr nvrhi::Format SsrColorFormat = nvrhi::Format::RGBA16_FLOAT;
	inline constexpr nvrhi::Format ShadowDepthFormat = nvrhi::Format::D32;
	inline constexpr nvrhi::Format ViewDepthFormat = nvrhi::Format::R16_FLOAT;
	inline constexpr nvrhi::Format AmbientOcclusionFormat = nvrhi::Format::R8_UNORM;
	inline constexpr nvrhi::Format SelectionMaskFormat = nvrhi::Format::R8_UNORM;

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

	// M21 (C21.2, Docs/Decisions/0032-m21-contract.md decision 22): the Lights2D pass's per-view targets, at the scene
	// resolution. Lights2DNormals: the lit sprites' and tiles' world normals (xyz) and whether the pixel's lit item has a normal
	// map (w, 1 or 0). Lights2DLight: the linear light every 2D light added at the pixel (rgb; a stays 0).
	inline constexpr nvrhi::Format Lights2DNormalsFormat = nvrhi::Format::RGBA16_FLOAT;
	inline constexpr nvrhi::Format Lights2DLightFormat = nvrhi::Format::RGBA16_FLOAT;

	// The framebuffer of the lit items' normals (the QuadBatcher's Normals pipeline): Lights2DNormals with SceneDepth, so a
	// mesh in front of a sprite hides its normal as it hides its colour.
	[[nodiscard]] inline nvrhi::FramebufferInfo GetLights2DNormalsFramebufferInfo()
	{
		nvrhi::FramebufferInfo info;
		info.addColorFormat(Lights2DNormalsFormat);
		info.setDepthFormat(SceneDepthFormat);
		return info;
	}

	// The framebuffer the 2D lights accumulate into (the Lights2D pipeline, additive): Lights2DLight alone.
	[[nodiscard]] inline nvrhi::FramebufferInfo GetLights2DLightFramebufferInfo()
	{
		nvrhi::FramebufferInfo info;
		info.addColorFormat(Lights2DLightFormat);
		return info;
	}

}
