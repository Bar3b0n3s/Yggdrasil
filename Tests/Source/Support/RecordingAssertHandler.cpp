#include "TestsPCH.h"
#include "Support/RecordingAssertHandler.h"

#include "Engine/Core/FatalError.h"
#include "Engine/Core/Log.h"
#include "Support/ExpectLog.h"
#include "Support/TestCaseTracker.h"

namespace Engine {

	namespace Test {

		void RecordingAssertHandler(const AssertInfo& info)
		{
			const std::string description = FormatAssertInfo(info);
			const std::optional<RunningTestCase> testCase = GetRunningTestCase();
			const std::string text = testCase.has_value() ? std::format("{} [test case: {}]", description, testCase->Name) : description;

			// The Critical lines of this handler and of FatalError are the report of this assertion, and the doctest failure
			// below records it once: they are not also undeclared errors of the test case. Never destroyed (FatalError exits).
			const ExpectLog assertionReport(LogLevel::Critical, description);

			if (info.IsClient)
				ENGINE_CRITICAL("{}", text);
			else
				ENGINE_CORE_CRITICAL("{}", text);

			if (testCase.has_value())
				ReportFailureToRunningTestCase(std::string(info.File), info.Line, text);

			// Flushes the log sinks and C stdio (doctest's report on a redirected stdout) and exits with code 4.
			FatalError(FatalErrorKind::Assert, description);
		}

		void InstallRecordingAssertHandler()
		{
			SetAssertHandler(&RecordingAssertHandler);
		}

	}

}
