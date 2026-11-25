#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// The view's fog (M32, Docs/Decisions/0047-m32-fog-ssr.md decisions 3 and 5; Renderer/Fog.h, Common/Fog.slang): one
	// constant buffer per view, written by the scene renderer with the other per-view constants from the snapshot's RenderFog
	// (MakeFogConstants), and read by the forward passes (b5 of the forward view layout), the terrain, the skybox (b5), the SSR
	// composite (b5), the QuadBatcher's Hdr and HdrLit pipelines (b1) and, from M33's merge, the Particles program (b1;
	// Docs/Decisions/0048-m33-particle-look.md decision 10). Self-contained: with a world position, the camera's position and
	// forward axis here give the point's view depth and view ray, so a pass needs no other constants to fog. Without fog (no
	// Fog component, a view other than Lit, the wireframe) Enabled is 0 and nothing reads the rest.
	struct FogConstants
	{
		// The fog's colour before the sun glow: Fog.Color tinted by the environment's ambient light (lerp(1, ambient,
		// AmbientTint)), linear; and the distance fog's density per metre of view depth.
		Float3 Color;
		float Density = 0.0f;
		// The camera's world position and the view depth where fog begins.
		Float3 CameraPosition;
		float StartDistance = 0.0f;
		// The camera's forward axis (its -Z) in world space, unit; and the most fog a point receives, in [0, 1].
		Float3 ViewForward;
		float MaxOpacity = 0.0f;
		// The direction toward the glow's light, unit (an enabled Sky's Sun, else the light list's first directional light, in
		// canonical order: GetSkyGlowLight, Renderer/SkyAtmosphere.h); and the glow's exponent.
		Float3 SunDirection;
		float SunGlowExponent = 1.0f;
		// The glow's radiance at its centre: Fog.SunGlow times that light's Color times its Intensity; zero without a
		// directional light. And the height fog's density per metre at BaseHeight.
		Float3 SunColor;
		float HeightDensity = 0.0f;
		float HeightFalloff = 1.0f; // per metre of height, > 0
		float BaseHeight = 0.0f;    // world metres
		uint32_t Enabled = 0;       // 1: the view is fogged
		uint32_t FogSkybox = 0;     // 1: the skybox is fogged too
		uint32_t Orthographic = 0;  // 1: an orthographic camera (each pixel's view ray is ViewForward)
		uint32_t Padding0 = 0;
		uint32_t Padding1 = 0;
		uint32_t Padding2 = 0;
		// M32's milestone review: the camera's clear colour (linear, CameraData::ClearColor), and whether it is the view's
		// sky (1: no skybox draws over the cleared pixels, set by the renderer). With FogSkybox the skybox pass then fogs the
		// cleared pixels as the sky, so a void below a bridge and the world's edge fade into the fog as a skybox does.
		Float3 ClearColor;
		uint32_t SkyIsClearColor = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(FogConstants) == 128, "FogConstants must match its 128-byte constant-buffer layout");
	static_assert(offsetof(FogConstants, Density) == 12 && offsetof(FogConstants, CameraPosition) == 16 && offsetof(FogConstants, StartDistance) == 28
			&& offsetof(FogConstants, ViewForward) == 32 && offsetof(FogConstants, MaxOpacity) == 44 && offsetof(FogConstants, SunDirection) == 48
			&& offsetof(FogConstants, SunGlowExponent) == 60 && offsetof(FogConstants, SunColor) == 64 && offsetof(FogConstants, HeightDensity) == 76
			&& offsetof(FogConstants, HeightFalloff) == 80 && offsetof(FogConstants, Enabled) == 88 && offsetof(FogConstants, Orthographic) == 96
			&& offsetof(FogConstants, ClearColor) == 112 && offsetof(FogConstants, SkyIsClearColor) == 124,
		"FogConstants members must sit where the constant-buffer layout puts them");

}
#endif
