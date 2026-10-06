#include "TestsPCH.h"

#include "Engine/Scene/TransformSystem.h"

#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/GlmApprox.h"
#include "Support/SceneTestFixture.h"

namespace Engine {

	// Reference rotation matrices written out by hand (no CRT trigonometry): 90 degrees about +Y maps +X to -Z.
	static glm::mat4 ReferenceRotationY90()
	{
		return glm::mat4(
			glm::vec4(0.0f, 0.0f, -1.0f, 0.0f),
			glm::vec4(0.0f, 1.0f, 0.0f, 0.0f),
			glm::vec4(1.0f, 0.0f, 0.0f, 0.0f),
			glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
	}

	// The unit quaternion of 90 degrees about +Y: (x, y, z, w) = (0, sqrt(1/2), 0, sqrt(1/2)).
	static glm::quat RotationY90()
	{
		const float half = 0.70710678118654752f;
		return glm::quat(half, 0.0f, half, 0.0f);
	}

	static void SetLocal(Entity entity, const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale)
	{
		entity.Patch<TransformComponent>([&](TransformComponent& transform)
		{
			transform.Translation = translation;
			transform.Rotation = rotation;
			transform.Scale = scale;
		});
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("TransformSystem: matches reference matrices" * doctest::skip(true))
		{
			TransformComponent local;
			local.Translation = glm::vec3(1.0f, 2.0f, 3.0f);
			local.Rotation = RotationY90();
			local.Scale = glm::vec3(2.0f, 1.0f, 0.5f);

			glm::mat4 expected = ReferenceRotationY90();
			expected[0] *= 2.0f;
			expected[2] *= 0.5f;
			expected[3] = glm::vec4(1.0f, 2.0f, 3.0f, 1.0f);
			CHECK(Test::ApproxEqual(TransformSystem::ComputeLocalMatrix(local), expected, 1e-6f));

			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity parent = scene.CreateEntity("Parent");
			const Entity child = scene.CreateEntity("Child", parent);
			SetLocal(parent, glm::vec3(10.0f, 0.0f, 0.0f), RotationY90(), glm::vec3(2.0f));
			SetLocal(child, glm::vec3(1.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));

			TransformSystem::Update(scene);
			// The child's local +X offset of 1 is rotated to -Z and scaled by 2: world (10, 0, -2).
			const glm::mat4& world = child.GetComponent<WorldTransformComponent>().Matrix;
			CHECK(Test::ApproxEqual(glm::vec3(world[3]), glm::vec3(10.0f, 0.0f, -2.0f), 1e-5f));
			CHECK(Test::ApproxEqual(world, TransformSystem::ComputeWorldMatrix(child), 1e-6f));
			CHECK(Test::ApproxEqual(parent.GetComponent<WorldTransformComponent>().Matrix,
				TransformSystem::ComputeLocalMatrix(parent.GetComponent<TransformComponent>()), 1e-6f));
		}

		TEST_CASE("Transform: WorldPosition and WorldRotation setters round-trip through the parent inverse" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity parent = scene.CreateEntity("Parent");
			const Entity child = scene.CreateEntity("Child", parent);
			SetLocal(parent, glm::vec3(5.0f, -1.0f, 2.0f), RotationY90(), glm::vec3(2.0f, 2.0f, 2.0f));

			const glm::vec3 position(3.0f, 4.0f, -7.0f);
			TransformSystem::SetWorldPosition(child, position);
			CHECK(Test::ApproxEqual(TransformSystem::GetWorldPosition(child), position, 1e-5f));

			const glm::quat rotation = glm::normalize(glm::quat(0.9f, 0.1f, 0.3f, -0.2f));
			TransformSystem::SetWorldRotation(child, rotation);
			const glm::quat roundTrip = TransformSystem::GetWorldRotation(child);
			// q and -q are the same rotation.
			CHECK((Test::ApproxEqual(roundTrip, rotation, 1e-5f) || Test::ApproxEqual(roundTrip, -rotation, 1e-5f)));
			CHECK(Test::ApproxEqual(TransformSystem::GetWorldPosition(child), position, 1e-5f)); // rotation keeps position

			// The local transform is what the parent inverse gives.
			const glm::vec3 local = child.GetComponent<TransformComponent>().Translation;
			const glm::vec4 back = TransformSystem::ComputeWorldMatrix(parent) * glm::vec4(local, 1.0f);
			CHECK(Test::ApproxEqual(glm::vec3(back), position, 1e-5f));
		}

		TEST_CASE("TransformSystem: Euler angles in degrees convert to quaternions and back" * doctest::skip(true))
		{
			const glm::quat yaw = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(0.0f, 90.0f, 0.0f));
			CHECK(Test::ApproxEqual(yaw, RotationY90(), 1e-6f));

			const glm::vec3 angles(30.0f, -45.0f, 60.0f);
			const glm::vec3 back = TransformSystem::EulerDegreesFromQuaternion(TransformSystem::QuaternionFromEulerDegrees(angles));
			CHECK(Test::ApproxEqual(back, angles, 1e-4f));
		}

		TEST_CASE("TransformSystem: DecomposeMatrix recovers translation, rotation and scale" * doctest::skip(true))
		{
			TransformComponent local;
			local.Translation = glm::vec3(-1.0f, 2.0f, 0.5f);
			local.Rotation = RotationY90();
			local.Scale = glm::vec3(3.0f, 1.0f, 2.0f);
			const Result<TransformDecomposition> decomposed = TransformSystem::DecomposeMatrix(TransformSystem::ComputeLocalMatrix(local));
			REQUIRE(decomposed.has_value());
			CHECK(Test::ApproxEqual(decomposed->Translation, local.Translation, 1e-5f));
			CHECK(Test::ApproxEqual(decomposed->Rotation, local.Rotation, 1e-5f));
			CHECK(Test::ApproxEqual(decomposed->Scale, local.Scale, 1e-5f));

			glm::mat4 degenerate(1.0f);
			degenerate[1] = glm::vec4(0.0f);
			CHECK_FALSE(TransformSystem::DecomposeMatrix(degenerate).has_value());
		}

		TEST_CASE("TransformSystem: render pose equals the world pose outside play" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			const Entity entity = fixture.GetScene().CreateEntity("Entity");
			SetLocal(entity, glm::vec3(1.0f, 2.0f, 3.0f), RotationY90(), glm::vec3(1.0f));
			CHECK(Test::ApproxEqual(TransformSystem::GetRenderPosition(entity), glm::vec3(1.0f, 2.0f, 3.0f), 1e-6f));
			CHECK(Test::ApproxEqual(TransformSystem::GetRenderRotation(entity), RotationY90(), 1e-6f));
		}
	}

}
