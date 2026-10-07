#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Graphics/PipelineFactory.h"

// The MatrixConvention compute program of Resources/Shaders (Passes/MatrixConvention.slang): no engine pass uses it, so its
// pipeline layout lives here, shared by the reflection check ("Shaders: LayoutsMatchReflection") and the GPU test of the
// matrix convention of Architecture §8.4 ("Shaders: matrix convention transforms known vectors").

namespace Engine {

	namespace Test {

		// Set 0: b0 ViewConstants (its ViewProjection is the matrix under test) and u0 the RWStructuredBuffer<float4>
		// Vectors, transformed in place.
		[[nodiscard]] PipelineLayoutDescription MakeMatrixConventionLayoutDescription();

	}

}
