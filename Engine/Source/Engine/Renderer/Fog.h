#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/FogConstants.h"

#include <span>

// The view's fog (M32, Docs/Decisions/0047-m32-fog-ssr.md decisions 3 and 5; Architecture §8.6): the scene's Fog component,
// extracted into RenderSnapshot::Fog, drawn by every pass that shades the world in a Lit view: the forward passes' opaque and
// transparent shading (Passes/Scene.slang) and the terrains' (Passes/Terrain.slang), the skybox with FogSkybox
// (Passes/Skybox.slang), the SSR composite, whose addition is weighed by the point's remaining transparency (Passes/Ssr.slang),
// and the Sprites stage's world-space quads (Passes/Quad.slang: blended quads fogged, additive ones faded). The model is
// Common/Fog.slang's: distance is view depth, so an orthographic camera fogs as evenly as a perspective one. A view without
// fog (no Fog component, a view other than Lit, the wireframe, no camera) has FogConstants::Enabled 0, and every pass then
// renders exactly as before M32. Particles are fogged by M33's particle program; World-space UI and world text are not
// fogged (Docs/Backlog.md §6). lighting.bake's probe captures turn the fog off (Renderer/ProbeCapture.h).

namespace Engine {

	// The view's FogConstants (Shared/FogConstants.h) for `fog`, seen by `camera`, with the first directional light of
	// `lights` (in their order) as the sun and `environment` (the view's EnvironmentConstants) as the ambient light:
	//   - Enabled: `fogged` (the view applies fog: a Lit view with a camera, outside the wireframe) and fog.Enabled; every other
	//     member is filled either way;
	//   - Color: fog.Color times lerp(1, ambient, AmbientTint), ambient being the environment's mean SH9 irradiance / pi
	//     (IrradianceSH9[0] times the SH basis' constant term, 0.282095) with an environment map, its AmbientColor without;
	//   - CameraPosition and ViewForward: camera.Position and the camera's -Z axis in world space (from camera.View, normalized;
	//     -Z when the matrix holds none); Orthographic for an orthographic projection;
	//   - SunDirection and SunColor: toward the first directional light (its -Direction, normalized) and fog.SunGlow times its
	//     Color times its Intensity; +Y and zero without a directional light with a finite, non-zero direction;
	//   - the densities, distances, opacity, falloff and exponent made usable: a non-finite value takes RenderFog's default and
	//     each is clamped to its Fog component range (Density and HeightDensity [0, 10] per metre, StartDistance [0, 100000] m,
	//     MaxOpacity and AmbientTint [0, 1], HeightFalloff [0.001, 100] per metre, SunGlow [0, 100], SunGlowExponent [1, 1000]),
	//     BaseHeight any finite value, and each colour channel finite and at least 0;
	//   - ClearColor: camera.ClearColor made usable the same way; SkyIsClearColor 0, which the renderer sets to 1 for a view
	//     whose cleared pixels no skybox draws over (M32's milestone review).
	// Pure, thread-safe.
	[[nodiscard]] FogConstants MakeFogConstants(const RenderFog& fog, const CameraData& camera, std::span<const LightData> lights,
		const EnvironmentConstants& environment, bool fogged);

}
