#include "TestsPCH.h"
#include "Support/WindowedChild.h"

// M2 contract stub (Roadmap rule 3): stream D (application and frame loop) implements the windowed child, together with
// the --windowed-child mode of the Tests main. Until then the child mode reports a usage error and the parent fails with
// Unsupported without spawning anything.

namespace Engine {

	namespace Test {

		namespace {

			// The UsageError exit code of the §4.1 table.
			constexpr int WindowedChildUsageErrorExitCode = 2;

		}

		int RunWindowedChildTestCase(std::string_view /*testCase*/)
		{
			ENGINE_CONTRACT_STUB();
			return WindowedChildUsageErrorExitCode;
		}

		Result<WindowedChildResult> RunWindowedChild(std::string_view /*testCase*/, std::chrono::milliseconds /*timeout*/,
			const std::filesystem::path& /*userDataDirectory*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Test::RunWindowedChild is not implemented yet");
		}

		std::string DescribeWindowedChildFailure(std::string_view testCase)
		{
			ENGINE_CONTRACT_STUB();
			return std::format("the windowed child '{}' cannot run: Test::RunWindowedChild is not implemented yet", testCase);
		}

	}

}
