#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "PlanarReflection", version 1, category Probes, after ReflectionProbe (M42,
	// Docs/Decisions/0059-m42-planar-reflections.md decision 3): makes the flat surface its entity's MeshRenderer draws a true
	// mirror. Requires Transform; script-visible through its generated proxy.
	//   - The plane: through the point Normal * Offset of the entity's local space, facing Normal (the reflecting side), in
	//     the entity's local axes. The renderer takes it into the world through the entity's rendered world matrix (the
	//     normal through its inverse transpose, so a non-uniform scale is handled) every frame, so a mirror that moves or turns
	//     is followed. The default is the built-in Quad's face, +Z through the origin. Normal is normalized when used and must
	//     not be zero (a write of a zero Normal is refused); Offset is in local units.
	//   - Rendering: each view renders the reflection of what it sees in the plane from its camera mirrored in the plane,
	//     clipped at the plane, at ResolutionScale of its scene resolution, through a lighter path, and the entity's own
	//     MeshRenderer samples it in place of its probe and SSR reflections, blurred by the material's roughness
	//     (Renderer/PlanarReflections.h; Architecture §8.3). Only mirrors in view and facing the camera render, at most
	//     MaxPlanarReflectionsPerView a view, the largest on screen first; the others fall back to the probes and SSR.
	//   - Extraction: an effectively enabled entity whose MeshRenderer is Visible with a mesh gives a PlanarReflectionItem
	//     (Renderer/RenderSnapshot.h) in every view, edit scenes included; without one the component does nothing, which
	//     RENDER_REFLECTION_INVALID reports (Scene/PlanarReflectionValidation.h).
	// Presentation only: the component is component data (serialized and in the state hash of a scene that holds it), and
	// nothing the renderer does with it is hashed.
	struct PlanarReflectionComponent
	{
		glm::vec3 Normal = glm::vec3(0.0f, 0.0f, 1.0f); // the reflecting side, local axes; normalized when used, never zero
		float Offset = 0.0f;                            // local units along Normal, [-10000, 10000]
		float ResolutionScale = 0.5f;                   // of the view's scene resolution, [0.25, 1]
	};

}
