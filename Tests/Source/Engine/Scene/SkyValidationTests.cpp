#include "TestsPCH.h"

#include "Engine/Scene/SkyValidation.h"

#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/SkyComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/SceneTestFixture.h"

#include <vector>

// The sky checks (M45, Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; Scene/SkyValidation.h).

namespace Engine {

	namespace {

		std::vector<std::string> GetFields(const std::vector<SkySceneIssue>& issues, std::string_view code)
		{
			std::vector<std::string> fields;
			for (const SkySceneIssue& issue : issues)
				if (issue.Code == code)
					fields.push_back(issue.Field);
			return fields;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("SkyValidation: a Sun or Moon that names no usable light, and an Environment beside a Sky, are reported")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity camera = scene.CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			// No Sky: nothing, an Environment included.
			Entity environment = scene.CreateEntity("Environment");
			environment.AddComponent<EnvironmentComponent>();
			CHECK(FindSkyIssues(scene).empty());

			// A Sky with a null Sun in a scene without a DirectionalLight, beside the Environment.
			Entity skyEntity = scene.CreateEntity("Sky");
			skyEntity.AddComponent<SkyComponent>();
			std::vector<SkySceneIssue> issues = FindSkyIssues(scene);
			REQUIRE(issues.size() == 2);
			CHECK(issues[0].Code == SkyLightInvalidCode);
			CHECK(issues[0].Severity == DiagnosticSeverity::Warning);
			CHECK(issues[0].Entity == skyEntity.GetUUID());
			CHECK(issues[0].Component == "Sky");
			CHECK(issues[0].Field == "Sun");
			CHECK_FALSE(issues[0].Hint.empty());
			CHECK(issues[1].Code == SkyEnvironmentIgnoredCode);
			CHECK(issues[1].Entity == environment.GetUUID());
			CHECK(issues[1].Component == "Environment");

			// A disabled Environment is not reported; a DirectionalLight gives the null Sun its light.
			environment.SetActive(false);
			Entity light = scene.CreateEntity("Light");
			light.AddComponent<DirectionalLightComponent>();
			CHECK(FindSkyIssues(scene).empty());

			// A Sun naming an entity without a DirectionalLight, or a disabled light.
			skyEntity.Patch<SkyComponent>([&camera](SkyComponent& sky)
			{
				sky.Sun = camera.GetUUID();
			});
			CHECK(GetFields(FindSkyIssues(scene), SkyLightInvalidCode) == std::vector<std::string>{ "Sun" });
			skyEntity.Patch<SkyComponent>([&light](SkyComponent& sky)
			{
				sky.Sun = light.GetUUID();
			});
			CHECK(FindSkyIssues(scene).empty());
			light.SetActive(false);
			CHECK(GetFields(FindSkyIssues(scene), SkyLightInvalidCode) == std::vector<std::string>{ "Sun" });
			light.SetActive(true);

			// A Moon that is the Sun's entity, named or resolved from a null Sun.
			skyEntity.Patch<SkyComponent>([&light](SkyComponent& sky)
			{
				sky.Moon = light.GetUUID();
			});
			CHECK(GetFields(FindSkyIssues(scene), SkyLightInvalidCode) == std::vector<std::string>{ "Moon" });
			skyEntity.Patch<SkyComponent>([](SkyComponent& sky)
			{
				sky.Sun = UUID();
			});
			CHECK(GetFields(FindSkyIssues(scene), SkyLightInvalidCode) == std::vector<std::string>{ "Moon" });
			// A Moon on another entity is fine, light or not.
			skyEntity.Patch<SkyComponent>([&camera](SkyComponent& sky)
			{
				sky.Moon = camera.GetUUID();
			});
			CHECK(FindSkyIssues(scene).empty());

			// A Sun or Moon naming no entity is ENTITY_DANGLING_REFERENCE's, never this code's.
			skyEntity.Patch<SkyComponent>([](SkyComponent& sky)
			{
				sky.Sun = UUID(0x5ca1ab1e);
				sky.Moon = UUID(0x5ca1ab1f);
			});
			CHECK(FindSkyIssues(scene).empty());

			// A disabled Sky is not checked.
			skyEntity.Patch<SkyComponent>([&camera](SkyComponent& sky)
			{
				sky.Sun = camera.GetUUID();
			});
			environment.SetActive(true);
			CHECK(FindSkyIssues(scene).size() == 2);
			skyEntity.SetActive(false);
			CHECK(FindSkyIssues(scene).empty());
		}
	}

}
