#include "TestsPCH.h"

#include "Support/SceneTestFixture.h"

#include "Engine/Scene/Components/BuiltinComponents.h"

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("SceneTestFixture: provides a frozen registry, a seeded generator and an empty scene" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture(7);
			CHECK(fixture.GetRegistry().IsFrozen());
			CHECK(fixture.GetRegistry().AreComponentsRegistered(BuiltinComponents{}));
			CHECK(fixture.GetRegistry().FindStruct("ProjectSettings") != nullptr);

			Scene& scene = fixture.GetScene();
			CHECK(scene.GetName() == "Test");
			CHECK(scene.GetSeed() == 0u);
			CHECK_FALSE(scene.IsRuntime());
			CHECK(scene.GetEntityCount() == 0);
			CHECK(&scene.GetTypeRegistry() == &fixture.GetRegistry());
			CHECK(&scene.GetUUIDGenerator() == &fixture.GetGenerator());

			// The same seed gives the same IDs, so tests that create entities are deterministic.
			Test::SceneTestFixture again(7);
			CHECK(fixture.GetGenerator().Next() == again.GetGenerator().Next());

			const Scope<Scene> second = fixture.CreateEmptyScene(true);
			CHECK(second->IsRuntime());
			CHECK(&second->GetTypeRegistry() == &fixture.GetRegistry());
			CHECK(Test::SceneTestFixture(1, true).GetScene().IsRuntime());
		}
	}

}
