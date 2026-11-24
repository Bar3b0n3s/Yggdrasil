#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Graphics/PipelineFactory.h"

// The test entries of the Sky program (Passes/Sky.slang; M45, Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3):
// CSTestCopyWrite and CSTestCopyRead, the GPU proof of the copy into a view's constant buffers ("SkyLighting: a buffer copied
// into a view's constant buffers after their CPU writes reaches the passes that read them"). No engine pass creates their
// pipelines, so their layouts live here, shared by that case and the reflection check ("Shaders: LayoutsMatchReflection").

namespace Engine {

	namespace Test {

		// Set 0: b1 SkyConstants (its Intensity seeds the values) and u6 the RWStructuredBuffer<float4> TestResults, which
		// CSTestCopyWrite fills as the sky's computes fill a SkyResults buffer (Shared/SkyConstants.h).
		[[nodiscard]] PipelineLayoutDescription MakeSkyCopyWriteLayoutDescription();

		// Set 0: b2 EnvironmentConstants, b4 ProbeConstants and b5 FogConstants, the forward view's registers, and u7 the
		// RWStructuredBuffer<float4> TestReadback, which CSTestCopyRead fills with what it reads of them (SkyCopyReadbackCount
		// elements).
		[[nodiscard]] PipelineLayoutDescription MakeSkyCopyReadLayoutDescription();

		// The float4 elements CSTestCopyRead writes: EnvironmentConstants' twelve, FogConstants' first two, then each reflection
		// probe's AxisScale and Lookup.
		inline constexpr uint32_t SkyCopyReadbackCount = 14 + 2 * 8;

	}

}
