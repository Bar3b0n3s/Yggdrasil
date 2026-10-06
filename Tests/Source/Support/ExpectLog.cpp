#include "TestsPCH.h"
#include "Support/ExpectLog.h"

// M1 contract stub (Roadmap rule 3): stream E implements the listener and the expectation registry. The stub never
// lets an expectation pass: an ExpectLog fails the test case that declares it.

namespace Engine {

	namespace Test {

		void InstallExpectLogListener()
		{
		}

		void UninstallExpectLogListener()
		{
		}

		ExpectLog::ExpectLog(LogLevel /*level*/, std::string_view /*substring*/, const std::source_location& location)
		{
			ADD_FAIL_CHECK_AT(location.file_name(), static_cast<int>(location.line()), "Test::ExpectLog is an M1 contract stub");
		}

		ExpectLog::~ExpectLog() = default;

		uint32_t ExpectLog::GetMatchCount() const
		{
			return 0;
		}

	}

}
