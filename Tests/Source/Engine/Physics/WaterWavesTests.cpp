#include "TestsPCH.h"

#include "Engine/Physics/WaterWaves.h"

#include "Engine/Core/Random.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

// The water's wave function (M47, Docs/Decisions/0065-m47-water.md decision 3): a calm surface is its base plane; the
// inversion finds the displaced surface over a point; the normal and the velocity are the surface's analytic derivatives;
// the fold limit scales the steepness; the frame takes the yaw alone; the phases the renderer reads wrap in double.

namespace Engine {

	namespace {

		// A frame at `origin` turned `yawDegrees` about +Y.
		WaterFrame MakeYawFrame(const glm::dvec3& origin, double yawDegrees)
		{
			const glm::mat4 world = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(origin)), glm::radians(static_cast<float>(yawDegrees)),
				glm::vec3(0.0f, 1.0f, 0.0f));
			WaterFrame frame = MakeWaterFrame(glm::dmat4(world));
			frame.Origin = origin;
			return frame;
		}

		// A three-wave swell whose steepness sum (about 1.6) is past the fold limit, so it is scaled to it.
		std::vector<WaterWave> MakeSteepSwell()
		{
			return {
				WaterWave{ .Direction = 10.0f, .Wavelength = 24.0f, .Amplitude = 1.2f, .Steepness = 1.0f, .Speed = 0.0f, .Phase = 0.0f },
				WaterWave{ .Direction = 75.0f, .Wavelength = 9.0f, .Amplitude = 0.6f, .Steepness = 1.0f, .Speed = 0.0f, .Phase = 40.0f },
				WaterWave{ .Direction = -130.0f, .Wavelength = 3.5f, .Amplitude = 0.15f, .Steepness = 1.0f, .Speed = 4.0f, .Phase = 200.0f },
			};
		}

		// A storm of eight waves from 2 m to 300 m, at 2.5 times their amplitude.
		std::vector<WaterWave> MakeStorm()
		{
			std::vector<WaterWave> waves;
			const std::array<float, 8> wavelengths = { 300.0f, 120.0f, 61.0f, 33.0f, 17.0f, 9.0f, 4.5f, 2.0f };
			for (size_t index = 0; index < wavelengths.size(); ++index)
			{
				waves.push_back(WaterWave{ .Direction = 47.0f * static_cast<float>(index) - 80.0f,
					.Wavelength = wavelengths[index],
					.Amplitude = wavelengths[index] * 0.02f,
					.Steepness = 0.9f,
					.Speed = index % 3 == 0 ? 0.0f : 3.0f + static_cast<float>(index),
					.Phase = 33.0f * static_cast<float>(index) });
			}
			return waves;
		}

		double SteepnessSum(const WaterSurfaceDesc& surface)
		{
			double sum = 0.0;
			for (uint32_t index = 0; index < surface.WaveCount; ++index)
				sum += surface.Waves[index].Steepness * surface.Waves[index].WaveNumber * surface.Waves[index].Amplitude;
			return sum;
		}

	}

	TEST_SUITE("Physics")
	{
		TEST_CASE("WaterWaves: a calm water is flat at its base height")
		{
			const WaterFrame frame = MakeYawFrame(glm::dvec3(3.0, -2.5, 7.0), 30.0);
			const std::vector<WaterWave> swell = MakeSteepSwell();
			// No wave, and a swell at WaveScale 0: the base plane, with the current turned by the yaw.
			const std::array<WaterSurfaceDesc, 2> calms = { ResolveWaterSurface({}, 1.0, glm::dvec2(1.5, -0.5), frame),
				ResolveWaterSurface(swell, 0.0, glm::dvec2(1.5, -0.5), frame) };
			CHECK(calms[0].WaveCount == 0);
			CHECK(calms[1].WaveCount == 3);
			const glm::dvec2 current = frame.AxisX * 1.5 + frame.AxisZ * -0.5;
			for (const WaterSurfaceDesc& calm : calms)
			{
				for (const glm::dvec2 point : { glm::dvec2(0.0), glm::dvec2(-120.25, 33.5), glm::dvec2(2500.0, -2400.0) })
				{
					for (const double time : { 0.0, 1.0 / 60.0, 3600.0 })
					{
						const WaterSurfaceSample sample = EvaluateWaterSurface(calm, point, time);
						CHECK(sample.Height == -2.5);
						CHECK(sample.Normal == glm::dvec3(0.0, 1.0, 0.0));
						CHECK(sample.Velocity == glm::dvec3(current.x, 0.0, current.y));
					}
				}
			}
		}

		TEST_CASE("WaterWaves: a Gerstner surface's sampled height lies on the displaced surface within 1 mm")
		{
			double worst = 0.0;
			uint32_t samples = 0;
			// The height EvaluateWaterSurface finds over the displaced position of rest point `rest`, against the displaced height.
			const auto check = [&worst, &samples](const WaterSurfaceDesc& surface, const glm::dvec2& rest, double time)
			{
				const glm::dvec3 displaced = DisplaceWaterSurfacePoint(surface, rest, time);
				const WaterSurfaceSample sample = EvaluateWaterSurface(surface, glm::dvec2(displaced.x, displaced.z), time);
				worst = std::max(worst, std::abs(sample.Height - displaced.y));
				++samples;
			};

			// The swell at the fold limit, and a storm far from the origin, late in a session.
			const std::array<WaterSurfaceDesc, 2> surfaces = { ResolveWaterSurface(MakeSteepSwell(), 1.0, glm::dvec2(0.0), MakeYawFrame(glm::dvec3(0.0, 1.0, 0.0), 0.0)),
				ResolveWaterSurface(MakeStorm(), 2.5, glm::dvec2(0.3, 0.0), MakeYawFrame(glm::dvec3(2400.0, -3.0, -1800.0), 205.0)) };
			CHECK(SteepnessSum(surfaces[0]) == doctest::Approx(MaxWaterSteepnessSum).epsilon(1e-12));
			CHECK(SteepnessSum(surfaces[1]) == doctest::Approx(MaxWaterSteepnessSum).epsilon(1e-12));
			for (const WaterSurfaceDesc& surface : surfaces)
			{
				const glm::dvec2 origin(surface.Frame.Origin.x, surface.Frame.Origin.z);
				for (const double time : { 0.0, 7.25, 3599.5 })
				{
					for (int x = -12; x <= 12; ++x)
					{
						for (int z = -12; z <= 12; ++z)
							check(surface, origin + glm::dvec2(x * 1.37, z * 2.11), time);
					}
				}
			}

			// W47.1's review (finding 1): a long high wave carrying most of the steepness sum with a short chop riding it, where
			// Newton steps alone fall into a 2-cycle. A storm sea within every bound (sum 0.899, unscaled), and the same pair 40
			// times the size at the component's bounds (WaveScale 10). 8 contraction steps then 4 Newton steps missed by up to
			// 23 mm on the first and 0.92 m on the second.
			const std::vector<WaterWave> stormPair = { WaterWave{ .Direction = 72.57f, .Wavelength = 22.0f, .Amplitude = 2.68f, .Steepness = 1.0f, .Phase = 258.9f },
				WaterWave{ .Direction = 247.85f, .Wavelength = 0.1565f, .Amplitude = 0.00333f, .Steepness = 1.0f, .Phase = 192.3f } };
			const std::vector<WaterWave> boundsPair = { WaterWave{ .Direction = 72.57f, .Wavelength = 879.94f, .Amplitude = 10.734f, .Steepness = 1.0f, .Phase = 258.9f },
				WaterWave{ .Direction = 247.85f, .Wavelength = 6.2579f, .Amplitude = 0.013303f, .Steepness = 1.0f, .Phase = 192.3f } };
			const WaterFrame level = MakeYawFrame(glm::dvec3(0.0), 0.0);
			const std::array<std::pair<WaterSurfaceDesc, double>, 2> pairs = { std::pair{ ResolveWaterSurface(stormPair, 1.0, glm::dvec2(0.0), level), 1.0 },
				std::pair{ ResolveWaterSurface(boundsPair, 10.0, glm::dvec2(0.0), level), 40.0 } };
			for (const auto& [surface, size] : pairs)
			{
				for (const double time : { 0.0, 1.5 * size })
				{
					for (int x = 0; x < 400; ++x)
					{
						for (int z = 0; z < 39; ++z)
							check(surface, glm::dvec2(-60.0 + 0.3 * x, -55.0 + 0.27 * z) * size, time);
					}
				}
			}

			// A seeded search at the bounds: two to eight waves, the first long and high and carrying most of the sum, the others
			// down to a 20,000th of its wavelength, all of the steepest, at WaveScale 10, under a turned frame far from the origin.
			Random random(4701);
			const auto uniform = [&random](double low, double high)
			{
				return low + (high - low) * random.NextDouble();
			};
			for (int set = 0; set < 240; ++set)
			{
				const int count = static_cast<int>(random.RangeInt(2, 8));
				const double longest = uniform(5.0, 2000.0);
				double share = uniform(0.5, 0.97);
				double rest = 1.0 - share;
				std::vector<WaterWave> waves;
				for (int index = 0; index < count; ++index)
				{
					if (index > 0)
					{
						share = index + 1 < count ? rest * random.NextDouble() : rest;
						rest -= share;
					}
					const double wavelength = index == 0 ? longest : std::max(0.1, longest / std::exp2(uniform(1.6, 14.3)));
					const double amplitude = std::min(MaxWaterSteepnessSum * share * wavelength / (2.0 * std::numbers::pi) / 10.0, 20.0);
					waves.push_back(WaterWave{ .Direction = static_cast<float>(uniform(0.0, 360.0)),
						.Wavelength = static_cast<float>(wavelength),
						.Amplitude = static_cast<float>(amplitude),
						.Steepness = 1.0f,
						.Speed = set % 2 == 0 ? 0.0f : static_cast<float>(uniform(0.0, 200.0)),
						.Phase = static_cast<float>(uniform(0.0, 360.0)) });
				}
				const WaterSurfaceDesc surface = ResolveWaterSurface(waves, 10.0, glm::dvec2(0.0),
					MakeYawFrame(glm::dvec3(uniform(-2500.0, 2500.0), 0.0, uniform(-2500.0, 2500.0)), uniform(0.0, 360.0)));
				for (int point = 0; point < 12; ++point)
					check(surface, glm::dvec2(uniform(-3000.0, 3000.0), uniform(-3000.0, 3000.0)), uniform(0.0, 7200.0));
			}
			CAPTURE(samples);
			CAPTURE(worst);
			CHECK(worst < 1.0e-3);
		}

		TEST_CASE("WaterWaves: the normal is the height's gradient and the velocity its time derivative")
		{
			const WaterSurfaceDesc surface = ResolveWaterSurface(MakeSteepSwell(), 0.8, glm::dvec2(0.4, -0.2), MakeYawFrame(glm::dvec3(-40.0, 2.0, 15.0), -60.0));
			const glm::dvec2 current = surface.Current;
			constexpr double Step = 1.0e-4;
			for (const glm::dvec2 rest : { glm::dvec2(-40.0, 15.0), glm::dvec2(-31.7, 22.9), glm::dvec2(-55.3, 4.1), glm::dvec2(-12.0, -6.5) })
			{
				for (const double time : { 0.0, 2.5, 611.75 })
				{
					const glm::dvec3 displaced = DisplaceWaterSurfacePoint(surface, rest, time);
					const glm::dvec2 point(displaced.x, displaced.z);
					const WaterSurfaceSample sample = EvaluateWaterSurface(surface, point, time);
					// The gradient of the sampled height, by central differences, and the normal it implies.
					const double dx = (EvaluateWaterSurface(surface, point + glm::dvec2(Step, 0.0), time).Height
										  - EvaluateWaterSurface(surface, point - glm::dvec2(Step, 0.0), time).Height)
						/ (2.0 * Step);
					const double dz = (EvaluateWaterSurface(surface, point + glm::dvec2(0.0, Step), time).Height
										  - EvaluateWaterSurface(surface, point - glm::dvec2(0.0, Step), time).Height)
						/ (2.0 * Step);
					const glm::dvec3 expectedNormal = glm::normalize(glm::dvec3(-dx, 1.0, -dz));
					CAPTURE(rest.x);
					CAPTURE(time);
					CHECK(glm::length(sample.Normal - expectedNormal) < 1.0e-5);
					CHECK(std::abs(glm::length(sample.Normal) - 1.0) < 1.0e-12);
					// The surface particle's position's time derivative, plus the current.
					const glm::dvec3 derivative = (DisplaceWaterSurfacePoint(surface, rest, time + Step) - DisplaceWaterSurfacePoint(surface, rest, time - Step)) / (2.0 * Step);
					const glm::dvec3 expectedVelocity = derivative + glm::dvec3(current.x, 0.0, current.y);
					CHECK(glm::length(sample.Velocity - expectedVelocity) < 1.0e-5);
				}
			}
		}

		TEST_CASE("WaterWaves: the steepness is scaled so the surface never folds")
		{
			const WaterFrame frame = MakeYawFrame(glm::dvec3(0.0), 0.0);
			const std::vector<WaterWave> swell = MakeSteepSwell();
			const double authored = ComputeWaterSteepnessSum(swell, 1.0);
			REQUIRE(authored > MaxWaterSteepnessSum);
			const WaterSurfaceDesc steep = ResolveWaterSurface(swell, 1.0, glm::dvec2(0.0), frame);
			CHECK(steep.SteepnessFactor == doctest::Approx(MaxWaterSteepnessSum / authored).epsilon(1e-12));
			CHECK(SteepnessSum(steep) == doctest::Approx(MaxWaterSteepnessSum).epsilon(1e-12));
			// Every wave scaled by the one factor; the amplitudes kept.
			for (uint32_t index = 0; index < steep.WaveCount; ++index)
			{
				CHECK(steep.Waves[index].Steepness == doctest::Approx(swell[index].Steepness * steep.SteepnessFactor).epsilon(1e-12));
				CHECK(steep.Waves[index].Amplitude == doctest::Approx(static_cast<double>(swell[index].Amplitude)).epsilon(1e-12));
			}
			// The displaced grid keeps its orientation everywhere: the horizontal map's Jacobian determinant stays positive.
			constexpr double Step = 1.0e-3;
			double smallest = 1.0;
			for (int x = 0; x < 60; ++x)
			{
				for (int z = 0; z < 60; ++z)
				{
					const glm::dvec2 rest(x * 0.41, z * 0.37);
					const glm::dvec3 base = DisplaceWaterSurfacePoint(steep, rest, 1.5);
					const glm::dvec3 alongX = DisplaceWaterSurfacePoint(steep, rest + glm::dvec2(Step, 0.0), 1.5);
					const glm::dvec3 alongZ = DisplaceWaterSurfacePoint(steep, rest + glm::dvec2(0.0, Step), 1.5);
					const double determinant = ((alongX.x - base.x) * (alongZ.z - base.z) - (alongX.z - base.z) * (alongZ.x - base.x)) / (Step * Step);
					smallest = std::min(smallest, determinant);
				}
			}
			CAPTURE(smallest);
			CHECK(smallest > 1.0 - MaxWaterSteepnessSum - 1.0e-2);
			// Within the limit, nothing is scaled.
			const WaterSurfaceDesc gentle = ResolveWaterSurface(swell, 0.25, glm::dvec2(0.0), frame);
			CHECK(ComputeWaterSteepnessSum(swell, 0.25) < MaxWaterSteepnessSum);
			CHECK(gentle.SteepnessFactor == 1.0);
			CHECK(gentle.Waves[0].Steepness == 1.0);
		}

		TEST_CASE("WaterWaves: each wave travels along its turned direction at its speed, deep water by dispersion")
		{
			const std::vector<WaterWave> waves = {
				WaterWave{ .Direction = 0.0f, .Wavelength = 10.0f, .Amplitude = 0.5f, .Steepness = 0.0f, .Speed = 0.0f, .Phase = 90.0f },
				WaterWave{ .Direction = 90.0f, .Wavelength = 20.0f, .Amplitude = 0.5f, .Steepness = 0.0f, .Speed = 5.0f, .Phase = 0.0f },
			};
			// Unturned: 0 degrees is +X, 90 degrees is -Z.
			const WaterSurfaceDesc level = ResolveWaterSurface(waves, 1.0, glm::dvec2(0.0), MakeYawFrame(glm::dvec3(0.0), 0.0));
			CHECK(glm::length(level.Waves[0].Direction - glm::dvec2(1.0, 0.0)) < 1.0e-12);
			CHECK(glm::length(level.Waves[1].Direction - glm::dvec2(0.0, -1.0)) < 1.0e-12);
			const double k0 = 2.0 * std::numbers::pi / 10.0;
			const double k1 = 2.0 * std::numbers::pi / 20.0;
			CHECK(level.Waves[0].WaveNumber == doctest::Approx(k0).epsilon(1e-14));
			CHECK(level.Waves[0].AngularFrequency == doctest::Approx(std::sqrt(WaterGravity * k0)).epsilon(1e-14));
			CHECK(level.Waves[1].AngularFrequency == doctest::Approx(5.0 * k1).epsilon(1e-14));
			// Turned a quarter about +Y, local +X is world -Z.
			const WaterSurfaceDesc turned = ResolveWaterSurface(waves, 1.0, glm::dvec2(2.0, 0.0), MakeYawFrame(glm::dvec3(0.0), 90.0));
			CHECK(glm::length(turned.Waves[0].Direction - glm::dvec2(0.0, -1.0)) < 1.0e-6);
			CHECK(glm::length(turned.Current - glm::dvec2(0.0, -2.0)) < 1.0e-5);
			// A sine wave (Q 0) of the first alone: its crest at phase 90 degrees sits on the origin at time 0 and travels +X.
			const WaterSurfaceDesc sine = ResolveWaterSurface(std::span<const WaterWave>(waves.data(), 1), 1.0, glm::dvec2(0.0), MakeYawFrame(glm::dvec3(0.0), 0.0));
			CHECK(EvaluateWaterSurface(sine, glm::dvec2(0.0), 0.0).Height == doctest::Approx(0.5).epsilon(1e-12));
			const double speed = level.Waves[0].AngularFrequency / k0;
			CHECK(EvaluateWaterSurface(sine, glm::dvec2(speed * 3.0, 0.0), 3.0).Height == doctest::Approx(0.5).epsilon(1e-9));
			// The renderer's phases at a time: p - w t wrapped into [0, 2 pi).
			const std::array<double, MaxWaterSurfaceWaves> phases = ComputeWaterWavePhases(level, 1234.5);
			for (uint32_t index = 0; index < level.WaveCount; ++index)
			{
				const double expected = level.Waves[index].Phase - level.Waves[index].AngularFrequency * 1234.5;
				CHECK(phases[index] >= 0.0);
				CHECK(phases[index] < 2.0 * std::numbers::pi);
				CHECK(std::abs(std::remainder(phases[index] - expected, 2.0 * std::numbers::pi)) < 1.0e-9);
			}
			CHECK(phases[2] == 0.0);
		}

		TEST_CASE("WaterWaves: a water's frame is its translation and its yaw alone")
		{
			// A yaw of 40 degrees, then a pitch of 25 about the turned X axis and a non-uniform scale: the frame is the yaw's.
			const glm::mat4 yawed = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, -1.0f, 2.0f)), glm::radians(40.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			const glm::mat4 pitched = glm::scale(glm::rotate(yawed, glm::radians(25.0f), glm::vec3(1.0f, 0.0f, 0.0f)), glm::vec3(2.0f, 1.0f, 3.0f));
			const WaterFrame frame = MakeWaterFrame(glm::dmat4(pitched));
			CHECK(frame.Origin == glm::dvec3(5.0, -1.0, 2.0));
			const double yaw = glm::radians(40.0);
			CHECK(glm::length(frame.AxisX - glm::dvec2(std::cos(yaw), -std::sin(yaw))) < 1.0e-6);
			CHECK(glm::length(frame.AxisZ - glm::dvec2(std::sin(yaw), std::cos(yaw))) < 1.0e-6);
			// An X axis turned straight up takes the frame from the Z axis.
			const glm::mat4 upright = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
			const WaterFrame fromZ = MakeWaterFrame(glm::dmat4(upright));
			CHECK(glm::length(fromZ.AxisZ - glm::dvec2(0.0, 1.0)) < 1.0e-6);
			CHECK(glm::length(fromZ.AxisX - glm::dvec2(1.0, 0.0)) < 1.0e-6);
		}
	}

}
