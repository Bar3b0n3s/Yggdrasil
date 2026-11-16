#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

// The MaterialParameters category (M31; Docs/Decisions/0046-m31-material-parameters.md decision 1; ADR 0042's phase-5
// ledger: a category of its own after Terrain, in landing order): the MaterialParameters component, a MeshRenderer's
// per-entity material overrides, which render extraction resolves for the renderer (Scene/RenderExtraction.h).

namespace Engine {

	namespace Utils {

		// A linear colour channel's range (ADR 0006: colours are limited to [0, 1]; HDR comes from a strength).
		[[nodiscard]] static FieldMeta MaterialParametersColorRange()
		{
			FieldMeta meta;
			meta.Min = 0.0;
			meta.Max = 1.0;
			return meta;
		}

	}

	void RegisterMaterialParametersComponents(TypeRegistry& registry)
	{
		RegisterComponent<MaterialParametersComponent>(registry, "MaterialParameters",
			"Per-entity overrides on top of every material of the entity's MeshRenderer: a tint, an added glow, a texture offset and scroll, a "
			"dissolve with a glowing edge, and a dithered fade; they keep instancing and work on skinned meshes.")
			.Category("MaterialParameters")
			.Version(1)
			.Requires<MeshRendererComponent>()
			.ColorField("Tint", &MaterialParametersComponent::Tint, "The linear colour that multiplies the base colour of every material.",
				Utils::MaterialParametersColorRange())
			.ColorField("Emissive", &MaterialParametersComponent::Emissive,
				"The linear colour of the glow added to every material's emission, scaled by EmissiveStrength.", Utils::MaterialParametersColorRange())
			.Field("EmissiveStrength", &MaterialParametersComponent::EmissiveStrength,
				"How bright the added glow is: Emissive times this, in the units of a material's emission; 0 adds none.", { .Min = 0.0, .Max = 1000.0 })
			.Field("UVOffset", &MaterialParametersComponent::UVOffset,
				"Added to every map's texture coordinates after the material's own scale and offset; whole units repeat the texture.",
				{ .Min = -1000.0, .Max = 1000.0 })
			.Field("UVScroll", &MaterialParametersComponent::UVScroll,
				"How fast the texture coordinates move, in texture units per second of simulation time, added to UVOffset; it advances in play only.",
				{ .Min = -1000.0, .Max = 1000.0, .Unit = "1/s" })
			.Field("DissolveAmount", &MaterialParametersComponent::DissolveAmount,
				"How much of the surface has dissolved, from 0 (none) to 1 (all), by a built-in noise over the first texture coordinates; shadows "
				"dissolve too.",
				{ .Min = 0.0, .Max = 1.0 })
			.ColorField("DissolveEdgeColor", &MaterialParametersComponent::DissolveEdgeColor,
				"The linear colour of the glow along the dissolve's edge, scaled by DissolveEdgeStrength.", Utils::MaterialParametersColorRange())
			.Field("DissolveEdgeStrength", &MaterialParametersComponent::DissolveEdgeStrength,
				"How bright the dissolve's edge glows: DissolveEdgeColor times this, in the units of a material's emission.", { .Min = 0.0, .Max = 1000.0 })
			.Field("DissolveEdgeWidth", &MaterialParametersComponent::DissolveEdgeWidth,
				"How wide the glowing edge is, as the share of the noise just above DissolveAmount that glows; 0 draws no edge.", { .Min = 0.0, .Max = 1.0 })
			.Field("DissolveNoiseScale", &MaterialParametersComponent::DissolveNoiseScale,
				"How many times the dissolve noise repeats per unit of the mesh's first texture coordinates.", { .Min = 0.01, .Max = 100.0 })
			.Field("Opacity", &MaterialParametersComponent::Opacity,
				"The fade, from 1 (solid) to 0 (gone): a dithered see-through pattern on Opaque and Mask materials, which keep their depth, sorting "
				"and shadow, and a factor of a Blend material's alpha.",
				{ .Min = 0.0, .Max = 1.0 });
	}

}
