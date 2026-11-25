#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

// The RenderDistance category (M46; Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3C; ADR 0060's
// phase-8 ledger: a category of its own after M44's Vehicles, then Wind's, in train order): the RenderDistance component, a
// MeshRenderer's cull distance and level-of-detail choice, which render extraction carries to the renderer
// (Scene/RenderExtraction.h, Renderer/MeshLod.h).

namespace Engine {

	namespace Utils {

		// The farthest cull distance, in metres (100 km: beyond any camera's FarClip in practice).
		constexpr double MaxRenderCullDistance = 100000.0;
		// The level bias's range: from switching a hundred times earlier to a hundred times later.
		constexpr double MinRenderLodBias = 0.01;
		constexpr double MaxRenderLodBias = 100.0;
		// The widest fade band, as a share of its switch size or cull distance.
		constexpr double MaxRenderFadeWidth = 0.5;

	}

	void RegisterRenderDistanceComponents(TypeRegistry& registry)
	{
		RegisterComponent<RenderDistanceComponent>(registry, "RenderDistance",
			"How far and in which level of detail the MeshRenderer on this entity draws: a cull distance, a bias on its mesh's level switches, the "
			"shadow's coarser level and the width of the cross-fades. A MeshRenderer without one takes the defaults.")
			.Category("RenderDistance")
			.Version(1)
			.Requires<MeshRendererComponent>()
			.Field("CullDistance", &RenderDistanceComponent::CullDistance,
				"Beyond this distance from a view's camera, in metres, the mesh draws and casts nothing in that view, fading out across the last "
				"FadeWidth of it; 0 leaves only the camera's FarClip.",
				{ .Min = 0.0, .Max = Utils::MaxRenderCullDistance, .Unit = "m" })
			.Field("LodBias", &RenderDistanceComponent::LodBias,
				"Multiplies the mesh's screen size before its levels' switch sizes compare it: 2 keeps each finer level until half its switch size, "
				"0.5 switches to coarser levels twice as early.",
				{ .Min = Utils::MinRenderLodBias, .Max = Utils::MaxRenderLodBias })
			.Field("ShadowLodOffset", &RenderDistanceComponent::ShadowLodOffset,
				"How many levels coarser than its view's level the mesh's shadow draws, clamped to its last level; 0 casts from the drawn level.",
				{ .Min = 0.0, .Max = 4.0 })
			.Field("FadeWidth", &RenderDistanceComponent::FadeWidth,
				"The band around each level switch, and before CullDistance, as a share of the switch size or the distance, across which two levels "
				"cross-fade by dither (or the last fades out); 0 switches sharply.",
				{ .Min = 0.0, .Max = Utils::MaxRenderFadeWidth });
	}

}
