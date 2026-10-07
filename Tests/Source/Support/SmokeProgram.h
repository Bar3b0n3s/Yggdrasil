#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Graphics/PipelineFactory.h"

#include <string_view>

// The Smoke compute program of Resources/Shaders (Passes/Smoke.slang, Roadmap M0): no engine pass uses it, so its pipeline
// layout lives here, shared by the reflection check ("Shaders: LayoutsMatchReflection") and the GPU arithmetic test
// ("Compute: Smoke scales a buffer exactly").

namespace Engine {

	namespace Test {

		// Set 0: b0 SmokeConstants and u0 the RWStructuredBuffer<float> Values, for the permutation SMOKE_SATURATE=`saturate`
		// ("0" or "1").
		[[nodiscard]] PipelineLayoutDescription MakeSmokeLayoutDescription(std::string_view saturate);

	}

}
