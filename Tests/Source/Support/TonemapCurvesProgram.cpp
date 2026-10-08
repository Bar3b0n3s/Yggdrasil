#include "TestsPCH.h"
#include "Support/TonemapCurvesProgram.h"

#include "Engine/Core/Error.h"
#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	namespace Test {

		PipelineLayoutDescription MakeTonemapCurvesLayoutDescription()
		{
			ENGINE_CONTRACT_STUB();
			// The stub program has no bindings (Docs/Decisions/0013-m8-decisions.md decision 3).
			return { .Name = "TonemapCurves", .Program = "TonemapCurves", .Entries = { "CSMain" } };
		}

		Result<std::vector<glm::vec3>> RunTonemapCurves(HeadlessGpuFixture& /*gpu*/, RenderTonemapper /*tonemapper*/, std::span<const glm::dvec3> /*points*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "the TonemapCurves program is not implemented yet (M8 stream C)");
		}

	}

}
