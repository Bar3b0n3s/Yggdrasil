#include "TestsPCH.h"

#include "Support/EditorTestFixture.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

namespace Engine {

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

		TEST_CASE("EditorTestFixture: creates and opens a project and a scene" * doctest::skip(true))
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
