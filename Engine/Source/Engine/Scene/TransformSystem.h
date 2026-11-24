#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <span>

namespace Engine {

	class ConstEntity;
	class Entity;
	class Scene;
	struct TransformComponent;

	// A decomposed transform: what DecomposeMatrix returns.
	struct TransformDecomposition
	{
		glm::vec3 Translation = glm::vec3(0.0f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 Scale = glm::vec3(1.0f);
	};

	// World transforms (Architecture §5.2). TransformComponent is local TRS; world matrices are derived:
	//   - Update recomputes WorldTransformComponent for the subtrees whose transforms were written since the last update
	//     (M43's change tracking, Docs/Decisions/0061-m43-static-scenery.md decision 3: the scene marks every write, see
	//     Scene::MarkTransformWritten), parents before children, with the same products in the same order as a full pass, so
	//     every matrix is bit-identical to it. An update of a scene where nothing was written recomputes nothing. Debug
	//     builds (ENGINE_DEBUG) recompute every matrix after each update and ENGINE_CORE_VERIFY that the incremental ones
	//     equal them, naming the first stale entity, so a write that bypassed the tracking fails there instead of drawing
	//     stale in every configuration alike. Systems read WorldTransformComponent after an Update.
	//   - Between updates, gameplay reads (scripts, automation, the virtual fields) walk the parent chain on demand. The
	//     getters take a readable entity (Entity::IsReadable), so a script reads the pose of an entity pending destruction
	//     until the flush (M20, decision 4 of Docs/Decisions/0031-m20-contract.md); the setters take a valid one.
	//   - WorldPosition and WorldRotation are writable: the setters convert through the parent's world inverse and patch
	//     the local TransformComponent (recorded by the change tracker). WorldScale is read-only (a non-uniformly scaled
	//     parent can make a world scale unrepresentable as local TRS).
	// Simulation path (§4.12): only IEEE-exact operations and Core/DetMath; no CRT transcendental function, so results are
	// identical in every configuration. Main thread, like the scene. Static functions only; the canonical order cache lives
	// in the Scene.
	class TransformSystem
	{
	public:
		// Recomputes WorldTransformComponent (adding it where missing; a runtime-only component, so the scene's revision and
		// change tracker do not see it) for every entity of `scene` written since the last update and its descendants,
		// pending destruction excluded: World = ParentWorld * T(Translation) * R(Rotation) * S(Scale), with ParentWorld the
		// identity for roots. Returns the entities whose world matrix changed its bits (a new one included) and the written
		// entities whose matrix kept them, valid until the scene's next update; the scene's open world-change lists receive
		// them too (Scene::TakeWorldChanges). Callers that only need the matrices ignore the result.
		static std::span<const entt::entity> Update(Scene& scene);

		// T(Translation) * R(Rotation) * S(Scale).
		[[nodiscard]] static glm::mat4 ComputeLocalMatrix(const TransformComponent& transform);

		// The entity's world matrix computed now by walking the parent chain (independent of WorldTransformComponent).
		[[nodiscard]] static glm::mat4 ComputeWorldMatrix(ConstEntity entity);

		// Translation, rotation and scale of an affine matrix without shear, the rotation a unit quaternion with w >= 0.
		// Errors: InvalidArgument when a scale component's magnitude is below MinTransformScaleMagnitude or the matrix is not
		// finite (sheared input decomposes to the closest rotation, documented as lossy).
		[[nodiscard]] static Result<TransformDecomposition> DecomposeMatrix(const glm::mat4& matrix);

		[[nodiscard]] static glm::vec3 GetWorldPosition(ConstEntity entity);
		// Moves the entity so its world position is `position` (finite, asserted; reflected setters validate input first),
		// keeping its world rotation and local scale.
		static void SetWorldPosition(Entity entity, const glm::vec3& position);

		// The world rotation (parent world rotation * local rotation), normalized.
		[[nodiscard]] static glm::quat GetWorldRotation(ConstEntity entity);
		// Rotates the entity so its world rotation is `rotation` (unit, finite; asserted), keeping its world position.
		static void SetWorldRotation(Entity entity, const glm::quat& rotation);

		// The lossy world scale: the length of each column of the world matrix's upper 3x3, signs taken from the local
		// scale chain.
		[[nodiscard]] static glm::vec3 GetWorldScale(ConstEntity entity);

		// The rendered pose (§5.2): lerp/slerp from PreviousWorldTransformComponent to the current world pose at the scene's
		// interpolation Alpha; the current world pose when the entity or an ancestor has InterpolationResetTag, outside
		// play, or when it has no previous pose yet. M41 (Docs/Decisions/0058-m41-bone-attachments.md decision R): below Alpha
		// 1, an attached subtree's (RenderExtraction.h's "Attached subtrees") is its Animator's rendered pose times its current
		// pose relative to that Animator's entity, the position from that matrix and the rotation the Animator's render
		// rotation times the local rotations below it, so it agrees with the image.
		[[nodiscard]] static glm::vec3 GetRenderPosition(ConstEntity entity);
		[[nodiscard]] static glm::quat GetRenderRotation(ConstEntity entity);

		// Euler angles in degrees (rotation order: Z, then X, then Y applied to a vector, i.e. q = qY * qX * qZ, the
		// convention of Transform.EulerAngles, Quat.FromEuler and the inspector) <-> unit quaternion, through DetMath.
		// EulerDegreesFromQuaternion returns X in [-90, 90] and Y, Z in (-180, 180].
		[[nodiscard]] static glm::quat QuaternionFromEulerDegrees(const glm::vec3& degrees);
		[[nodiscard]] static glm::vec3 EulerDegreesFromQuaternion(const glm::quat& rotation);
	};

}
