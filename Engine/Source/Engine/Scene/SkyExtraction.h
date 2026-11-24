#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/RenderSnapshot.h"

// The sky's render extraction (M45, Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3): the scene's Sky component into
// RenderSnapshot::Sky, called once by ExtractRenderSnapshot after its canonical loop, beside the fog (Scene/RenderExtraction.h).
//   - The item: the scene's one effectively enabled Sky (UniquePerScene; should a scene hold several, the smallest entity UUID
//     among them), its fields, Enabled, the request's SimulationTime and its Entity; Enabled false (RenderSky's defaults) when
//     the scene has none, so a scene without a Sky extracts exactly as before.
//   - The Moon's direction: a Moon that names an effectively enabled entity other than the Sun's gives HasMoon and
//     MoonDirection, its rendered world matrix's local +Z (ComputeRenderedWorldMatrix, at the request's alpha); a Moon light
//     then takes its light's own direction (ResolveSkyLights).
//   - The roles: ResolveSkyLights over the extracted lights, then ApplySkyLightDriving (RenderSnapshot.h), so every consumer
//     of the light list sees the driven colours.
// Every view extracts it, an edit scene's included (at SimulationTime 0). Pure function of the scene and the request; IEEE
// exact operations and Core/DetMath only (§4.12); main thread.

namespace Engine {

	class Scene;
	struct RenderExtractionRequest;

	// Fills `snapshot.Sky` from `scene`, then resolves and drives its roles against `snapshot.Lights` (which the caller has
	// extracted) and `snapshot.Camera`.
	void ExtractRenderSky(const Scene& scene, const RenderExtractionRequest& request, RenderSnapshot& snapshot);

}
