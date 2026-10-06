#pragma once

#include <glm/gtc/quaternion.hpp>

namespace Engine {

	float OscillatorOffset(float time);

	float AliasedOffset(float time);

	glm::quat SpinTowards(const glm::vec3& eulerAngles, const glm::quat& target, float weight);

}
