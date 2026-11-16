#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

// The Fog category (M32, Docs/Decisions/0047-m32-fog-ssr.md decision 2; ADR 0042's phase-5 ledger: after Terrain, in
// landing order): the Fog component, the scene's distance and height fog, which every Lit view's forward shading, terrain,
// skybox, SSR composite and 3D sprites apply (Renderer/Fog.h, Shared/FogConstants.h).

namespace Engine {

	namespace Utils {

		// The densest fog, per metre: at 10 /m a metre of fog is already 99.995 % opaque.
		constexpr double MaxFogDensity = 10.0;
		// The farthest fog start, in metres.
		constexpr double MaxFogStartDistance = 100000.0;
		// The height falloff's range, per metre: from fog that thins over a kilometre to fog that ends within centimetres.
		constexpr double MinFogHeightFalloff = 0.001;
		constexpr double MaxFogHeightFalloff = 100.0;
		// The strongest sun glow (a multiple of the light's colour times its intensity) and the glow exponent's range.
		constexpr double MaxFogSunGlow = 100.0;
		constexpr double MinFogSunGlowExponent = 1.0;
		constexpr double MaxFogSunGlowExponent = 1000.0;

		// A colour channel range: linear colours in [0, 1].
		static FieldMeta FogColorRange()
		{
			FieldMeta meta;
			meta.Min = 0.0;
			meta.Max = 1.0;
			return meta;
		}

	}

	void RegisterFogComponents(TypeRegistry& registry)
	{
		RegisterComponent<FogComponent>(registry, "Fog",
			"The scene's distance and height fog, drawn over every surface, terrain, 3D sprite and, optionally, the skybox by view depth (one per "
			"scene).")
			.Category("Fog")
			.Version(1)
			.Flags(ComponentFlags::UniquePerScene)
			.ColorField("Color", &FogComponent::Color, "The fog's linear colour.", Utils::FogColorRange())
			.Field("AmbientTint", &FogComponent::AmbientTint,
				"How much the environment's ambient light tints Color: 0 keeps Color, 1 multiplies it by the ambient light.", { .Min = 0.0, .Max = 1.0 })
			.Field("Density", &FogComponent::Density, "The distance fog's density: the share of light it absorbs per metre of view depth, as an exponent.",
				{ .Min = 0.0, .Max = Utils::MaxFogDensity, .Unit = "1/m" })
			.Field("StartDistance", &FogComponent::StartDistance, "The view depth, in metres, where fog begins.",
				{ .Min = 0.0, .Max = Utils::MaxFogStartDistance, .Unit = "m" })
			.Field("MaxOpacity", &FogComponent::MaxOpacity, "The most fog any point receives, from none (0) to fully fogged (1).", { .Min = 0.0, .Max = 1.0 })
			.Field("HeightDensity", &FogComponent::HeightDensity, "The height fog's density per metre at BaseHeight; 0 has no height fog.",
				{ .Min = 0.0, .Max = Utils::MaxFogDensity, .Unit = "1/m" })
			.Field("HeightFalloff", &FogComponent::HeightFalloff,
				"How fast the height fog thins above BaseHeight and thickens below it, per metre, as an exponent.",
				{ .Min = Utils::MinFogHeightFalloff, .Max = Utils::MaxFogHeightFalloff, .Unit = "1/m" })
			.Field("BaseHeight", &FogComponent::BaseHeight, "The world height, in metres, where the height fog has HeightDensity.", { .Unit = "m" })
			.Field("SunGlow", &FogComponent::SunGlow,
				"How strongly the fog glows toward the first directional light, as a multiple of that light's colour times its intensity; 0 has no glow.",
				{ .Min = 0.0, .Max = Utils::MaxFogSunGlow })
			.Field("SunGlowExponent", &FogComponent::SunGlowExponent, "How tightly the sun glow gathers around the light's direction: higher is tighter.",
				{ .Min = Utils::MinFogSunGlowExponent, .Max = Utils::MaxFogSunGlowExponent })
			.Field("FogSkybox", &FogComponent::FogSkybox, "Whether the skybox is fogged too, as if it lay infinitely far away.");
	}

}
