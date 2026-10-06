#include "TestsPCH.h"

#include "Engine/Scene/ComponentAccess.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/GlmApprox.h"
#include "Support/SceneTestFixture.h"

#include <nlohmann/json.hpp>

#include <limits>

namespace Engine {

	static Json ParseAccessJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("ComponentAccess: components are added, read, patched and removed by name" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			const Entity entity = fixture.GetScene().CreateEntity("Ball");

			const Json initial = ParseAccessJson(R"({ "Mass": 3, "MotionQuality": "LinearCast" })");
			REQUIRE(ComponentAccess::AddComponent(entity, "RigidBody", &initial).has_value());
			REQUIRE(entity.HasComponent<RigidBodyComponent>());
			CHECK(entity.GetComponent<RigidBodyComponent>().Mass == 3.0f);
			CHECK(entity.GetComponent<RigidBodyComponent>().MotionQuality == MotionQuality::LinearCast);

			REQUIRE(ComponentAccess::PatchComponentJson(entity, "RigidBody", ParseAccessJson(R"({ "Friction": 0.8 })")).has_value());
			const Result<Json> json = ComponentAccess::GetComponentJson(entity, "RigidBody");
			REQUIRE(json.has_value());
			CHECK((*json)["Mass"] == 3);
			CHECK(JsonReader((*json)["Friction"]).ReadFloat().value_or(0.0f) == doctest::Approx(0.8f));

			REQUIRE(ComponentAccess::RemoveComponent(entity, "RigidBody").has_value());
			CHECK_FALSE(entity.HasComponent<RigidBodyComponent>());
		}

		TEST_CASE("ComponentAccess: unknown names fail with suggestions" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			const Entity entity = fixture.GetScene().CreateEntity("Ball");
			const Result<Json> unknown = ComponentAccess::GetComponentJson(entity, "RigidBdy");
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::NotFound);
			CHECK(unknown.error().GetHint() == "did you mean 'RigidBody'?");

			const Result<Value> field = ComponentAccess::GetFieldValue(entity, "Transform", "Translaton");
			REQUIRE_FALSE(field.has_value());
			CHECK(field.error().GetCode() == ErrorCode::NotFound);
			CHECK(field.error().GetHint().find("Translation") != std::string::npos);

			const Status entityLevel = ComponentAccess::RemoveComponent(entity, "Name");
			REQUIRE_FALSE(entityLevel.has_value());
			CHECK(entityLevel.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("ComponentAccess: writes are validated and atomic" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			const Entity entity = fixture.GetScene().CreateEntity("Ball");
			const uint64_t revision = fixture.GetScene().GetRevision();

			const Status nan = ComponentAccess::SetFieldValue(entity, "Transform", "Translation",
				Value::FromVec3(glm::vec3(0.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f)));
			REQUIRE_FALSE(nan.has_value());
			CHECK(nan.error().GetCode() == ErrorCode::Validation);

			const Json zeroScale = ParseAccessJson(R"({ "Translation": [1, 2, 3], "Scale": [1, 0, 1] })");
			const Status scale = ComponentAccess::PatchComponentJson(entity, "Transform", zeroScale);
			REQUIRE_FALSE(scale.has_value());
			CHECK(entity.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
			CHECK(fixture.GetScene().GetRevision() == revision);
		}

		TEST_CASE("ComponentAccess: Requires, Excludes and UniquePerScene are enforced" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity character = scene.CreateEntity("Character");
			REQUIRE(ComponentAccess::AddComponent(character, "CharacterController", nullptr).has_value());
			const Status excluded = ComponentAccess::AddComponent(character, "RigidBody", nullptr);
			REQUIRE_FALSE(excluded.has_value());
			CHECK(excluded.error().GetCode() == ErrorCode::InvalidState);

			const Entity first = scene.CreateEntity("Sky");
			const Entity second = scene.CreateEntity("Sky2");
			REQUIRE(ComponentAccess::AddComponent(first, "Environment", nullptr).has_value());
			const Status unique = ComponentAccess::AddComponent(second, "Environment", nullptr);
			REQUIRE_FALSE(unique.has_value());
			CHECK(unique.error().GetCode() == ErrorCode::InvalidState);

			const Status required = ComponentAccess::RemoveComponent(first, "Transform");
			REQUIRE_FALSE(required.has_value());
			CHECK(required.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("ComponentAccess: engine-maintained Hidden components are readable but never written" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			const Entity entity = fixture.GetScene().CreateEntity("Member");

			// A PrefabLink naming an arbitrary root, or a Prefab marker, would be a structural defect (§6).
			for (const std::string_view hidden : { std::string_view("PrefabLink"), std::string_view("Prefab") })
			{
				INFO(std::string(hidden));
				const Status added = ComponentAccess::AddComponent(entity, hidden, nullptr);
				REQUIRE_FALSE(added.has_value());
				CHECK(added.error().GetCode() == ErrorCode::InvalidArgument);
			}
			CHECK_FALSE(entity.HasComponent<PrefabLinkComponent>());
			CHECK_FALSE(entity.HasComponent<PrefabInstanceComponent>());

			// PrefabInstantiator adds them directly; by name they can then be read, but not removed or changed.
			entity.AddComponent<PrefabInstanceComponent>();
			CHECK(ComponentAccess::GetComponentJson(entity, "Prefab").has_value());
			const Status removed = ComponentAccess::RemoveComponent(entity, "Prefab");
			REQUIRE_FALSE(removed.has_value());
			CHECK(removed.error().GetCode() == ErrorCode::InvalidArgument);
			const Status patched = ComponentAccess::PatchComponentJson(entity, "Prefab", ParseAccessJson(R"({ "Overrides": [] })"));
			REQUIRE_FALSE(patched.has_value());
			CHECK(patched.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(entity.HasComponent<PrefabInstanceComponent>());
		}

		TEST_CASE("ComponentAccess: virtual Transform fields are read and written" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity parent = scene.CreateEntity("Parent");
			const Entity child = scene.CreateEntity("Child", parent);
			parent.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 10.0f, 0.0f);
			});

			REQUIRE(ComponentAccess::SetFieldValue(child, "Transform", "WorldPosition", Value::FromVec3(glm::vec3(1.0f, 12.0f, 0.0f))).has_value());
			CHECK(Test::ApproxEqual(child.GetComponent<TransformComponent>().Translation, glm::vec3(1.0f, 2.0f, 0.0f)));

			const Result<Value> world = ComponentAccess::GetFieldValue(child, "Transform", "WorldPosition");
			REQUIRE(world.has_value());
			CHECK(Test::ApproxEqual(world->AsVec3(), glm::vec3(1.0f, 12.0f, 0.0f)));

			const Status readOnly = ComponentAccess::SetFieldValue(child, "Transform", "WorldScale", Value::FromVec3(glm::vec3(2.0f)));
			REQUIRE_FALSE(readOnly.has_value());
			CHECK(readOnly.error().GetCode() == ErrorCode::InvalidState);
		}
	}

}
