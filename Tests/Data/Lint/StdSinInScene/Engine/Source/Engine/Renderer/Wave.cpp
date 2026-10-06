#include "Engine/Renderer/Wave.h"

#include <cmath>

namespace Engine {

	// Control: rendering may use <cmath> (Architecture section 4.12), so this is not a finding.
	float WaveHeight(float phase)
	{
		return std::sin(phase);
	}

}
