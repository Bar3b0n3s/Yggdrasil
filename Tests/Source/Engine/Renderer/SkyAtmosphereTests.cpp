#include "TestsPCH.h"

#include "Engine/Renderer/SkyAtmosphere.h"

#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/SkyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/SceneTestFixture.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <vector>

// The sky's CPU part (M45, Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; Renderer/SkyAtmosphere.h): the
// atmosphere's transmittance, the moon's phase, the stars' frame, the driven light colours and the cascade order.

namespace Engine {

	namespace {

		// The unit direction toward a source at `elevation` degrees above the horizon, toward +Z turned by `azimuth` degrees
		// about +Y.
		glm::dvec3 MakeSkyDirection(double elevation, double azimuth = 0.0)
		{
			const double e = elevation * std::numbers::pi / 180.0;
			const double a = azimuth * std::numbers::pi / 180.0;
			return glm::dvec3(std::cos(e) * std::sin(a), std::sin(e), std::cos(e) * std::cos(a));
		}

		double Luminance(const glm::dvec3& color)
		{
			return 0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b;
		}

		// `vector` turned by `angle` radians about the unit `axis` (Rodrigues).
		glm::dvec3 RotateAbout(const glm::dvec3& vector, const glm::dvec3& axis, double angle)
		{
			return vector * std::cos(angle) + glm::cross(axis, vector) * std::sin(angle) + axis * glm::dot(axis, vector) * (1.0 - std::cos(angle));
		}

		void CheckClose(const glm::dvec3& actual, const glm::dvec3& expected, double tolerance = 1e-9)
		{
			CHECK(glm::length(actual - expected) <= tolerance);
		}

		// A directional light entity whose light shines toward -(the direction `elevation` degrees up, toward +Z): its local -Z
		// turned about X.
		Entity CreateSkyLight(Scene& scene, std::string_view name, const glm::vec3& color, float intensity, float elevation)
		{
			Entity light = scene.CreateEntity(name);
			light.Patch<TransformComponent>([elevation](TransformComponent& transform)
			{
				transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(180.0f + elevation, 0.0f, 0.0f));
			});
			light.AddComponent<DirectionalLightComponent>(DirectionalLightComponent{ .Color = color, .Intensity = intensity });
			return light;
		}

		const LightData& FindLight(const RenderSnapshot& snapshot, UUID entity)
		{
			for (const LightData& light : snapshot.Lights)
				if (light.Entity == entity)
					return light;
			FAIL("no light for the entity");
			return snapshot.Lights.front();
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("SkyAtmosphere: the transmittance toward the sun falls with the zenith angle, reddens at the horizon and is zero below it")
		{
			const double ground = ComputeSkyCameraAltitude(glm::vec3(0.0f, -5.0f, 0.0f));
			CHECK(ground == SkyCameraHeightOffset);
			CHECK(ComputeSkyCameraAltitude(glm::vec3(0.0f, 2000.0f, 0.0f)) == doctest::Approx(2.0 + SkyCameraHeightOffset));

			// Monotone from the zenith to the horizon, in every channel.
			const std::array<double, 7> elevations = { 90.0, 60.0, 30.0, 15.0, 5.0, 2.0, 0.5 };
			glm::dvec3 previous(2.0);
			for (const double elevation : elevations)
			{
				CAPTURE(elevation);
				const glm::dvec3 transmittance = ComputeSkyTransmittance(ground, MakeSkyDirection(elevation), 1.0f, 0.0f);
				for (glm::length_t channel = 0; channel < 3; ++channel)
				{
					CHECK(transmittance[channel] > 0.0);
					CHECK(transmittance[channel] < previous[channel]);
				}
				previous = transmittance;
			}

			// Clear at the zenith (Rayleigh's blue scatters most), red at the horizon: red over blue grows as the sun sets.
			const glm::dvec3 zenith = ComputeSkyTransmittance(ground, MakeSkyDirection(90.0), 1.0f, 0.0f);
			const glm::dvec3 horizon = ComputeSkyTransmittance(ground, MakeSkyDirection(1.0), 1.0f, 0.0f);
			CHECK(zenith.r > 0.9);
			CHECK(zenith.b > 0.6);
			CHECK(zenith.r > zenith.g);
			CHECK(zenith.g > zenith.b);
			CHECK(horizon.r / horizon.b > 10.0 * (zenith.r / zenith.b));

			// Zero below the horizon, also for a direction straight down; a ray from altitude that clears the ground's curve
			// is not below it.
			CHECK(ComputeSkyTransmittance(ground, MakeSkyDirection(-1.0), 1.0f, 0.0f) == glm::dvec3(0.0));
			CHECK(ComputeSkyTransmittance(ground, glm::dvec3(0.0, -1.0, 0.0), 1.0f, 0.0f) == glm::dvec3(0.0));
			CHECK(Luminance(ComputeSkyTransmittance(10.0, MakeSkyDirection(-0.5), 1.0f, 0.0f)) > 0.0);
			CHECK(ComputeSkyTransmittance(ground, glm::dvec3(0.0), 1.0f, 0.0f) == glm::dvec3(0.0));

			// Haze and Overcast thicken it: a full overcast leaves a few percent of the sun at the zenith.
			CHECK(Luminance(ComputeSkyTransmittance(ground, MakeSkyDirection(30.0), 5.0f, 0.0f)) < Luminance(ComputeSkyTransmittance(ground, MakeSkyDirection(30.0), 1.0f, 0.0f)));
			const double overcast = Luminance(ComputeSkyTransmittance(ground, MakeSkyDirection(90.0), 1.0f, 1.0f));
			CHECK(overcast > 0.0);
			CHECK(overcast < 0.1);

			// Pure: the same inputs give the same bits.
			CHECK(ComputeSkyTransmittance(ground, MakeSkyDirection(7.0), 1.5f, 0.25f) == ComputeSkyTransmittance(ground, MakeSkyDirection(7.0), 1.5f, 0.25f));
		}

		TEST_CASE("SkyAtmosphere: the moon's illuminated fraction follows its elongation from the sun")
		{
			const glm::dvec3 sun = MakeSkyDirection(20.0, 40.0);
			// The moon at elongation psi from the sun: (1 - cos psi) / 2.
			const glm::dvec3 axis = glm::normalize(glm::cross(sun, glm::dvec3(0.0, 1.0, 0.0)));
			double previous = -1.0;
			for (const double degrees : { 0.0, 30.0, 60.0, 90.0, 120.0, 150.0, 180.0 })
			{
				CAPTURE(degrees);
				const double psi = degrees * std::numbers::pi / 180.0;
				const double fraction = ComputeMoonIlluminatedFraction(sun, RotateAbout(sun, axis, psi));
				CHECK(fraction == doctest::Approx((1.0 - std::cos(psi)) * 0.5).epsilon(1e-12));
				CHECK(fraction > previous);
				previous = fraction;
			}
			CHECK(ComputeMoonIlluminatedFraction(sun, sun) == doctest::Approx(0.0));
			CHECK(ComputeMoonIlluminatedFraction(sun, -sun) == doctest::Approx(1.0));
			CHECK(ComputeMoonIlluminatedFraction(sun, RotateAbout(sun, axis, std::numbers::pi / 2.0)) == doctest::Approx(0.5));
			// Lengths do not matter; a zero direction gives none.
			CHECK(ComputeMoonIlluminatedFraction(sun * 3.0, -sun * 0.5) == doctest::Approx(1.0));
			CHECK(ComputeMoonIlluminatedFraction(sun, glm::dvec3(0.0)) == 0.0);
		}

		TEST_CASE("SkyAtmosphere: the stars turn about the celestial pole by the sun's hour angle")
		{
			for (const double latitude : { 45.0, -30.0, 90.0, 0.0 })
			{
				CAPTURE(latitude);
				const double northAngle = 30.0;
				const glm::dvec3 pole = ComputeSkyPole(latitude, northAngle);
				CHECK(glm::length(pole) == doctest::Approx(1.0));
				CHECK(pole.y == doctest::Approx(std::sin(latitude * std::numbers::pi / 180.0)));
				// North is -Z turned 30 degrees counter-clockwise seen from above (toward -X).
				if (latitude != 90.0)
					CHECK(glm::dot(glm::normalize(glm::dvec3(pole.x, 0.0, pole.z)), glm::dvec3(-0.5, 0.0, -std::sqrt(3.0) / 2.0)) == doctest::Approx(1.0));

				const glm::dvec3 sun = glm::normalize(MakeSkyDirection(35.0, 160.0));
				const glm::dmat3 frame = ComputeSkyStarFrame(sun, latitude, northAngle);
				// A rotation, with the pole as the celestial +Z.
				const glm::dmat3 product = frame * glm::transpose(frame);
				for (glm::length_t column = 0; column < 3; ++column)
					for (glm::length_t row = 0; row < 3; ++row)
						CHECK(product[column][row] == doctest::Approx(column == row ? 1.0 : 0.0).epsilon(1e-12));
				CheckClose(frame * pole, glm::dvec3(0.0, 0.0, 1.0), 1e-12);

				// The sun turned about the pole by h: the frame turns with it, so the sun keeps its celestial coordinates and every
				// star's world direction turns about the pole by h too.
				const std::array<glm::dvec3, 3> stars = { glm::normalize(glm::dvec3(1.0, 2.0, 3.0)), glm::normalize(glm::dvec3(-0.5, 0.1, 0.8)),
					glm::dvec3(0.0, 0.0, 1.0) };
				for (const double hours : { 1.0, 6.0, 13.5, 23.0 })
				{
					CAPTURE(hours);
					const double angle = hours / 24.0 * 2.0 * std::numbers::pi;
					const glm::dvec3 turned = RotateAbout(sun, pole, angle);
					const glm::dmat3 turnedFrame = ComputeSkyStarFrame(turned, latitude, northAngle);
					CheckClose(turnedFrame * turned, frame * sun, 1e-12);
					for (const glm::dvec3& star : stars)
						CheckClose(glm::transpose(turnedFrame) * star, RotateAbout(glm::transpose(frame) * star, pole, angle), 1e-12);
				}
			}
			// A zero sun takes the hour angle 0: still a rotation with the pole as +Z.
			const glm::dmat3 still = ComputeSkyStarFrame(glm::dvec3(0.0), 45.0, 0.0);
			CheckClose(still * ComputeSkyPole(45.0, 0.0), glm::dvec3(0.0, 0.0, 1.0), 1e-12);
		}

		TEST_CASE("SkyAtmosphere: driving the lights is idempotent and leaves the components' colours as authored")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity camera = scene.CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			camera.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 2.0f, 5.0f);
			});
			const glm::vec3 sunColor(1.0f, 0.95f, 0.9f);
			const glm::vec3 moonColor(0.6f, 0.7f, 1.0f);
			Entity sun = CreateSkyLight(scene, "Sun", sunColor, 6.0f, 8.0f);
			// The moon 40 degrees up toward +Z, opposite the sun's side: a gibbous moon.
			Entity moon = CreateSkyLight(scene, "Moon", moonColor, 0.2f, 0.0f);
			moon.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(-40.0f, 0.0f, 0.0f));
			});
			Entity skyEntity = scene.CreateEntity("Sky");
			skyEntity.AddComponent<SkyComponent>(SkyComponent{ .Sun = sun.GetUUID(), .Moon = moon.GetUUID(), .Haze = 2.0f });
			TransformSystem::Update(scene);

			const Result<RenderSnapshot> extracted = ExtractRenderSnapshot(scene, { .Width = 16, .Height = 16 });
			REQUIRE_MESSAGE(extracted.has_value(), extracted.error().ToString());
			RenderSnapshot snapshot = *extracted;
			REQUIRE(snapshot.Sky.Enabled);
			REQUIRE(snapshot.Sky.SunLight.has_value());
			REQUIRE(snapshot.Sky.MoonLight.has_value());
			const LightData& drivenSun = snapshot.Lights[*snapshot.Sky.SunLight];
			const LightData& drivenMoon = snapshot.Lights[*snapshot.Sky.MoonLight];
			CHECK(drivenSun.Entity == sun.GetUUID());
			CHECK(drivenMoon.Entity == moon.GetUUID());
			CHECK(snapshot.Sky.SunLightColor == sunColor);
			CHECK(snapshot.Sky.MoonLightColor == moonColor);

			// The Sun's colour times the transmittance toward it from the camera's altitude (2 m), low in the sky: dimmer and redder.
			const double altitude = ComputeSkyCameraAltitude(snapshot.Camera.Position);
			const glm::dvec3 toSun = -glm::dvec3(drivenSun.Direction);
			const glm::dvec3 sunTransmittance = ComputeSkyTransmittance(altitude, toSun, 2.0f, 0.0f);
			CHECK(drivenSun.Color == glm::vec3(glm::dvec3(sunColor) * sunTransmittance));
			CHECK(drivenSun.Color.b / sunColor.b < drivenSun.Color.r / sunColor.r);
			CHECK(drivenSun.Intensity == 6.0f);
			// The Moon light's colour also times its illuminated fraction.
			const glm::dvec3 toMoon = -glm::dvec3(drivenMoon.Direction);
			const double fraction = ComputeMoonIlluminatedFraction(toSun, toMoon);
			CHECK(fraction > 0.5);
			CHECK(fraction < 1.0);
			CHECK(drivenMoon.Color.b > 0.0f);
			CHECK(drivenMoon.Color == glm::vec3(glm::dvec3(moonColor) * ComputeSkyTransmittance(altitude, toMoon, 2.0f, 0.0f) * fraction));
			CheckClose(glm::dvec3(snapshot.Sky.MoonDirection), toMoon, 1e-6);

			// The components keep their authored colours.
			CHECK(sun.GetComponent<DirectionalLightComponent>().Color == sunColor);
			CHECK(moon.GetComponent<DirectionalLightComponent>().Color == moonColor);

			// Idempotent: driving again changes nothing; a sun turned in the snapshot and turned back drives to the same colours.
			const RenderSnapshot first = snapshot;
			ApplySkyLightDriving(snapshot);
			CHECK(snapshot.Lights[*snapshot.Sky.SunLight].Color == first.Lights[*first.Sky.SunLight].Color);
			CHECK(snapshot.Lights[*snapshot.Sky.MoonLight].Color == first.Lights[*first.Sky.MoonLight].Color);
			const glm::vec3 direction = snapshot.Lights[*snapshot.Sky.SunLight].Direction;
			snapshot.Lights[*snapshot.Sky.SunLight].Direction = glm::vec3(0.0f, -1.0f, 0.0f);
			ApplySkyLightDriving(snapshot);
			const glm::vec3 noon = snapshot.Lights[*snapshot.Sky.SunLight].Color;
			CHECK(Luminance(glm::dvec3(noon)) > Luminance(glm::dvec3(first.Lights[*first.Sky.SunLight].Color)));
			snapshot.Lights[*snapshot.Sky.SunLight].Direction = direction;
			ApplySkyLightDriving(snapshot);
			CHECK(snapshot.Lights[*snapshot.Sky.SunLight].Color == first.Lights[*first.Sky.SunLight].Color);
			// Below the horizon the Sun is dark.
			snapshot.Lights[*snapshot.Sky.SunLight].Direction = glm::vec3(0.0f, 1.0f, 0.0f);
			ApplySkyLightDriving(snapshot);
			CHECK(snapshot.Lights[*snapshot.Sky.SunLight].Color == glm::vec3(0.0f));

			// Without DriveLights, the authored colours.
			skyEntity.Patch<SkyComponent>([](SkyComponent& sky)
			{
				sky.DriveLights = false;
			});
			const Result<RenderSnapshot> undriven = ExtractRenderSnapshot(scene, { .Width = 16, .Height = 16 });
			REQUIRE_MESSAGE(undriven.has_value(), undriven.error().ToString());
			CHECK(FindLight(*undriven, sun.GetUUID()).Color == sunColor);
			CHECK(FindLight(*undriven, moon.GetUUID()).Color == moonColor);
		}

		TEST_CASE("SkyAtmosphere: the cascades go to the Sun above the horizon and to the Moon below it")
		{
			RenderSnapshot snapshot;
			const UUID other(5);
			const UUID moon(6);
			const UUID sun(7);
			snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Point, .Entity = UUID(4) });
			snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Directional, .Direction = glm::vec3(0.0f, -1.0f, 0.0f), .CastShadows = true, .Entity = other });
			snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Directional, .Direction = glm::vec3(0.0f, -1.0f, 0.0f), .CastShadows = true, .Entity = moon });
			snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Directional, .Direction = glm::normalize(glm::vec3(0.0f, -1.0f, 1.0f)), .CastShadows = true, .Entity = sun });

			// Without a Sky: every directional light in snapshot order (M23's rule).
			CHECK(OrderShadowCandidates(snapshot) == std::vector<uint32_t>{ 1, 2, 3 });

			snapshot.Sky.Enabled = true;
			snapshot.Sky.Sun = sun;
			snapshot.Sky.Moon = moon;
			ResolveSkyLights(snapshot);
			REQUIRE(snapshot.Sky.SunLight == std::optional<uint32_t>(3));
			REQUIRE(snapshot.Sky.MoonLight == std::optional<uint32_t>(2));
			// The Sun above the horizon (it shines down) first.
			CHECK(OrderShadowCandidates(snapshot) == std::vector<uint32_t>{ 3, 1, 2 });
			// Below the horizon (it shines up): the Moon first, the Sun with the rest.
			snapshot.Lights[3].Direction = glm::normalize(glm::vec3(0.0f, 1.0f, 1.0f));
			CHECK(OrderShadowCandidates(snapshot) == std::vector<uint32_t>{ 2, 1, 3 });
			// Without a Moon light, snapshot order again.
			snapshot.Sky.Moon = UUID();
			ResolveSkyLights(snapshot);
			CHECK_FALSE(snapshot.Sky.MoonLight.has_value());
			CHECK(OrderShadowCandidates(snapshot) == std::vector<uint32_t>{ 1, 2, 3 });
			// A disabled Sky changes nothing.
			snapshot.Sky.Moon = moon;
			ResolveSkyLights(snapshot);
			snapshot.Sky.Enabled = false;
			CHECK(OrderShadowCandidates(snapshot) == std::vector<uint32_t>{ 1, 2, 3 });
		}
	}

}
