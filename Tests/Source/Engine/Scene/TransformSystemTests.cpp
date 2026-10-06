#include "TestsPCH.h"

#include "Engine/Scene/TransformSystem.h"

#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/GlmApprox.h"
#include "Support/SceneTestFixture.h"

#include <limits>

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

	// 90 degrees about +X maps +Y to +Z.
	static glm::mat4 ReferenceRotationX90()
	{
		return glm::mat4(
			glm::vec4(1.0f, 0.0f, 0.0f, 0.0f),
			glm::vec4(0.0f, 0.0f, 1.0f, 0.0f),
			glm::vec4(0.0f, -1.0f, 0.0f, 0.0f),
			glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
	}

	// The unit quaternion of 90 degrees about +Y: (x, y, z, w) = (0, sqrt(1/2), 0, sqrt(1/2)).
	static glm::quat RotationY90()
	{
		const float half = 0.70710678118654752f;
		return glm::quat(half, 0.0f, half, 0.0f);
	}

	// 90 degrees about +X.
	static glm::quat RotationX90()
	{
		const float half = 0.70710678118654752f;
		return glm::quat(half, half, 0.0f, 0.0f);
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

	static bool SameRotation(const glm::quat& a, const glm::quat& b, float epsilon)
	{
		// q and -q are the same rotation.
		return Test::ApproxEqual(a, b, epsilon) || Test::ApproxEqual(a, -b, epsilon);
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("TransformSystem: matches reference matrices")
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
			CHECK(TransformSystem::ComputeLocalMatrix(TransformComponent{}) == glm::mat4(1.0f));

			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity parent = scene.CreateEntity("Parent");
			const Entity child = scene.CreateEntity("Child", parent);
			const Entity grandchild = scene.CreateEntity("Grandchild", child);
			SetLocal(parent, glm::vec3(10.0f, 0.0f, 0.0f), RotationY90(), glm::vec3(2.0f));
			SetLocal(child, glm::vec3(1.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));
			SetLocal(grandchild, glm::vec3(0.0f, 1.0f, 0.0f), RotationX90(), glm::vec3(1.0f, 1.0f, 3.0f));

			TransformSystem::Update(scene);
			// The child's local +X offset of 1 is rotated to -Z and scaled by 2: world (10, 0, -2).
			const glm::mat4& world = child.GetComponent<WorldTransformComponent>().Matrix;
			CHECK(Test::ApproxEqual(glm::vec3(world[3]), glm::vec3(10.0f, 0.0f, -2.0f), 1e-5f));
			CHECK(Test::ApproxEqual(world, TransformSystem::ComputeWorldMatrix(child), 1e-6f));
			CHECK(Test::ApproxEqual(parent.GetComponent<WorldTransformComponent>().Matrix,
				TransformSystem::ComputeLocalMatrix(parent.GetComponent<TransformComponent>()), 1e-6f));

			// The grandchild against a product of hand-written matrices: T(10, 0, 0) Ry(90) S(2) T(1, 0, 0) T(0, 1, 0) Rx(90) S(1, 1, 3).
			glm::mat4 parentReference = ReferenceRotationY90() * 2.0f;
			parentReference[3] = glm::vec4(10.0f, 0.0f, 0.0f, 1.0f);
			glm::mat4 childReference(1.0f);
			childReference[3] = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
			glm::mat4 grandchildReference = ReferenceRotationX90();
			grandchildReference[2] *= 3.0f;
			grandchildReference[3] = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
			const glm::mat4 grandchildWorld = grandchild.GetComponent<WorldTransformComponent>().Matrix;
			CHECK(Test::ApproxEqual(grandchildWorld, parentReference * childReference * grandchildReference, 1e-5f));
			// Update and the on-demand chain walk are the same computation.
			CHECK(grandchildWorld == TransformSystem::ComputeWorldMatrix(grandchild));
		}

		TEST_CASE("TransformSystem: Update keeps world matrices current without touching the revision")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity parent = scene.CreateEntity("Parent");
			const Entity child = scene.CreateEntity("Child", parent);
			SetLocal(child, glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));

			const uint64_t revision = scene.GetRevision();
			TransformSystem::Update(scene);
			CHECK(scene.GetRevision() == revision);
			REQUIRE(child.HasComponent<WorldTransformComponent>());
			CHECK(Test::ApproxEqual(glm::vec3(child.GetComponent<WorldTransformComponent>().Matrix[3]), glm::vec3(0.0f, 1.0f, 0.0f)));

			// A later change is picked up by the next Update, including a move to another parent.
			SetLocal(parent, glm::vec3(5.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));
			TransformSystem::Update(scene);
			CHECK(Test::ApproxEqual(glm::vec3(child.GetComponent<WorldTransformComponent>().Matrix[3]), glm::vec3(5.0f, 1.0f, 0.0f)));

			REQUIRE(scene.SetParent(child, Entity{}, {}, false).has_value());
			TransformSystem::Update(scene);
			CHECK(Test::ApproxEqual(glm::vec3(child.GetComponent<WorldTransformComponent>().Matrix[3]), glm::vec3(0.0f, 1.0f, 0.0f)));
		}

		TEST_CASE("Transform: WorldPosition and WorldRotation setters round-trip through the parent inverse")
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
			CHECK(SameRotation(roundTrip, rotation, 1e-5f));
			CHECK(Test::ApproxEqual(TransformSystem::GetWorldPosition(child), position, 1e-5f)); // rotation keeps position

			// The local transform is what the parent inverse gives.
			const glm::vec3 local = child.GetComponent<TransformComponent>().Translation;
			const glm::vec4 back = TransformSystem::ComputeWorldMatrix(parent) * glm::vec4(local, 1.0f);
			CHECK(Test::ApproxEqual(glm::vec3(back), position, 1e-5f));
			CHECK(SameRotation(TransformSystem::GetWorldRotation(parent) * child.GetComponent<TransformComponent>().Rotation, rotation, 1e-5f));

			// The world position keeps the local scale; the world rotation is that of the world matrix.
			CHECK(child.GetComponent<TransformComponent>().Scale == glm::vec3(1.0f));
			glm::mat3 worldAxes(TransformSystem::ComputeWorldMatrix(child));
			worldAxes[0] = glm::normalize(worldAxes[0]);
			worldAxes[1] = glm::normalize(worldAxes[1]);
			worldAxes[2] = glm::normalize(worldAxes[2]);
			CHECK(Test::ApproxEqual(worldAxes, glm::mat3_cast(rotation), 1e-5f));

			// A root's local transform is its world transform.
			const Entity root = scene.CreateEntity("Root");
			TransformSystem::SetWorldPosition(root, position);
			TransformSystem::SetWorldRotation(root, rotation);
			CHECK(root.GetComponent<TransformComponent>().Translation == position);
			CHECK(root.GetComponent<TransformComponent>().Rotation == rotation);
		}

		TEST_CASE("TransformSystem: SetWorldRotation accepts every rotation a Quat field accepts")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity parent = scene.CreateEntity("Parent");
			const Entity child = scene.CreateEntity("Child", parent);
			SetLocal(parent, glm::vec3(0.0f), RotationY90(), glm::vec3(1.0f));

			// Length 1.0008: within the 1e-3 a Quat field allows, though its squared length is 1.0016.
			const glm::quat slightlyLong(0.0f, 0.0f, 1.0008f, 0.0f);
			TransformSystem::SetWorldRotation(child, slightlyLong);
			CHECK(SameRotation(TransformSystem::GetWorldRotation(child), glm::normalize(slightlyLong), 1e-5f));
		}

		TEST_CASE("TransformSystem: the world scale is the lossy column scale with the local signs")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity parent = scene.CreateEntity("Parent");
			const Entity child = scene.CreateEntity("Child", parent);
			SetLocal(parent, glm::vec3(0.0f), RotationY90(), glm::vec3(2.0f, 2.0f, -2.0f));
			SetLocal(child, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(-0.5f, 3.0f, 1.0f));
			CHECK(Test::ApproxEqual(TransformSystem::GetWorldScale(parent), glm::vec3(2.0f, 2.0f, -2.0f)));
			CHECK(Test::ApproxEqual(TransformSystem::GetWorldScale(child), glm::vec3(-1.0f, 6.0f, -2.0f)));
		}

		TEST_CASE("TransformSystem: Euler angles in degrees convert to quaternions and back")
		{
			const glm::quat yaw = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(0.0f, 90.0f, 0.0f));
			CHECK(Test::ApproxEqual(yaw, RotationY90(), 1e-6f));
			CHECK(Test::ApproxEqual(TransformSystem::QuaternionFromEulerDegrees(glm::vec3(90.0f, 0.0f, 0.0f)), RotationX90(), 1e-6f));
			CHECK(Test::ApproxEqual(TransformSystem::QuaternionFromEulerDegrees(glm::vec3(0.0f)), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));

			const glm::vec3 angles(30.0f, -45.0f, 60.0f);
			const glm::vec3 back = TransformSystem::EulerDegreesFromQuaternion(TransformSystem::QuaternionFromEulerDegrees(angles));
			CHECK(Test::ApproxEqual(back, angles, 1e-4f));

			// The order is Z, then X, then Y applied to a vector: q = qY * qX * qZ.
			const glm::quat qx = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(angles.x, 0.0f, 0.0f));
			const glm::quat qy = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(0.0f, angles.y, 0.0f));
			const glm::quat qz = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(0.0f, 0.0f, angles.z));
			CHECK(SameRotation(TransformSystem::QuaternionFromEulerDegrees(angles), qy * qx * qz, 1e-6f));

			// Ranges: X in [-90, 90], Y and Z in (-180, 180].
			const auto roundTrip = [](const glm::vec3& degrees)
			{
				return TransformSystem::EulerDegreesFromQuaternion(TransformSystem::QuaternionFromEulerDegrees(degrees));
			};
			CHECK(Test::ApproxEqual(roundTrip(glm::vec3(0.0f, 180.0f, 0.0f)), glm::vec3(0.0f, 180.0f, 0.0f), 1e-4f));
			CHECK(Test::ApproxEqual(roundTrip(glm::vec3(0.0f, 0.0f, -170.0f)), glm::vec3(0.0f, 0.0f, -170.0f), 1e-4f));
			CHECK(Test::ApproxEqual(roundTrip(glm::vec3(0.0f, 270.0f, 0.0f)), glm::vec3(0.0f, -90.0f, 0.0f), 1e-4f));
			CHECK(Test::ApproxEqual(roundTrip(glm::vec3(-60.0f, 135.0f, -150.0f)), glm::vec3(-60.0f, 135.0f, -150.0f), 1e-4f));
		}

		TEST_CASE("TransformSystem: Euler extraction in gimbal lock keeps the rotation")
		{
			for (const glm::vec3& angles : { glm::vec3(90.0f, 30.0f, 0.0f), glm::vec3(-90.0f, -20.0f, 45.0f), glm::vec3(90.0f, 0.0f, 120.0f) })
			{
				INFO(std::format("{}, {}, {}", angles.x, angles.y, angles.z));
				const glm::quat rotation = TransformSystem::QuaternionFromEulerDegrees(angles);
				const glm::vec3 extracted = TransformSystem::EulerDegreesFromQuaternion(rotation);
				CHECK(std::abs(std::abs(extracted.x) - 90.0f) < 1e-3f);
				CHECK(extracted.z == 0.0f);
				CHECK(SameRotation(TransformSystem::QuaternionFromEulerDegrees(extracted), rotation, 1e-5f));
			}
		}

		TEST_CASE("TransformSystem: DecomposeMatrix recovers translation, rotation and scale")
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

			// The rotation is returned with w >= 0.
			local.Rotation = -RotationY90();
			const Result<TransformDecomposition> negated = TransformSystem::DecomposeMatrix(TransformSystem::ComputeLocalMatrix(local));
			REQUIRE(negated.has_value());
			CHECK(negated->Rotation.w >= 0.0f);
			CHECK(Test::ApproxEqual(negated->Rotation, RotationY90(), 1e-5f));

			glm::mat4 degenerate(1.0f);
			degenerate[1] = glm::vec4(0.0f);
			CHECK_FALSE(TransformSystem::DecomposeMatrix(degenerate).has_value());

			glm::mat4 tiny(1.0f);
			tiny[2] *= 5e-5f; // below MinTransformScaleMagnitude
			const Result<TransformDecomposition> tinyResult = TransformSystem::DecomposeMatrix(tiny);
			REQUIRE_FALSE(tinyResult.has_value());
			CHECK(tinyResult.error().GetCode() == ErrorCode::InvalidArgument);

			glm::mat4 parallel(1.0f);
			parallel[1] = parallel[0]; // two equal axes: rank-deficient
			CHECK_FALSE(TransformSystem::DecomposeMatrix(parallel).has_value());

			glm::mat4 notFinite(1.0f);
			notFinite[3].y = std::numeric_limits<float>::infinity();
			CHECK_FALSE(TransformSystem::DecomposeMatrix(notFinite).has_value());
		}

		TEST_CASE("TransformSystem: DecomposeMatrix expresses a mirror as a negative X scale")
		{
			TransformComponent mirrored;
			mirrored.Translation = glm::vec3(1.0f, 2.0f, 3.0f);
			mirrored.Rotation = RotationX90();
			mirrored.Scale = glm::vec3(2.0f, -1.0f, 4.0f);
			const glm::mat4 matrix = TransformSystem::ComputeLocalMatrix(mirrored);

			const Result<TransformDecomposition> decomposed = TransformSystem::DecomposeMatrix(matrix);
			REQUIRE(decomposed.has_value());
			CHECK(decomposed->Scale.x < 0.0f);
			CHECK(decomposed->Scale.y > 0.0f);
			CHECK(decomposed->Scale.z > 0.0f);

			TransformComponent recomposed;
			recomposed.Translation = decomposed->Translation;
			recomposed.Rotation = decomposed->Rotation;
			recomposed.Scale = decomposed->Scale;
			CHECK(Test::ApproxEqual(TransformSystem::ComputeLocalMatrix(recomposed), matrix, 1e-5f));
		}

		TEST_CASE("TransformSystem: DecomposeMatrix takes the closest rotation of a sheared matrix")
		{
			// The Y axis leans towards +X while X stays put: the closest rotation splits the difference, turning each axis by the
			// same angle (about Z, X towards -Y and Y towards +X).
			glm::mat4 sheared(1.0f);
			sheared[1] = glm::vec4(0.1f, 1.0f, 0.0f, 0.0f);
			const Result<TransformDecomposition> decomposed = TransformSystem::DecomposeMatrix(sheared);
			REQUIRE(decomposed.has_value());
			const glm::mat3 rotation = glm::mat3_cast(decomposed->Rotation);
			CHECK(Test::ApproxEqual(rotation * glm::transpose(rotation), glm::mat3(1.0f), 1e-5f));
			CHECK(Test::ApproxEqual(rotation[2], glm::vec3(0.0f, 0.0f, 1.0f), 1e-5f));
			CHECK(rotation[0].y < 0.0f);
			const glm::vec3 shearedY = glm::normalize(glm::vec3(0.1f, 1.0f, 0.0f));
			CHECK(glm::dot(rotation[0], glm::vec3(1.0f, 0.0f, 0.0f)) == doctest::Approx(glm::dot(rotation[1], shearedY)).epsilon(1e-5));
			CHECK(Test::ApproxEqual(decomposed->Scale, glm::vec3(1.0f, glm::length(glm::vec3(0.1f, 1.0f, 0.0f)), 1.0f), 1e-6f));
		}

		TEST_CASE("TransformSystem: render pose equals the world pose outside play")
		{
			Test::SceneTestFixture fixture;
			const Entity entity = fixture.GetScene().CreateEntity("Entity");
			SetLocal(entity, glm::vec3(1.0f, 2.0f, 3.0f), RotationY90(), glm::vec3(1.0f));
			entity.AddComponent<PreviousWorldTransformComponent>(); // ignored outside play
			fixture.GetScene().SetInterpolationAlpha(0.5f);
			CHECK(Test::ApproxEqual(TransformSystem::GetRenderPosition(entity), glm::vec3(1.0f, 2.0f, 3.0f), 1e-6f));
			CHECK(Test::ApproxEqual(TransformSystem::GetRenderRotation(entity), RotationY90(), 1e-6f));
		}

		TEST_CASE("TransformSystem: render pose interpolates from the previous step in play")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			const Entity parent = scene.CreateEntity("Parent");
			const Entity entity = scene.CreateEntity("Entity", parent);
			SetLocal(entity, glm::vec3(4.0f, 0.0f, 0.0f), RotationY90(), glm::vec3(1.0f));

			// No previous pose yet: the current one.
			scene.SetInterpolationAlpha(0.5f);
			CHECK(Test::ApproxEqual(TransformSystem::GetRenderPosition(entity), glm::vec3(4.0f, 0.0f, 0.0f)));

			entity.AddComponent<PreviousWorldTransformComponent>(); // identity: at the origin, unrotated
			CHECK(Test::ApproxEqual(TransformSystem::GetRenderPosition(entity), glm::vec3(2.0f, 0.0f, 0.0f)));
			const glm::quat halfway = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(0.0f, 45.0f, 0.0f));
			CHECK(SameRotation(TransformSystem::GetRenderRotation(entity), halfway, 1e-5f));

			scene.SetInterpolationAlpha(0.0f);
			CHECK(Test::ApproxEqual(TransformSystem::GetRenderPosition(entity), glm::vec3(0.0f)));
			CHECK(SameRotation(TransformSystem::GetRenderRotation(entity), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 1e-6f));
			scene.SetInterpolationAlpha(1.0f);
			CHECK(TransformSystem::GetRenderPosition(entity) == TransformSystem::GetWorldPosition(entity));
			CHECK(TransformSystem::GetRenderRotation(entity) == TransformSystem::GetWorldRotation(entity));

			// A reset tag on the entity or an ancestor renders the current pose.
			scene.SetInterpolationAlpha(0.5f);
			parent.AddComponent<InterpolationResetTag>();
			CHECK(Test::ApproxEqual(TransformSystem::GetRenderPosition(entity), glm::vec3(4.0f, 0.0f, 0.0f)));
			CHECK(Test::ApproxEqual(TransformSystem::GetRenderRotation(entity), RotationY90(), 1e-6f));
		}
	}

}
