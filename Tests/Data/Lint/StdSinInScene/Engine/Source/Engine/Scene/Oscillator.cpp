#include "Engine/Scene/Oscillator.h"

#include <cmath>

namespace Engine {

	float OscillatorOffset(float time)
	{
		// Seeded defect: a C runtime transcendental function on the simulation path.
		return std::sin(time);
	}

	namespace Math = std;

	float AliasedOffset(float time)
	{
		// Seeded defect: the same kind of call, qualified by a namespace alias of std.
		return Math::cos(time);
	}

	glm::quat SpinTowards(const glm::vec3& eulerAngles, const glm::quat& target, float weight)
	{
		// Seeded defects: an Euler-angle quaternion (cos and sin) and quaternion interpolation (acos and sin).
		const glm::quat start = glm::quat(eulerAngles);
		return glm::mix(start, target, weight);
	}

}
