#pragma once

#include "Engine/Testing/FeatureTestRunner.h"

namespace Engine {

	// Seeded defect: Session includes Testing. Layer5Edges allows Session -> Testing (Testing may include Session),
	// never the reverse, so the layer-5 graph stays acyclic.
	class PlaySession
	{
	private:
		FeatureTestRunner m_Runner;
	};

}
