#include "EnginePCH.h"
#include "Engine/Physics/WaterWaves.h"

#include "Engine/Core/DetMath.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

namespace Engine {

	namespace Utils {

		constexpr double WaterTwoPi = 2.0 * std::numbers::pi;
		constexpr double WaterRadiansPerDegree = std::numbers::pi / 180.0;

		// The unit vector of the horizontal (x, z) part of `axis`, or nullopt when it has none beyond a float matrix's rounding
		// (a millionth of the axis's length): an axis turned straight up or down.
		static std::optional<glm::dvec2> NormalizeHorizontal(const glm::dvec3& axis)
		{
			const glm::dvec2 horizontal(axis.x, axis.z);
			const double length = std::sqrt(horizontal.x * horizontal.x + horizontal.y * horizontal.y);
			const double full = std::sqrt(glm::dot(axis, axis));
			if (!(length > 1.0e-6 * full) || !std::isfinite(length))
				return std::nullopt;
			return horizontal / length;
		}

		// A world (x, z) vector of local (x, z) `local` in `frame`.
		static glm::dvec2 ToWorldHorizontal(const WaterFrame& frame, const glm::dvec2& local)
		{
			return frame.AxisX * local.x + frame.AxisZ * local.y;
		}

		// x wrapped into [0, 2 pi).
		static double WrapAngle(double x)
		{
			const double wrapped = x - WaterTwoPi * std::floor(x / WaterTwoPi);
			return wrapped < WaterTwoPi && wrapped >= 0.0 ? wrapped : 0.0;
		}

		// Each wave's phase argument at rest point `rest` and `time`, and its sine and cosine.
		struct WaterWaveAngle
		{
			double Sin = 0.0;
			double Cos = 1.0;
		};

		static WaterWaveAngle EvaluateWaveAngle(const WaterSurfaceDesc& surface, const WaterSurfaceWave& wave, const glm::dvec2& rest, double time)
		{
			const glm::dvec2 relative = rest - glm::dvec2(surface.Frame.Origin.x, surface.Frame.Origin.z);
			const double along = wave.Direction.x * relative.x + wave.Direction.y * relative.y;
			const double angle = wave.WaveNumber * along - wave.AngularFrequency * time + wave.Phase;
			const SinCosResult<double> sinCos = DetMath::SinCos(angle);
			return WaterWaveAngle{ .Sin = sinCos.Sin, .Cos = sinCos.Cos };
		}

	}

	WaterFrame MakeWaterFrame(const glm::dmat4& world)
	{
		WaterFrame frame;
		frame.Origin = glm::dvec3(world[3]);
		if (const std::optional<glm::dvec2> axisX = Utils::NormalizeHorizontal(glm::dvec3(world[0])))
		{
			frame.AxisX = *axisX;
			frame.AxisZ = glm::dvec2(-axisX->y, axisX->x);
		}
		else if (const std::optional<glm::dvec2> axisZ = Utils::NormalizeHorizontal(glm::dvec3(world[2])))
		{
			frame.AxisZ = *axisZ;
			frame.AxisX = glm::dvec2(axisZ->y, -axisZ->x);
		}
		return frame;
	}

	double ComputeWaterSteepnessSum(std::span<const WaterWave> waves, double waveScale)
	{
		double sum = 0.0;
		const size_t count = std::min<size_t>(waves.size(), MaxWaterSurfaceWaves);
		for (size_t index = 0; index < count; ++index)
		{
			const WaterWave& wave = waves[index];
			const double wavelength = std::max(static_cast<double>(wave.Wavelength), MinWaterWavelength);
			const double waveNumber = Utils::WaterTwoPi / wavelength;
			sum += static_cast<double>(wave.Steepness) * waveNumber * static_cast<double>(wave.Amplitude) * waveScale;
		}
		return sum;
	}

	WaterSurfaceDesc ResolveWaterSurface(std::span<const WaterWave> waves, double waveScale, const glm::dvec2& current, const WaterFrame& frame)
	{
		WaterSurfaceDesc surface;
		surface.Frame = frame;
		surface.Current = Utils::ToWorldHorizontal(frame, current);
		const size_t count = std::min<size_t>(waves.size(), MaxWaterSurfaceWaves);
		for (size_t index = 0; index < count; ++index)
		{
			const WaterWave& authored = waves[index];
			const SinCosResult<double> direction = DetMath::SinCos(static_cast<double>(authored.Direction) * Utils::WaterRadiansPerDegree);
			const double wavelength = std::max(static_cast<double>(authored.Wavelength), MinWaterWavelength);
			WaterSurfaceWave& wave = surface.Waves[index];
			// A turn about +Y by the direction's angle takes local +X to (cos, -sin) in (x, z).
			wave.Direction = Utils::ToWorldHorizontal(frame, glm::dvec2(direction.Cos, -direction.Sin));
			wave.WaveNumber = Utils::WaterTwoPi / wavelength;
			wave.AngularFrequency = authored.Speed > 0.0f ? static_cast<double>(authored.Speed) * wave.WaveNumber : std::sqrt(WaterGravity * wave.WaveNumber);
			wave.Amplitude = static_cast<double>(authored.Amplitude) * waveScale;
			wave.Steepness = static_cast<double>(authored.Steepness);
			wave.Phase = static_cast<double>(authored.Phase) * Utils::WaterRadiansPerDegree;
		}
		surface.WaveCount = static_cast<uint32_t>(count);
		const double sum = ComputeWaterSteepnessSum(waves, waveScale);
		if (sum > MaxWaterSteepnessSum)
		{
			surface.SteepnessFactor = MaxWaterSteepnessSum / sum;
			for (uint32_t index = 0; index < surface.WaveCount; ++index)
				surface.Waves[index].Steepness *= surface.SteepnessFactor;
		}
		return surface;
	}

	glm::dvec3 DisplaceWaterSurfacePoint(const WaterSurfaceDesc& surface, const glm::dvec2& rest, double time)
	{
		glm::dvec2 horizontal = rest;
		double height = surface.Frame.Origin.y;
		for (uint32_t index = 0; index < surface.WaveCount; ++index)
		{
			const WaterSurfaceWave& wave = surface.Waves[index];
			const Utils::WaterWaveAngle angle = Utils::EvaluateWaveAngle(surface, wave, rest, time);
			horizontal += wave.Direction * (wave.Steepness * wave.Amplitude * angle.Cos);
			height += wave.Amplitude * angle.Sin;
		}
		return glm::dvec3(horizontal.x, height, horizontal.y);
	}

	WaterSurfaceSample EvaluateWaterSurface(const WaterSurfaceDesc& surface, const glm::dvec2& point, double time)
	{
		// The contraction steps: x0 <- point - D(x0), D the horizontal displacement.
		glm::dvec2 rest = point;
		for (uint32_t step = 0; step < WaterContractionSteps; ++step)
		{
			const glm::dvec3 displaced = DisplaceWaterSurfacePoint(surface, rest, time);
			rest -= glm::dvec2(displaced.x, displaced.z) - point;
		}
		// The Newton steps on the horizontal map's analytic Jacobian.
		for (uint32_t step = 0; step < WaterInversionSteps; ++step)
		{
			glm::dvec2 residual = rest - point;
			double jxx = 1.0;
			double jxz = 0.0;
			double jzz = 1.0;
			for (uint32_t index = 0; index < surface.WaveCount; ++index)
			{
				const WaterSurfaceWave& wave = surface.Waves[index];
				const Utils::WaterWaveAngle angle = Utils::EvaluateWaveAngle(surface, wave, rest, time);
				const double horizontal = wave.Steepness * wave.Amplitude;
				residual += wave.Direction * (horizontal * angle.Cos);
				const double slope = horizontal * wave.WaveNumber * angle.Sin;
				jxx -= slope * wave.Direction.x * wave.Direction.x;
				jxz -= slope * wave.Direction.x * wave.Direction.y;
				jzz -= slope * wave.Direction.y * wave.Direction.y;
			}
			// The determinant is at least 1 - MaxWaterSteepnessSum, so the step is always defined.
			const double determinant = jxx * jzz - jxz * jxz;
			rest.x -= (jzz * residual.x - jxz * residual.y) / determinant;
			rest.y -= (jxx * residual.y - jxz * residual.x) / determinant;
		}

		// The analytic height, tangents and particle velocity at the rest point found.
		WaterSurfaceSample sample;
		double height = surface.Frame.Origin.y;
		glm::dvec3 tangentX(1.0, 0.0, 0.0);
		glm::dvec3 tangentZ(0.0, 0.0, 1.0);
		glm::dvec3 velocity(surface.Current.x, 0.0, surface.Current.y);
		for (uint32_t index = 0; index < surface.WaveCount; ++index)
		{
			const WaterSurfaceWave& wave = surface.Waves[index];
			const Utils::WaterWaveAngle angle = Utils::EvaluateWaveAngle(surface, wave, rest, time);
			const glm::dvec2& direction = wave.Direction;
			height += wave.Amplitude * angle.Sin;
			// d/dx0 and d/dz0 of the displaced point.
			const double horizontalSlope = wave.Steepness * wave.Amplitude * wave.WaveNumber * angle.Sin;
			const double verticalSlope = wave.Amplitude * wave.WaveNumber * angle.Cos;
			tangentX += glm::dvec3(-horizontalSlope * direction.x * direction.x, verticalSlope * direction.x, -horizontalSlope * direction.x * direction.y);
			tangentZ += glm::dvec3(-horizontalSlope * direction.x * direction.y, verticalSlope * direction.y, -horizontalSlope * direction.y * direction.y);
			// d/dt of the displaced point (d th / dt = -w).
			const double horizontalSpeed = wave.Steepness * wave.Amplitude * wave.AngularFrequency * angle.Sin;
			velocity += glm::dvec3(horizontalSpeed * direction.x, -wave.Amplitude * wave.AngularFrequency * angle.Cos, horizontalSpeed * direction.y);
		}
		const glm::dvec3 normal = glm::cross(tangentZ, tangentX);
		const double length = std::sqrt(glm::dot(normal, normal));
		sample.Height = height;
		sample.Normal = length > 0.0 ? normal / length : glm::dvec3(0.0, 1.0, 0.0);
		sample.Velocity = velocity;
		return sample;
	}

	std::array<double, MaxWaterSurfaceWaves> ComputeWaterWavePhases(const WaterSurfaceDesc& surface, double time)
	{
		std::array<double, MaxWaterSurfaceWaves> phases{};
		for (uint32_t index = 0; index < surface.WaveCount; ++index)
		{
			const WaterSurfaceWave& wave = surface.Waves[index];
			phases[index] = Utils::WrapAngle(wave.Phase - wave.AngularFrequency * time);
		}
		return phases;
	}

}
