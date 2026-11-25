#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// The sky's CPU part (M45, Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; Architecture §8.3 "Sky", §8.6): the
// atmosphere of Hillaire's LUT method ("A Scalable and Production Ready Sky and Atmosphere Rendering Technique", EGSR 2020)
// with its Earth constants, the moon's phase, the stars' frame and the roles of a RenderSky's Sun and Moon. Pure functions of
// their arguments, thread-safe, no GPU state; double precision, Core/DetMath for every transcendental (no <cmath>) and fixed
// step counts, so render extraction (Scene, on the simulation path's lint scope, §4.12) may call ApplySkyLightDriving
// (RenderSnapshot.h) and every result is identical in every configuration. The GPU (Renderer/SkyLighting.h, Passes/Sky.slang,
// Common/Atmosphere.slang) evaluates the same model in float from SkyConstants (Shared/SkyConstants.h).
//   - Units: the atmosphere in kilometres (radii, heights) and per kilometre (coefficients); directions unit, world space,
//     +Y up. The ground is a sphere of SkyBottomRadius whose top is world Y 0 under the camera; the camera's altitude is
//     max(Position.y, 0) metres plus SkyCameraHeightOffset (ComputeSkyCameraAltitude), so a camera at or below Y 0 sees the
//     horizon as one standing on the ground.
//   - Directions toward a light: the opposite of its LightData::Direction (a light shines down its local -Z, so its source is
//     toward its local +Z), and the moon's, RenderSky::MoonDirection.
//   - Haze multiplies the aerosols' (Mie) density; Overcast adds SkyOvercastMieScale times as many grey aerosols and lowers
//     their asymmetry toward SkyOvercastMieAsymmetry, so a full overcast dims the sun at the zenith to a few percent and turns
//     the sky grey (MakeSkyAtmosphere).

namespace Engine {

	// Hillaire's Earth (EGSR 2020, table 1, and Bruneton's reference constants it takes): the ground's and the atmosphere's top
	// radii (km), Rayleigh scattering (per km, no absorption) with its scale height, Mie scattering and absorption (per km;
	// single-scattering albedo 0.9) with its scale height and asymmetry, and ozone absorption (per km) in a tent of
	// SkyOzoneHalfWidth about SkyOzoneCenter.
	inline constexpr double SkyBottomRadius = 6360.0;
	inline constexpr double SkyTopRadius = 6460.0;
	inline constexpr glm::dvec3 SkyRayleighScattering = glm::dvec3(5.802e-3, 13.558e-3, 33.1e-3);
	inline constexpr double SkyRayleighScaleHeight = 8.0;
	inline constexpr double SkyMieScattering = 3.996e-3;
	inline constexpr double SkyMieAbsorption = 0.444e-3;
	inline constexpr double SkyMieScaleHeight = 1.2;
	inline constexpr double SkyMieAsymmetry = 0.8;
	inline constexpr glm::dvec3 SkyOzoneAbsorption = glm::dvec3(0.650e-3, 1.881e-3, 0.085e-3);
	inline constexpr double SkyOzoneCenter = 25.0;
	inline constexpr double SkyOzoneHalfWidth = 15.0;
	// What a full Overcast adds: SkyOvercastMieScale times the clear day's aerosols (a vertical optical depth of about 3.2),
	// with the asymmetry lowered from SkyMieAsymmetry toward SkyOvercastMieAsymmetry.
	inline constexpr double SkyOvercastMieScale = 600.0;
	inline constexpr double SkyOvercastMieAsymmetry = 0.3;
	// The camera's height above the ground at Y 0 and below it: 1 m, in km.
	inline constexpr double SkyCameraHeightOffset = 0.001;
	// The optical depth's fixed steps along a ray (ComputeSkyTransmittance; the transmittance LUT's texels take as many).
	inline constexpr uint32_t SkyTransmittanceSteps = 40;

	// The GPU's resources (Renderer/SkyLighting.h), sized here so the CPU references match them: the transmittance LUT
	// (SkyTransmittanceLutWidth x SkyTransmittanceLutHeight over Bruneton's (mu, r) mapping), the multiple-scattering LUT
	// (SkyMultipleScatteringLutSize square over (cos sun zenith, height)), the sky-view LUT (SkyViewLutWidth x SkyViewLutHeight
	// over (world azimuth, view elevation): the sun and the moon scatter at their own azimuths) and the image-based light's
	// cube (SkyCubeSize, the environment asset's EnvironmentData::SpecularFaceSize, so its prefiltered mips keep
	// SpecularMaxLod's meaning).
	inline constexpr uint32_t SkyTransmittanceLutWidth = 256;
	inline constexpr uint32_t SkyTransmittanceLutHeight = 64;
	inline constexpr uint32_t SkyMultipleScatteringLutSize = 32;
	inline constexpr uint32_t SkyViewLutWidth = 192;
	inline constexpr uint32_t SkyViewLutHeight = 108;
	inline constexpr uint32_t SkyCubeSize = 256;
	// The cloud layer: a plane SkyCloudHeight metres above Y 0; the image-based light follows its drift in steps of
	// SkyCloudLightingStep seconds of simulation time, so a drifting layer does not regenerate it every frame (decision 4).
	inline constexpr float SkyCloudHeight = 2000.0f;
	inline constexpr double SkyCloudLightingStep = 1.0;

	// The atmosphere of one Sky's Haze, Overcast and GroundAlbedo (MakeSkyAtmosphere): the coefficients per km at density 1.
	struct SkyAtmosphere
	{
		glm::dvec3 RayleighScattering = SkyRayleighScattering;
		double MieScattering = SkyMieScattering;
		double MieExtinction = SkyMieScattering + SkyMieAbsorption;
		double MieAsymmetry = SkyMieAsymmetry;
		glm::dvec3 OzoneAbsorption = SkyOzoneAbsorption;
		glm::dvec3 GroundAlbedo = glm::dvec3(0.3);

		bool operator==(const SkyAtmosphere&) const = default;
	};

	// The atmosphere of `haze` (clamped to [0, 10]), `overcast` ([0, 1]) and `groundAlbedo` (each channel [0, 1]); a non-finite
	// value takes the component's default (1, 0, 0.3). Mie scattering and extinction are Hillaire's times (haze + overcast *
	// SkyOvercastMieScale); the asymmetry is SkyMieAsymmetry lerped toward SkyOvercastMieAsymmetry by overcast.
	[[nodiscard]] SkyAtmosphere MakeSkyAtmosphere(float haze, float overcast, const glm::vec3& groundAlbedo);

	// The atmosphere's extinction per km at `height` km above the ground (Rayleigh scattering, Mie extinction and ozone
	// absorption by their densities: exp(-height / scale height), and the ozone's tent).
	[[nodiscard]] glm::dvec3 ComputeSkyExtinction(const SkyAtmosphere& atmosphere, double height);

	// The transmittance from radius `radius` km (clamped to [SkyBottomRadius, SkyTopRadius]) along a ray whose cosine with the
	// local up is `mu` ([-1, 1]) to the atmosphere's top: exp(-optical depth), the depth by SkyTransmittanceSteps midpoint
	// steps; zero when the ray meets the ground. What the transmittance LUT holds at that (radius, mu).
	[[nodiscard]] glm::dvec3 ComputeSkyTransmittance(const SkyAtmosphere& atmosphere, double radius, double mu);

	// The transmittance from `altitude` km above the ground (Y 0) toward the unit world `direction`: the overload above at
	// SkyBottomRadius + altitude with mu = direction.y; zero below the horizon (a ray that meets the ground). Errors: none; a
	// non-finite or zero direction gives zero.
	[[nodiscard]] glm::dvec3 ComputeSkyTransmittance(double altitude, const glm::dvec3& direction, float haze, float overcast);

	// The camera's altitude above the ground in km: max(position.y, 0) metres plus SkyCameraHeightOffset; a non-finite height
	// is 0 m.
	[[nodiscard]] double ComputeSkyCameraAltitude(const glm::vec3& position);

	// The moon's illuminated fraction, (1 - cos psi) / 2 of its elongation psi from the sun (the angle between the unit
	// directions toward them): 0 new (beside the sun), 1 full (opposite). Non-unit directions are normalized; a zero or
	// non-finite one gives 0.
	[[nodiscard]] double ComputeMoonIlluminatedFraction(const glm::dvec3& toSun, const glm::dvec3& toMoon);

	// The celestial pole: north (the direction about +Y by northAngle degrees, counter-clockwise seen from above, from -Z)
	// raised by latitude degrees: cos(latitude) * north + sin(latitude) * up.
	[[nodiscard]] glm::dvec3 ComputeSkyPole(double latitude, double northAngle);

	// The stars' frame: the rotation from world directions to the stars' fixed (celestial) directions, in which the pole is +Z.
	// Its axes before the turn: Z the pole, X the meridian (east cross the pole, east = north cross up, so X leans up toward
	// the zenith), Y = Z cross X. The sun's hour angle h is its angle about the pole from X toward Y; the frame then turns by
	// -h about the pole, so the sun keeps fixed celestial coordinates and the stars turn about the pole with it, with no time
	// state. A zero or non-finite sun direction takes h 0.
	[[nodiscard]] glm::dmat3 ComputeSkyStarFrame(const glm::dvec3& toSun, double latitude, double northAngle);

	// The order in which the renderer offers `snapshot`'s directional lights the shadow cascades (§8.7: the first that is
	// visible, casts shadows and that the planner accepts takes them): with an enabled Sky, its SunLight first while the Sun
	// is above the horizon (its direction toward it has y > 0), else its MoonLight; then every other directional light in
	// snapshot order. Without a Sky, or with neither role, every directional light in snapshot order (M23's rule). Indices
	// into snapshot.Lights.
	[[nodiscard]] std::vector<uint32_t> OrderShadowCandidates(const RenderSnapshot& snapshot);

	// The light the fog's glow follows (Renderer/Fog.h): an enabled Sky's SunLight; otherwise the first directional light of
	// snapshot.Lights; empty without one.
	[[nodiscard]] std::optional<size_t> GetSkyGlowLight(const RenderSnapshot& snapshot);

}
