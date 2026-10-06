#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Engine {

	// The smallest magnitude of each Scale component (Architecture §5.3), so a world matrix is always invertible and no
	// zero scale reaches Jolt or the renderer. Registered as FieldMeta::MinMagnitude of Scale.
	inline constexpr float MinTransformScaleMagnitude = 1e-4f;

	// Registry name "Transform" (Architecture §5.2, §5.3): the entity's local TRS relative to its parent (the scene root for
	// a root entity). Required. Right-handed, +Y up, metres; Rotation is a unit quaternion serialized as [x, y, z, w].
	// Virtual fields registered alongside (CoreRegistration.cpp, through TransformSystem): EulerAngles (local, degrees,
	// read-write), WorldPosition and WorldRotation (read-write, converted through the parent's inverse), WorldScale
	// (read-only), RenderPosition and RenderRotation (read-only, interpolated at the frame's Alpha, §5.2).
	struct TransformComponent
	{
		glm::vec3 Translation = glm::vec3(0.0f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity (glm's constructor order is w, x, y, z)
		glm::vec3 Scale = glm::vec3(1.0f);
	};

}
