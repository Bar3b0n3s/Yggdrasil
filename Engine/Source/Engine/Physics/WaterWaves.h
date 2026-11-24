#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>

// The water's wave function (M47, Docs/Decisions/0065-m47-water.md decision 3; Architecture §9.12): a water surface is a sum
// of at most MaxWaterSurfaceWaves directional Gerstner waves over a level base plane, a pure function of a horizontal world
// point and the world's time. It is on the simulation path and ECS-agnostic: Scene's queries (Scene/WaterQueries.h), the
// physics' buoyancy and the scripts sample it, and render extraction resolves it for the renderer, whose shader
// (Resources/Shaders/Common/Waves.slang) mirrors the sum. Everything runs in double precision with Core/DetMath and a fixed
// number of operations, in Physics (precise floating point, §2.2), so a tick gives bit-identical surfaces in Debug, Release
// and Dist.
//
// The waves. Wave i, with world unit direction D_i, wave number k_i = 2 pi / Wavelength, angular frequency w_i, amplitude
// A_i (the authored one times the water's WaveScale), steepness Q_i and phase p_i, moves the surface particle that rests at
// the horizontal point x0 (world XZ, as a glm::dvec2 (x, z)) at time t to
//   x = x0 + sum_i Q_i A_i D_i cos(th_i),   y = Base + sum_i A_i sin(th_i),   th_i = k_i dot(D_i, x0 - Origin) - w_i t + p_i,
// a crest travelling along D_i. w_i is Speed * k_i, or the deep-water dispersion sqrt(WaterGravity * k_i) for a Speed of 0. The
// horizontal map's Jacobian is I - sum_i Q_i A_i k_i sin(th_i) D_i D_i^T; ResolveWaterSurface scales every Q_i by one factor
// so that sum_i Q_i k_i A_i <= MaxWaterSteepnessSum < 1, so the map is a diffeomorphism and the surface never folds.
//
// Sampling under a world point (EvaluateWaterSurface) inverts the horizontal map: WaterContractionSteps fixed-point steps
// (x0 <- x - (H(x0) - x0), a contraction of factor sum_i Q_i k_i A_i), then WaterInversionSteps Newton steps on the analytic
// 2 x 2 Jacobian, from x0 = x. Every input runs the same operations. The normal and the particle's velocity are analytic at
// the x0 found; the velocity adds the water's Current, a horizontal flow that moves no wave.

namespace Engine {

	// The most waves a water surface sums (the Water component's Waves, Scene/Components/WaterComponent.h, holds at most
	// MaxWaterWaves of Renderer/RenderSnapshot.h, which equals it).
	inline constexpr uint32_t MaxWaterSurfaceWaves = 8;
	// The gravity of the deep-water dispersion relation w = sqrt(g k), m/s^2.
	inline constexpr double WaterGravity = 9.81;
	// The largest sum_i Q_i k_i A_i: the horizontal map's Jacobian keeps a determinant of at least 1 - 0.9 everywhere.
	inline constexpr double MaxWaterSteepnessSum = 0.9;
	// The fixed-point steps before the Newton steps: they bring x0 within the Newton steps' reach from any start, which Newton
	// alone does not on a sum of short steep waves over long ones (Docs/Decisions/0065-m47-water.md, the W47.1 record).
	inline constexpr uint32_t WaterContractionSteps = 8;
	// The Newton steps that finish the inversion.
	inline constexpr uint32_t WaterInversionSteps = 4;
	// The authored bounds of a wave (the Water component's registry ranges).
	inline constexpr double MinWaterWavelength = 0.1;
	inline constexpr double MaxWaterWavelength = 2000.0;
	inline constexpr double MaxWaterAmplitude = 20.0;
	inline constexpr double MaxWaterWaveSpeed = 200.0;
	inline constexpr double MaxWaterWaveScale = 10.0;
	inline constexpr double MaxWaterCurrent = 100.0;

	// Registry struct "WaterWave" (M47, decision 3): one authored wave of a Water component, in the water's local frame. A
	// plain value; the Water component's Waves holds these.
	struct WaterWave
	{
		float Direction = 0.0f;   // degrees about +Y in the water's local XZ plane: 0 travels along local +X, 90 along local -Z
		float Wavelength = 20.0f; // metres, [MinWaterWavelength, MaxWaterWavelength]
		float Amplitude = 0.2f;   // metres, [0, MaxWaterAmplitude]
		float Steepness = 0.5f;   // Gerstner's Q, [0, 1]; 0 is a sine wave
		float Speed = 0.0f;       // m/s, [0, MaxWaterWaveSpeed]; 0 takes deep-water dispersion
		float Phase = 0.0f;       // degrees
	};

	// Where a water lies in the world: its base point (the entity's world position; its Y is the base height) and the
	// directions its local +X and +Z axes take in the world's XZ plane (unit vectors as (x, z); only the yaw orients a water).
	struct WaterFrame
	{
		glm::dvec3 Origin = glm::dvec3(0.0);
		glm::dvec2 AxisX = glm::dvec2(1.0, 0.0);
		glm::dvec2 AxisZ = glm::dvec2(0.0, 1.0);
	};

	// One resolved wave: its world unit direction (x, z), wave number k (rad/m), angular frequency w (rad/s), amplitude (m,
	// WaveScale applied), steepness Q (scaled by the fold limit) and phase (radians).
	struct WaterSurfaceWave
	{
		glm::dvec2 Direction = glm::dvec2(1.0, 0.0);
		double WaveNumber = 0.0;
		double AngularFrequency = 0.0;
		double Amplitude = 0.0;
		double Steepness = 0.0;
		double Phase = 0.0;
	};

	// A water surface ready to evaluate: its frame, its resolved waves (the first WaveCount of Waves) and its current (world
	// XZ, m/s, as (x, z)). SteepnessFactor is the factor every authored Q was scaled by (1 when the sum was within the limit;
	// WATER_WAVES_TOO_STEEP names it).
	struct WaterSurfaceDesc
	{
		WaterFrame Frame{};
		std::array<WaterSurfaceWave, MaxWaterSurfaceWaves> Waves{};
		uint32_t WaveCount = 0;
		glm::dvec2 Current = glm::dvec2(0.0);
		double SteepnessFactor = 1.0;
	};

	// The surface at one horizontal point and time: the world height, the world unit normal and the surface particle's
	// velocity plus the current (m/s).
	struct WaterSurfaceSample
	{
		double Height = 0.0;
		glm::dvec3 Normal = glm::dvec3(0.0, 1.0, 0.0);
		glm::dvec3 Velocity = glm::dvec3(0.0);
	};

	// The frame of a water entity whose world matrix is `world`: its translation, and its local X axis projected on the XZ
	// plane and normalized (its local Z axis's, turned, when X is vertical; the world's axes when both are), Z a quarter
	// turn from X about +Y. A pitched, rolled or sheared matrix gives the level frame nearest its yaw.
	[[nodiscard]] WaterFrame MakeWaterFrame(const glm::dmat4& world);

	// The sum of authored Q_i k_i A_i of `waves` at `waveScale`, before the fold limit (WATER_WAVES_TOO_STEEP compares it with
	// MaxWaterSteepnessSum). Only the first MaxWaterSurfaceWaves waves count.
	[[nodiscard]] double ComputeWaterSteepnessSum(std::span<const WaterWave> waves, double waveScale);

	// The surface of `waves` (at most the first MaxWaterSurfaceWaves) in `frame`, every amplitude times `waveScale` and the
	// local `current` (x, z) turned into the world. Each wave's local direction turns by the frame's yaw; its wave number is
	// 2 pi / Wavelength; its angular frequency Speed * k, or sqrt(WaterGravity * k) for a Speed of 0; its phase in radians.
	// When the steepness sum exceeds MaxWaterSteepnessSum every Q is scaled by MaxWaterSteepnessSum / sum (SteepnessFactor).
	// The inputs are the component's, already within its registry ranges; a wavelength below MinWaterWavelength is taken at
	// it.
	[[nodiscard]] WaterSurfaceDesc ResolveWaterSurface(std::span<const WaterWave> waves, double waveScale, const glm::dvec2& current, const WaterFrame& frame);

	// The displaced surface point of the particle at rest at horizontal world point `rest` (x, z) at `time` (the forward map
	// above), in world space.
	[[nodiscard]] glm::dvec3 DisplaceWaterSurfacePoint(const WaterSurfaceDesc& surface, const glm::dvec2& rest, double time);

	// The surface under horizontal world point `point` (x, z) at `time` seconds of world time: the particle whose displaced
	// position lies over `point` (the inversion above), its height, its analytic normal and its velocity plus the current.
	// A calm surface (no wave) is the base plane: Height Frame.Origin.y, Normal +Y, Velocity the current.
	[[nodiscard]] WaterSurfaceSample EvaluateWaterSurface(const WaterSurfaceDesc& surface, const glm::dvec2& point, double time);

	// Each wave's phase at `time`, p_i - w_i time, wrapped into [0, 2 pi) in double precision (the renderer's per-wave phase:
	// th_i = k_i dot(D_i, x0 - Origin) + phase_i). Entries past WaveCount are 0.
	[[nodiscard]] std::array<double, MaxWaterSurfaceWaves> ComputeWaterWavePhases(const WaterSurfaceDesc& surface, double time);

}
