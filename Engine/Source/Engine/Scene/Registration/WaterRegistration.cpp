#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Core/Random.h"
#include "Engine/Physics/WaterWaves.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Scene/ComponentRegistration.h"

#include <format>

// The Water category (M47, Docs/Decisions/0065-m47-water.md decision 3; phase 8's ledger: the twentieth category, after M46's
// Wind): the Water, a body of water whose waves are a pure function of position and the world's time (Physics/WaterWaves.h),
// and the Buoyancy, which floats the Dynamic body of its entity on it (Architecture §9.12). Ranges keep every value the wave
// function and Jolt receive finite and meaningful; what spans fields or entities (a pitched Water, a second Ocean, waves that
// would fold, a Buoyancy without a Dynamic body) is a validation warning (Scene/WaterValidation.h), never a write error.

namespace Engine {

	static_assert(MaxWaterWaves == MaxWaterSurfaceWaves, "the component's and the wave function's wave counts are one bound");

	namespace Utils {

		// A water holds at most MaxWaterWaves waves.
		static void ValidateWater(const WaterComponent& water, ValidationContext& context)
		{
			if (water.Waves.size() > MaxWaterWaves)
				context.Error("Waves", std::format("must hold at most {} waves, not {}", MaxWaterWaves, water.Waves.size()));
		}

		static void MakeWaterValid(WaterComponent& water, Random& /*random*/)
		{
			if (water.Waves.size() > MaxWaterWaves)
				water.Waves.resize(MaxWaterWaves);
		}

		// A colour channel range: linear colours in [0, 1].
		static FieldMeta WaterColorRange()
		{
			FieldMeta meta;
			meta.Min = 0.0;
			meta.Max = 1.0;
			return meta;
		}

	}

	void RegisterWaterComponents(TypeRegistry& registry)
	{
		registry.Enum<WaterShape>("WaterShape", "What a Water covers.")
			.Entry(WaterShape::Ocean, "Ocean", "An unbounded sea: all of X and Z and everything below its surface; one a scene.")
			.Entry(WaterShape::Rectangle, "Rectangle",
				"A bounded water (a pool, a canal, a cave lake): Size along the entity's local X and Z centred on it, from Depth below its base "
				"to Headroom above it.");

		registry.Struct<WaterWave>("WaterWave", "One Gerstner wave of a Water, in the water's local frame.")
			.Field("Direction", &WaterWave::Direction,
				"The direction the wave travels, in degrees about +Y in the water's local XZ plane: 0 along local +X, 90 along local -Z.",
				{ .Unit = "deg" })
			.Field("Wavelength", &WaterWave::Wavelength, "The distance from crest to crest, in metres.",
				{ .Min = MinWaterWavelength, .Max = MaxWaterWavelength, .Unit = "m" })
			.Field("Amplitude", &WaterWave::Amplitude, "The height of a crest above the base, in metres, before the water's WaveScale.",
				{ .Min = 0.0, .Max = MaxWaterAmplitude, .Unit = "m" })
			.Field("Steepness", &WaterWave::Steepness,
				"Gerstner's steepness Q: 0 is a sine wave, 1 the sharpest crest; scaled down with the others when their sum would fold the surface "
				"(WATER_WAVES_TOO_STEEP).",
				{ .Min = 0.0, .Max = 1.0 })
			.Field("Speed", &WaterWave::Speed, "The speed of the crests, in metres per second; 0 takes deep water's speed for the wavelength.",
				{ .Min = 0.0, .Max = MaxWaterWaveSpeed, .Unit = "m/s" })
			.Field("Phase", &WaterWave::Phase, "The wave's phase at time 0, in degrees.", { .Unit = "deg" });

		RegisterComponent<WaterComponent>(registry, "Water",
			"A body of water at the entity's world height: an unbounded Ocean or a bounded Rectangle, with Gerstner waves of the world's time "
			"that scripts, the physics and the renderer sample alike.")
			.Category("Water")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("Shape", &WaterComponent::Shape,
				"Ocean (unbounded, one a scene) or Rectangle (a box; it masks the Ocean inside its volume, so a cave lake below the sea is dry above "
				"its surface).")
			.Field("Size", &WaterComponent::Size, "A Rectangle's extent along the entity's local X and Z axes, centred on it, in local units (its scale applies).",
				{ .Min = static_cast<double>(MinWaterSize), .Max = static_cast<double>(MaxWaterExtent), .Unit = "m" })
			.Field("Depth", &WaterComponent::Depth, "How far a Rectangle's volume reaches below its base, in metres.",
				{ .Min = 0.0, .Max = static_cast<double>(MaxWaterExtent), .Unit = "m" })
			.Field("Headroom", &WaterComponent::Headroom,
				"How far a Rectangle's volume reaches above its base, in metres; reach a cave's ceiling so the Ocean is masked in the cave.",
				{ .Min = 0.0, .Max = static_cast<double>(MaxWaterExtent), .Unit = "m" })
			.Field("Waves", &WaterComponent::Waves, "At most eight Gerstner waves summed over the base; empty is a calm surface.")
			.Field("WaveScale", &WaterComponent::WaveScale, "Multiplies every wave's amplitude: 0 is calm, above 1 a storm.",
				{ .Min = 0.0, .Max = MaxWaterWaveScale })
			.Field("Current", &WaterComponent::Current,
				"The water's horizontal flow along its local X and Z axes, in metres per second; it carries floating bodies and moves no wave.",
				{ .Min = -MaxWaterCurrent, .Max = MaxWaterCurrent, .Unit = "m/s" })
			.Field("Density", &WaterComponent::Density, "The fluid's density, which Buoyancy floats bodies against, in kilograms per cubic metre.",
				{ .Min = static_cast<double>(MinWaterDensity), .Max = static_cast<double>(MaxWaterDensity), .Unit = "kg/m^3" })
			.ColorField("ShallowColor", &WaterComponent::ShallowColor, "The linear colour of shallow water, over the bottom it shows.",
				Utils::WaterColorRange())
			.ColorField("DeepColor", &WaterComponent::DeepColor, "The linear colour deep water absorbs toward.", Utils::WaterColorRange())
			.Field("Clarity", &WaterComponent::Clarity, "The metres of water column over which the bottom fades toward DeepColor.",
				{ .Min = 0.01, .Max = 1000.0, .Unit = "m" })
			.Field("Roughness", &WaterComponent::Roughness, "The surface's perceptual roughness: low is a sharp sun glint and clear reflections.",
				{ .Min = 0.02, .Max = 1.0 })
			.Field("RefractionStrength", &WaterComponent::RefractionStrength, "How far the waves bend the view of what lies under the surface.",
				{ .Min = 0.0, .Max = 1.0 })
			.Field("FoamAmount", &WaterComponent::FoamAmount, "How much foam the crests and the shallows show: 0 none.", { .Min = 0.0, .Max = 1.0 })
			.Field("ShoreFoamDepth", &WaterComponent::ShoreFoamDepth,
				"The water column, in metres, under which foam gathers where the surface meets the ground or an object.",
				{ .Min = 0.0, .Max = 10.0, .Unit = "m" })
			.Field("DetailNormalStrength", &WaterComponent::DetailNormalStrength, "How strongly the small-scale detail normals ripple the surface.",
				{ .Min = 0.0, .Max = 1.0 })
			.Field("DetailScale", &WaterComponent::DetailScale, "The metres one repeat of the detail normal map covers.",
				{ .Min = 0.1, .Max = 1000.0, .Unit = "m" })
			.Field("DetailDrift", &WaterComponent::DetailDrift,
				"How fast the detail normals drift along the water's local X and Z axes, in metres per second.",
				{ .Min = -100.0, .Max = 100.0, .Unit = "m/s" })
			.ColorField("UnderwaterColor", &WaterComponent::UnderwaterColor, "The linear colour of the fog a camera under the surface sees.",
				Utils::WaterColorRange())
			.Field("UnderwaterVisibility", &WaterComponent::UnderwaterVisibility, "How far a camera under the surface sees, in metres.",
				{ .Min = 0.1, .Max = 10000.0, .Unit = "m" })
			.Field("NormalMap", &WaterComponent::NormalMap, "The detail normal map; null takes the built-in WaterNormal.")
			.Field("FoamTexture", &WaterComponent::FoamTexture, "The foam's texture; null takes the built-in WaterFoam.")
			.Validate(&Utils::ValidateWater)
			.Generate(&Utils::MakeWaterValid);

		RegisterComponent<BuoyancyComponent>(registry, "Buoyancy",
			"Floats the Dynamic body of the entity's RigidBody on water: each solid collider is submerged against the surface under it, with "
			"the water's density and drag.")
			.Category("Water")
			.Version(1)
			.Requires<RigidBodyComponent>()
			.Field("Water", &BuoyancyComponent::Water,
				"The entity whose Water floats the body (a Rectangle only within its footprint and volume); null takes, per collider, the water "
				"that holds its centre.")
			.Field("BuoyancyScale", &BuoyancyComponent::BuoyancyScale, "Multiplies the buoyant force: 1 is physical, 0 none.",
				{ .Min = 0.0, .Max = static_cast<double>(MaxBuoyancyScale) })
			.Field("LinearDrag", &BuoyancyComponent::LinearDrag, "The water's drag on the body's motion relative to the water (Jolt's linear drag coefficient).",
				{ .Min = 0.0, .Max = static_cast<double>(MaxBuoyancyDrag) })
			.Field("AngularDrag", &BuoyancyComponent::AngularDrag, "The water's drag on the body's turning (Jolt's angular drag coefficient).",
				{ .Min = 0.0, .Max = static_cast<double>(MaxBuoyancyDrag) });
	}

}
