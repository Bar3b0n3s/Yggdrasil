#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

// The TonemapCurves compute program of Resources/Shaders (Passes/TonemapCurves.slang): the GPU side of "TonemapPass: every
// tonemapper matches its CPU reference at 1,024 points" (Architecture §8.9, §15.3). It evaluates the tonemappers of
// Common/Tonemapping.slang, the functions TonemapPass uses, at the points of a buffer, so the curves are compared in floating
// point rather than through an 8-bit target. No engine pass uses it, so its pipeline layout lives here, shared by the
// reflection check ("Shaders: LayoutsMatchReflection") and the test. Frozen by the M8 contract
// (Docs/Decisions/0013-m8-decisions.md decision 8).

namespace Engine {

	namespace Test {

		class HeadlessGpuFixture;

		// The layout of the TonemapCurves program: the points in, the tonemapped values out, and which tonemapper.
		[[nodiscard]] PipelineLayoutDescription MakeTonemapCurvesLayoutDescription();

		// Evaluates `tonemapper` at every point on the fixture's device (one dispatch, read back): the display-linear values
		// before the OETF, in point order. Errors: those of the pipeline, buffer and readback creation.
		[[nodiscard]] Result<std::vector<glm::vec3>> RunTonemapCurves(HeadlessGpuFixture& gpu, RenderTonemapper tonemapper,
			std::span<const glm::dvec3> points);

	}

}
