#include "TestsPCH.h"

#include "Support/EditorTestFixture.h"

#include "EditorCore/Automation/RegisterMethods.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

namespace Engine {

	namespace Test {

		// Registry struct "FixtureExtra": the type the registration hook test adds.
		struct FixtureExtra
		{
			int32_t Value = 0;
		};

	}

	static void RegisterFixtureTestTypes(TypeRegistry& registry)
	{
		RegisterEditorMethodTypes(registry);
		registry.Struct<Test::FixtureExtra>("FixtureExtra", "A type only the fixture test registers.")
			.Field("Value", &Test::FixtureExtra::Value, "A number.");
	}

	TEST_SUITE("Support")
	{
		TEST_CASE("EditorTestFixture: starts an editor in the launcher state over a registry with the editor's types")
		{
			Test::EditorTestFixture fixture("FixtureLauncher");
			CHECK_FALSE(fixture.GetEditor().HasProject());
			CHECK(fixture.GetEngine().GetTypeRegistry().IsFrozen());
			CHECK(fixture.GetEngine().GetTypeRegistry().AreComponentsRegistered(BuiltinComponents{}));
			CHECK(fixture.GetEngine().GetVfs().IsMounted("user"));
			CHECK(fixture.GetProjectRoot() == fixture.GetDirectory() / "TestProject");
			CHECK(fixture.GetEditor().GetSpecification().IdGeneratorState == Test::EditorTestIdState);
		}

		TEST_CASE("EditorTestFixture: a registration hook adds the test's own types to the editor's")
		{
			Test::EditorTestFixture fixture("FixtureTypes", {}, &RegisterFixtureTestTypes);
			const TypeRegistry& registry = fixture.GetEngine().GetTypeRegistry();
			CHECK(registry.FindStruct("FixtureExtra") != nullptr);
			CHECK(registry.AreComponentsRegistered(BuiltinComponents{}));
			CHECK(Test::EditorTestFixture("FixtureDefaultTypes").GetEngine().GetTypeRegistry().FindStruct("FixtureExtra") == nullptr);
		}

		TEST_CASE("EditorTestFixture: creates and opens a project and a scene")
		{
			Test::EditorTestFixture fixture("FixtureProject");
			fixture.CreateAndOpenProject("Game");
			CHECK(fixture.GetEditor().HasProject());
			CHECK(FileSystem::Exists(fixture.GetProjectRoot("Game") / "Game.eproj"));
			fixture.CreateAndOpenScene("Assets/Scenes/Level.scene");
			REQUIRE(fixture.GetEditor().HasScene());
			CHECK(fixture.GetEditor().GetScene().GetName() == "Level");
			CHECK(FileSystem::Exists(fixture.GetProjectRoot("Game") / "Assets/Scenes/Level.scene"));
		}
	}

}
