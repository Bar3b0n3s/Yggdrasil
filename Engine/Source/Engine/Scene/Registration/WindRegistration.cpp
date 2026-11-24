#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

// The Wind category (M46; Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3F; ADR 0060's phase-8
// ledger: a category of its own after RenderDistance, before M47's Water): the Wind component, the scene's wind, which the
// renderer's wind displacement and Wind:Sample read (Scene/Wind.h).

namespace Engine {

	namespace Utils {

		// The strongest wind and gust, in m/s (a gale is about 20, a hurricane above 33).
		constexpr double MaxWindSpeed = 100.0;
		// The most gusts per second.
		constexpr double MaxWindGustFrequency = 10.0;

		// A wind's Direction names a horizontal direction: the length of its X and Z, in double so that no finite vector
		// overflows, is not zero. A write of a Direction without a horizontal part is refused on every write path (as a value
		// outside a field's range is), M42's Normal treatment.
		static void ValidateWind(const WindComponent& wind, ValidationContext& context)
		{
			const glm::dvec2 horizontal(wind.Direction.x, wind.Direction.z);
			if (!(glm::dot(horizontal, horizontal) > 0.0))
				context.Error("Direction", "must have a horizontal part (X or Z not zero): the wind blows along the ground");
		}

		static void MakeWindValid(WindComponent& wind, Random& /*random*/)
		{
			const glm::dvec2 horizontal(wind.Direction.x, wind.Direction.z);
			if (!(glm::dot(horizontal, horizontal) > 0.0))
				wind.Direction = glm::vec3(1.0f, 0.0f, 0.0f);
		}

	}

	void RegisterWindComponents(TypeRegistry& registry)
	{
		RegisterComponent<WindComponent>(registry, "Wind",
			"The scene's wind (one per scene): it bends, sways and flutters the meshes whose material has wind enabled, and scripts read it with "
			"Wind:Sample.")
			.Category("Wind")
			.Version(1)
			.Flags(ComponentFlags::UniquePerScene)
			.Field("Direction", &WindComponent::Direction,
				"Where the wind blows, in world axes; only its horizontal part (X and Z) counts, normalized, and it must not be zero.")
			.Field("Strength", &WindComponent::Strength, "The steady wind speed, in metres per second; a gale is about 20.",
				{ .Min = 0.0, .Max = Utils::MaxWindSpeed, .Unit = "m/s" })
			.Field("GustStrength", &WindComponent::GustStrength, "The speed gusts add to Strength at their peak, in metres per second.",
				{ .Min = 0.0, .Max = Utils::MaxWindSpeed, .Unit = "m/s" })
			.Field("GustFrequency", &WindComponent::GustFrequency,
				"How many gusts pass a point each second; they travel with the wind, so their wavelength is Strength / GustFrequency. 0 holds the "
				"gusts still.",
				{ .Min = 0.0, .Max = Utils::MaxWindGustFrequency, .Unit = "Hz" })
			.Field("Turbulence", &WindComponent::Turbulence,
				"How much the gusts vary across positions and turn the wind sideways: 0 blows straight and in step everywhere, 1 is most unsteady.",
				{ .Min = 0.0, .Max = 1.0 })
			.Validate(&Utils::ValidateWind)
			.Generate(&Utils::MakeWindValid);
	}

}
