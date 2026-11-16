#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "MaterialParameters", version 1, category MaterialParameters (M31; Docs/Decisions/0046-m31-material-
	// parameters.md decisions 1 to 3): per-entity overrides applied on top of every material of the entity's MeshRenderer,
	// which it requires, so a script, automation or the inspector can tint one entity, make it glow, shift or scroll its
	// texture coordinates, dissolve it or fade it out without a material asset per state. They are ordinary component data,
	// serialized and hashed like any component's; the renderer reads them only through render extraction
	// (Scene/RenderExtraction.h), which resolves them into MeshDrawItem::Parameters (Renderer/RenderSnapshot.h's
	// MeshMaterialParameters), so they are presentation: nothing in the simulation reads them. The defaults draw exactly as an
	// entity without the component. They reach every submesh's draws, a skinned mesh's included, and ride in the instance
	// records, so entities that differ only in them still share instanced draws; a dissolving or fading entity of an Opaque
	// material draws with the Mask pipelines instead (Renderer/RenderPrepare.h's SelectMeshAlphaMode).
	struct MaterialParametersComponent
	{
		glm::vec3 Tint = glm::vec3(1.0f);                          // linear, each in [0, 1]: multiplies the base colour's RGB
		glm::vec3 Emissive = glm::vec3(1.0f);                      // linear, each in [0, 1]: the colour of the added emission
		float EmissiveStrength = 0.0f;                             // >= 0: the added emission is Emissive times this (0 adds none)
		glm::vec2 UVOffset = glm::vec2(0.0f);                      // texture-coordinate units, added after each material's UV transform
		glm::vec2 UVScroll = glm::vec2(0.0f);                      // texture-coordinate units per second of simulation time
		float DissolveAmount = 0.0f;                               // [0, 1]: the share of the surface dissolved, by the noise
		glm::vec3 DissolveEdgeColor = glm::vec3(1.0f, 0.5f, 0.1f); // linear, each in [0, 1]: the glow along the edge
		float DissolveEdgeStrength = 2.0f;                         // >= 0: the edge's emission is DissolveEdgeColor times this
		float DissolveEdgeWidth = 0.05f;                           // [0, 1]: the share of the noise range above the amount that glows
		float DissolveNoiseScale = 1.0f;                           // [0.01, 100]: the noise's repeats per unit of the first texture coordinates
		float Opacity = 1.0f;                                      // [0, 1]: the fade (a dithered clip of an Opaque or Mask material)
	};

}
