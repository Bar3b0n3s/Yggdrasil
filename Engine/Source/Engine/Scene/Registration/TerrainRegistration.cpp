#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Scene/ComponentRegistration.h"

#include <format>

// The Terrain category (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 4; ADR 0028's phase-3 ledger: after
// Probes): the Terrain, a heightfield drawn with splatted layers, and the TerrainCollider, its static height field in the 3D
// physics world (the collision split from the drawing, as TilemapCollider is from TilemapRenderer). Registered by the
// contract; no scene of FeatureTest or of the audited projects holds them.

namespace Engine {

	namespace Utils {

		// A terrain holds at most MaxTerrainLayers layers, one per splat map channel.
		static void ValidateTerrain(const TerrainComponent& terrain, ValidationContext& context)
		{
			if (terrain.Layers.size() > MaxTerrainLayers)
				context.Error("Layers", std::format("must hold at most {} layers, not {}", MaxTerrainLayers, terrain.Layers.size()));
		}

		static void MakeTerrainValid(TerrainComponent& terrain, Random& /*random*/)
		{
			if (terrain.Layers.size() > MaxTerrainLayers)
				terrain.Layers.resize(MaxTerrainLayers);
		}

	}

	void RegisterTerrainComponents(TypeRegistry& registry)
	{
		registry.Struct<TerrainLayer>("TerrainLayer", "One material layer of a Terrain, tiled over its surface where its splat map channel weighs it.")
			.Field("BaseColorMap", &TerrainLayer::BaseColorMap, "The layer's colour texture; null is white, so the layer is its Tint.")
			.Field("NormalMap", &TerrainLayer::NormalMap, "The layer's tangent-space normal map; null is flat.")
			.ColorField("Tint", &TerrainLayer::Tint, "The linear colour that multiplies the layer's base colour.", { .Min = 0.0, .Max = 1.0 })
			.Field("Tiling", &TerrainLayer::Tiling, "The metres one repeat of the layer's maps covers in the terrain's local XZ plane.",
				{ .Min = 0.001, .Max = 1000.0, .Unit = "m" })
			.Field("Roughness", &TerrainLayer::Roughness, "The layer's perceptual roughness.", { .Min = 0.0, .Max = 1.0 })
			.Field("Metallic", &TerrainLayer::Metallic, "The layer's metalness.", { .Min = 0.0, .Max = 1.0 });

		RegisterComponent<TerrainComponent>(registry, "Terrain",
			"A heightfield laid out under the entity and drawn with up to four material layers blended by a splat map; sculpt and paint it with "
			"terrain.edit.")
			.Category("Terrain")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("Heightfield", &TerrainComponent::Heightfield, "The heightfield whose samples shape the terrain; null draws nothing.")
			.Field("Size", &TerrainComponent::Size, "The terrain's extent along its local X and Z axes from the entity's origin, in metres.",
				{ .Min = 0.01, .Max = static_cast<double>(MaxTerrainSize), .Unit = "m" })
			.Field("HeightScale", &TerrainComponent::HeightScale, "The height of the largest heightfield sample above the entity's origin, in metres.",
				{ .Min = 0.001, .Max = static_cast<double>(MaxTerrainSize), .Unit = "m" })
			.Field("Layers", &TerrainComponent::Layers,
				"At most four material layers, weighed by the splat map's red, green, blue and alpha channels; empty draws one white layer.")
			.Field("SplatMap", &TerrainComponent::SplatMap,
				"The texture whose channels weigh the layers per point (a data texture, imported with Usage Linear and Compression None); null "
				"draws the first layer alone.")
			.Field("ReceiveShadows", &TerrainComponent::ReceiveShadows, "Whether shadows fall on the terrain.")
			.Field("LodBias", &TerrainComponent::LodBias, "Scales the distances at which the terrain's chunks drop detail; above 1 keeps detail farther.",
				{ .Min = 0.25, .Max = 4.0 })
			.Validate(&Utils::ValidateTerrain)
			.Generate(&Utils::MakeTerrainValid);

		RegisterComponent<TerrainColliderComponent>(registry, "TerrainCollider",
			"The entity's Terrain as a static height field in the physics world, colliding with exactly the surface that is drawn.")
			.Category("Terrain")
			.Version(1)
			.Requires<TerrainComponent>()
			.Field("Layer", &TerrainColliderComponent::Layer,
				"Physics layer name from project settings, of the terrain's own static body (unused when the terrain joins another body: a "
				"RigidBody on or above the entity, or a collider-only ancestor's).")
			.Field("Friction", &TerrainColliderComponent::Friction,
				"The friction coefficient of the terrain's own static body (unused when the terrain joins another body, as Layer); a contact uses "
				"the geometric mean of both bodies' values.",
				{ .Min = 0.0 })
			.Field("Restitution", &TerrainColliderComponent::Restitution,
				"The bounciness of the terrain's own static body, from 0 to 1 (unused when the terrain joins another body, as Layer); a contact "
				"uses the larger of both bodies' values.",
				{ .Min = 0.0, .Max = 1.0 });
	}

}
