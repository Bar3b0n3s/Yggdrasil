#include "Engine/Audio/PitchJitter.h"

#include <cstdlib>

namespace Engine {

	float RandomPitchJitter()
	{
		// Seeded defect: rand() instead of a seeded Engine::Random.
		return static_cast<float>(std::rand() % 100) / 1000.0f;
	}

}
