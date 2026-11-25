#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// The SkyResults buffer of a view (M45, Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; Renderer/SkyLighting.h):
	// float4 elements the sky's computes write (CSSkyFinalize, CSProbeRelight) and the renderer copies into the view's constant
	// buffers after their CPU writes (RecordSkyResultCopies):
	//   - SkyResultsIrradiance to + 8: the sky's irradiance SH9 times Intensity (EnvironmentConstants::IrradianceSH9's
	//     convention, w 0), copied over the 144 bytes of EnvironmentConstants::IrradianceSH9;
	//   - SkyResultsFogColor: the fog's colour tinted by that irradiance (MakeFogConstants' rule, xyz), copied over the 12
	//     bytes of FogConstants::Color of the view and of each mirror slot;
	//   - SkyResultsReflectionScales + i, i below MaxViewReflectionProbes: x, reflection probe i's relit intensity, copied over
	//     the 4 bytes of ProbeConstants::Reflections[i].AxisScale.w.
#if defined(ENGINE_SHADER)
	static const uint SkyResultsIrradiance = 0;
	static const uint SkyResultsFogColor = 9;
	static const uint SkyResultsReflectionScales = 10;
	static const uint SkyResultsCount = 18;
#else
inline constexpr uint32_t SkyResultsIrradiance = 0;
inline constexpr uint32_t SkyResultsFogColor = 9;
inline constexpr uint32_t SkyResultsReflectionScales = 10;
inline constexpr uint32_t SkyResultsCount = 18;
#endif

	// The relit irradiance volumes (W45.3; Renderer/ProbeRelight.h, Passes/Sky.slang's CSProbeRelight):
	//   - a relightable volume's GPU relight block (GpuResourceCache's GpuIrradianceVolume::Relight) holds SkyRelightProbeVectors
	//     float4 per probe, in its probes' order: IrradianceProbeRelight::Local's nine coefficients (xyz, w 0), then its
	//     Transfer's 45 values four by four in twelve float4 (the last three components 0);
	//   - CSProbeRelight's modes (SkyDispatchConstants::Mode): SkyRelightModeVolume relights Count probes of a volume into the
	//     view's IrradianceProbes copy from element Face (the volume's first coefficient, ProbeVolumeConstants::Probes.w);
	//     SkyRelightModeReflections writes the Count reflection probes' relit intensities into SkyResults;
	//   - SkyViewResources::ReflectionRelights holds MaxViewReflectionProbes float4, one per reflection probe of the view in
	//     ProbeConstants' order: x the IrradianceProbes element of the relit constant term of the volume probe nearest its
	//     centre (an exact integer), or -1 outside every relit volume; y that probe's baked constant term's luminance; z the
	//     reflection probe's Intensity.
#if defined(ENGINE_SHADER)
	static const uint SkyRelightProbeVectors = 21;
	static const uint SkyRelightModeVolume = 0;
	static const uint SkyRelightModeReflections = 1;
	static const uint SkyRelightGroupSize = 64;
#else
inline constexpr uint32_t SkyRelightProbeVectors = 21;
inline constexpr uint32_t SkyRelightModeVolume = 0;
inline constexpr uint32_t SkyRelightModeReflections = 1;
inline constexpr uint32_t SkyRelightGroupSize = 64;
#endif

	// The sky of one view (M45, decision 3; Renderer/SkyLighting.h, Passes/Sky.slang, Common/Atmosphere.slang): a constant
	// buffer the scene renderer writes once per view from the snapshot's RenderSky, read by the Sky pass and the sky's computes.
	// Directions are unit, world space, toward their source; the atmosphere is in km and per km (Renderer/SkyAtmosphere.h's
	// MakeSkyAtmosphere); colours are linear.
	struct SkyConstants
	{
		// The atmosphere: Rayleigh scattering and the ground's radius; ozone absorption and the top's radius; the ground's
		// albedo and Mie scattering; Mie extinction and asymmetry; the camera's altitude above the ground (km,
		// ComputeSkyCameraAltitude) and the sky's Intensity.
		Float3 RayleighScattering;
		float BottomRadius = 0.0f;
		Float3 OzoneAbsorption;
		float TopRadius = 0.0f;
		Float3 GroundAlbedo;
		float MieScattering = 0.0f;
		float MieExtinction = 0.0f;
		float MieAsymmetry = 0.0f;
		float CameraAltitude = 0.0f;
		float Intensity = 0.0f;
		// The sun: toward it; the cosine of its disc's angular radius (its light's LightAngle / 2); the illuminance the
		// atmosphere scatters (the Sun light's authored Color times its Intensity, RenderSky::SunLightColor; zero without a Sun
		// light); the disc's radiance per unit of transmitted illuminance (SunDisc over the disc's solid angle; 0 draws none).
		Float3 SunDirection;
		float SunCosRadius = 1.0f;
		Float3 SunIlluminance;
		float SunDiscRadiance = 0.0f;
		// The moon: toward it; the cosine of its disc's angular radius (MoonSize / 2); the moon light's authored Color times its
		// Intensity (zero without a Moon light); its illuminated fraction (ComputeMoonIlluminatedFraction), by which its light
		// scatters.
		Float3 MoonDirection;
		float MoonCosRadius = 1.0f;
		Float3 MoonIlluminance;
		float MoonFraction = 0.0f;
		// The stars' frame (ComputeSkyStarFrame): its rows, world directions to celestial ones (xyz; w 0).
		Float4 StarFrame0;
		Float4 StarFrame1;
		Float4 StarFrame2;
		// The stars' brightness (Stars), Overcast, the cloud layer's coverage (Clouds) and its height above Y 0 (m,
		// SkyCloudHeight).
		float Stars = 0.0f;
		float Overcast = 0.0f;
		float CloudCoverage = 0.0f;
		float CloudHeight = 0.0f;
		// The cloud layer's offset in world XZ (m): CloudSpeed times SimulationTime, in double, wrapped to the cloud noise's
		// period; 1 when the view has a moon (RenderSky::HasMoon), else 0.
		Float2 CloudOffset;
		uint32_t HasMoon = 0;
		uint32_t Padding0 = 0;
		// The fog's colour and AmbientTint as MakeFogConstants makes them usable (Renderer/Fog.h), from which CSSkyFinalize
		// tints the colour by the sky's irradiance (SkyResultsFogColor).
		Float3 FogColor;
		float FogAmbientTint = 0.0f;
	};

	// The per-dispatch constants of the sky's computes, their push constants: each entry reads the members its comment names.
	struct SkyDispatchConstants
	{
		Float2 CloudOffset;    // CSSkyCube: the cloud layer's offset the image-based light draws (at its SkyCloudLightingStep)
		float Altitude = 0.0f; // CSSkyViewLut: the altitude (km) the LUT is taken from: the camera's, or sea level's for the cube
		uint32_t Face = 0;     // the cube face, or the first element a dispatch writes (CSProbeRelight's volume: its first coefficient)
		uint32_t Count = 0;    // the items a dispatch covers (CSProbeRelight: probes, or reflection probes)
		uint32_t Mode = 0;     // an entry's variant (CSProbeRelight: SkyRelightModeVolume or SkyRelightModeReflections)
		uint32_t Padding0 = 0;
		uint32_t Padding1 = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(SkyConstants) == 224, "SkyConstants must match its 224-byte constant-buffer layout");
	static_assert(offsetof(SkyConstants, MieExtinction) == 48 && offsetof(SkyConstants, SunDirection) == 64
			&& offsetof(SkyConstants, MoonDirection) == 96 && offsetof(SkyConstants, StarFrame0) == 128 && offsetof(SkyConstants, Stars) == 176
			&& offsetof(SkyConstants, CloudOffset) == 192 && offsetof(SkyConstants, FogColor) == 208,
		"SkyConstants members must sit where the constant-buffer layout puts them");
	static_assert(sizeof(SkyDispatchConstants) == 32 && offsetof(SkyDispatchConstants, Altitude) == 8,
		"SkyDispatchConstants must match its 32-byte push-constant layout");

}
#endif
