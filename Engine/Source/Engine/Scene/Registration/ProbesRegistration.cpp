#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Asset/IrradianceVolumeData.h"
#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Scene/ComponentRegistration.h"

#include <algorithm>
#include <bit>
#include <format>

// The Probes category (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 4; ADR 0028's phase-3 ledger:
// after Particles): the IrradianceVolume and the ReflectionProbe, baked by lighting.bake and committed per scene, which the
// forward passes add as the indirect term of M23's light loop. Registered by the contract; no scene of FeatureTest or of
// the audited projects holds them. M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3) appends the
// PlanarReflection, the third reflection source: a mirror rendered from the camera mirrored in its plane.

namespace Engine {

	namespace Utils {

		// The largest probe intensity: the largest finite binary16 value, as an Environment's.
		constexpr double MaxProbeIntensity = 65504.0;
		// The largest blend distance and reflection probe box side, in metres.
		constexpr double MaxProbeBlendDistance = 1000.0;
		constexpr double MaxReflectionProbeSize = 10000.0;

		// A reflection probe's capture cube has a power-of-two face (its mips halve exactly).
		static void ValidateReflectionProbe(const ReflectionProbeComponent& probe, ValidationContext& context)
		{
			if (!std::has_single_bit(probe.Resolution))
				context.Error("Resolution", std::format("must be a power of two, not {}", probe.Resolution));
		}

		static void MakeReflectionProbeValid(ReflectionProbeComponent& probe, Random& /*random*/)
		{
			if (!std::has_single_bit(probe.Resolution))
				probe.Resolution = std::max(MinReflectionProbeResolution, std::bit_floor(probe.Resolution));
		}

		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): the farthest plane offset, in local units.
		constexpr double MaxPlanarReflectionOffset = 10000.0;

		// A planar reflection's Normal names a direction: its length, in double so that no finite vector overflows, is not
		// zero. A write of a zero Normal is refused on every write path (as a value outside a field's range is).
		static void ValidatePlanarReflection(const PlanarReflectionComponent& reflection, ValidationContext& context)
		{
			const glm::dvec3 normal(reflection.Normal);
			if (!(glm::dot(normal, normal) > 0.0))
				context.Error("Normal", "must not be zero: it names the reflecting side of the plane");
		}

		static void MakePlanarReflectionValid(PlanarReflectionComponent& reflection, Random& /*random*/)
		{
			const glm::dvec3 normal(reflection.Normal);
			if (!(glm::dot(normal, normal) > 0.0))
				reflection.Normal = glm::vec3(0.0f, 0.0f, 1.0f);
		}

	}

	void RegisterProbesComponents(TypeRegistry& registry)
	{
		RegisterComponent<IrradianceVolumeComponent>(registry, "IrradianceVolume",
			"A box of baked irradiance probes that lights the diffuse surfaces inside it with the light their surroundings bounce; lighting.bake "
			"bakes it.")
			.Category("Probes")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("Data", &IrradianceVolumeComponent::Data,
				"The baked volume that lighting.bake writes beside the scene and assigns; null (not baked) lights nothing.")
			.Field("Size", &IrradianceVolumeComponent::Size, "The box's size in metres, centred on the entity in its local axes.",
				{ .Min = 0.01, .Max = static_cast<double>(MaxIrradianceVolumeSize), .Unit = "m" })
			.Field("ProbeCountX", &IrradianceVolumeComponent::ProbeCountX, "The probes along the box's local X axis, the outer ones on its faces.",
				{ .Min = static_cast<double>(MinIrradianceProbesPerAxis), .Max = static_cast<double>(MaxIrradianceProbesPerAxis) })
			.Field("ProbeCountY", &IrradianceVolumeComponent::ProbeCountY, "The probes along the box's local Y axis, the outer ones on its faces.",
				{ .Min = static_cast<double>(MinIrradianceProbesPerAxis), .Max = static_cast<double>(MaxIrradianceProbesPerAxis) })
			.Field("ProbeCountZ", &IrradianceVolumeComponent::ProbeCountZ, "The probes along the box's local Z axis, the outer ones on its faces.",
				{ .Min = static_cast<double>(MinIrradianceProbesPerAxis), .Max = static_cast<double>(MaxIrradianceProbesPerAxis) })
			.Field("Intensity", &IrradianceVolumeComponent::Intensity, "The unitless strength of the baked indirect light.",
				{ .Min = 0.0, .Max = Utils::MaxProbeIntensity })
			.Field("BlendDistance", &IrradianceVolumeComponent::BlendDistance,
				"The distance inside the box's faces over which its light fades to the Environment's, in metres.",
				{ .Min = 0.0, .Max = Utils::MaxProbeBlendDistance, .Unit = "m" });

		RegisterComponent<ReflectionProbeComponent>(registry, "ReflectionProbe",
			"A baked reflection of the probe's surroundings that glossy surfaces inside its box reflect instead of the Environment; lighting.bake "
			"bakes it.")
			.Category("Probes")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("Data", &ReflectionProbeComponent::Data,
				"The baked reflection, an Environment that lighting.bake writes beside the scene and assigns; null (not baked) reflects nothing.")
			.Field("Size", &ReflectionProbeComponent::Size, "The box whose surfaces reflect the probe, in metres, centred on the entity in its local axes.",
				{ .Min = 0.01, .Max = Utils::MaxReflectionProbeSize, .Unit = "m" })
			.Field("BoxProjection", &ReflectionProbeComponent::BoxProjection,
				"Whether reflections are corrected for the box (parallax-corrected), so a room's reflection lines up with its walls.")
			.Field("Resolution", &ReflectionProbeComponent::Resolution, "The bake's capture cube face, in texels: a power of two.",
				{ .Min = static_cast<double>(MinReflectionProbeResolution), .Max = static_cast<double>(MaxReflectionProbeResolution), .Unit = "texels" })
			.Field("Intensity", &ReflectionProbeComponent::Intensity, "The unitless strength of the reflection.", { .Min = 0.0, .Max = Utils::MaxProbeIntensity })
			.Field("BlendDistance", &ReflectionProbeComponent::BlendDistance,
				"The distance inside the box's faces over which its reflection fades to the Environment's, in metres.",
				{ .Min = 0.0, .Max = Utils::MaxProbeBlendDistance, .Unit = "m" })
			.Validate(&Utils::ValidateReflectionProbe)
			.Generate(&Utils::MakeReflectionProbeValid);

		RegisterComponent<PlanarReflectionComponent>(registry, "PlanarReflection",
			"Makes the flat surface this entity's MeshRenderer draws a true mirror: each view renders the scene again from its camera "
			"mirrored in the plane, clipped at the plane, and the surface reflects that image in place of its probe and screen-space "
			"reflections, blurred by its roughness. At most 4 mirrors a view render, the largest on screen first; the others, and those out "
			"of view or seen from behind, reflect the probes as before.")
			.Category("Probes")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("Normal", &PlanarReflectionComponent::Normal,
				"The plane's normal, pointing out of the reflecting side, in this entity's local axes (the built-in Quad faces +Z, the Plane "
				"+Y). Normalized when used; it must not be zero.")
			.Field("Offset", &PlanarReflectionComponent::Offset,
				"The plane's distance from the entity's origin along Normal, in metres of the entity's local space (its scale applies): the "
				"mirror's surface for a glass that does not pass through its origin, such as one face of a thin box.",
				{ .Min = -Utils::MaxPlanarReflectionOffset, .Max = Utils::MaxPlanarReflectionOffset, .Unit = "m" })
			.Field("ResolutionScale", &PlanarReflectionComponent::ResolutionScale,
				"The reflection's size as a share of the view's scene resolution: 0.5 renders it at half the width and height, a quarter of "
				"the pixels.",
				{ .Min = static_cast<double>(MinPlanarReflectionResolutionScale), .Max = static_cast<double>(MaxPlanarReflectionResolutionScale) })
			.Validate(&Utils::ValidatePlanarReflection)
			.Generate(&Utils::MakePlanarReflectionValid);
	}

}
