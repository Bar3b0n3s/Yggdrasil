#include "TestsPCH.h"

#include "Engine/Scene/SkyExtraction.h"

#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/SkyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/SceneTestFixture.h"

#include <optional>
#include <string_view>

// The sky's extraction (M45, Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; Scene/SkyExtraction.h), through
// ExtractRenderSnapshot, which calls it after its canonical loop. Its acceptance case moved here from
// RenderExtractionTests.cpp at phase 8's landing, which merged M43's extraction cases there: the merged file passed the
// object format's section limit.

namespace Engine {

	namespace {

		Entity CreateCamera(Scene& scene, std::string_view name, bool primary)
		{
			Entity camera = scene.CreateEntity(name);
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = primary });
			return camera;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("RenderExtraction: a Sky is extracted with its Sun and Moon in every view, and a scene without one extracts as before")
		{
			// M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; Scene/SkyExtraction.h).
			Test::SceneTestFixture edit;
			Scene& scene = edit.GetScene();
			CreateCamera(scene, "Camera", true);
			Entity first = scene.CreateEntity("First");
			first.AddComponent<DirectionalLightComponent>(DirectionalLightComponent{ .Color = glm::vec3(0.5f, 0.5f, 1.0f) });
			Entity sun = scene.CreateEntity("Sun");
			sun.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(-60.0f, 0.0f, 0.0f));
			});
			sun.AddComponent<DirectionalLightComponent>(DirectionalLightComponent{ .Color = glm::vec3(1.0f, 0.9f, 0.8f), .Intensity = 4.0f });
			// A moon without a light: its local +Z turned 30 degrees up about X (toward +Y and +Z).
			Entity moon = scene.CreateEntity("Moon");
			moon.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(-30.0f, 0.0f, 0.0f));
			});
			TransformSystem::Update(scene);

			// Without a Sky: disabled, and the lights as authored.
			const Result<RenderSnapshot> plain = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(plain.has_value(), plain.error().ToString());
			CHECK_FALSE(plain->Sky.Enabled);
			CHECK(plain->Lights.size() == 2);
			CHECK(plain->Lights[1].Color == glm::vec3(1.0f, 0.9f, 0.8f));

			const SkyComponent authored{ .Sun = sun.GetUUID(),
				.Moon = moon.GetUUID(),
				.Intensity = 2.5f,
				.DriveLights = false,
				.Haze = 3.0f,
				.Overcast = 0.25f,
				.GroundAlbedo = glm::vec3(0.1f, 0.2f, 0.3f),
				.SunDisc = 4.0f,
				.MoonSize = 1.5f,
				.Stars = 7.0f,
				.Latitude = -20.0f,
				.NorthAngle = 15.0f,
				.Clouds = 0.5f,
				.CloudSpeed = glm::vec2(-3.0f, 2.0f) };
			Entity skyEntity = scene.CreateEntity("Sky");
			skyEntity.AddComponent<SkyComponent>(authored);
			for (const RenderCameraSource source : { RenderCameraSource::Primary, RenderCameraSource::Explicit })
			{
				CAPTURE(static_cast<int>(source));
				const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Camera = source, .Width = 8, .Height = 8, .SimulationTime = 12.5 });
				REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
				const RenderSky& sky = snapshot->Sky;
				CHECK(sky.Enabled);
				CHECK(sky.Entity == skyEntity.GetUUID());
				CHECK(sky.Sun == sun.GetUUID());
				CHECK(sky.Moon == moon.GetUUID());
				CHECK(sky.Intensity == 2.5f);
				CHECK_FALSE(sky.DriveLights);
				CHECK(sky.Haze == 3.0f);
				CHECK(sky.Overcast == 0.25f);
				CHECK(sky.GroundAlbedo == glm::vec3(0.1f, 0.2f, 0.3f));
				CHECK(sky.SunDisc == 4.0f);
				CHECK(sky.MoonSize == 1.5f);
				CHECK(sky.Stars == 7.0f);
				CHECK(sky.Latitude == -20.0f);
				CHECK(sky.NorthAngle == 15.0f);
				CHECK(sky.Clouds == 0.5f);
				CHECK(sky.CloudSpeed == glm::vec2(-3.0f, 2.0f));
				CHECK(sky.SimulationTime == 12.5);
				// The Sun is the second light, the named one; the Moon holds no light, so its direction is its local +Z.
				CHECK(sky.SunLight == std::optional<uint32_t>(1));
				CHECK(sky.SunLightColor == glm::vec3(1.0f, 0.9f, 0.8f));
				CHECK_FALSE(sky.MoonLight.has_value());
				CHECK(sky.HasMoon);
				const glm::vec3 forward = glm::normalize(glm::vec3(moon.GetComponent<WorldTransformComponent>().Matrix[2]));
				CHECK(glm::length(sky.MoonDirection - forward) < 1e-6f);
				CHECK(sky.MoonDirection.y > 0.4f);
			}

			// A null Sun takes the first directional light; a Moon that is the Sun's entity has no moon.
			skyEntity.Patch<SkyComponent>([&first](SkyComponent& sky)
			{
				sky.Sun = UUID();
				sky.Moon = first.GetUUID();
			});
			Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK(snapshot->Sky.SunLight == std::optional<uint32_t>(0));
			CHECK_FALSE(snapshot->Sky.MoonLight.has_value());
			CHECK_FALSE(snapshot->Sky.HasMoon);
			CHECK(snapshot->Sky.SimulationTime == 0.0);

			// A Moon light: its light's index, colour and direction.
			skyEntity.Patch<SkyComponent>([&sun](SkyComponent& sky)
			{
				sky.Sun = sun.GetUUID();
			});
			snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK(snapshot->Sky.MoonLight == std::optional<uint32_t>(0));
			CHECK(snapshot->Sky.MoonLightColor == glm::vec3(0.5f, 0.5f, 1.0f));
			CHECK(snapshot->Sky.HasMoon);
			CHECK(glm::length(snapshot->Sky.MoonDirection + snapshot->Lights[0].Direction) < 1e-6f);

			// A disabled Sky is not extracted.
			skyEntity.SetActive(false);
			snapshot = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK_FALSE(snapshot->Sky.Enabled);
			CHECK(snapshot->Lights[1].Color == glm::vec3(1.0f, 0.9f, 0.8f));
		}
	}

}
