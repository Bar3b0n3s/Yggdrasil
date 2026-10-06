#include "TestsPCH.h"
#include "Support/DeathTest.h"

// M1 contract stub (Roadmap rule 3): stream E implements the registry, the child runner and the parent check. The stub
// never lets a death test pass: CheckDeath fails the calling test case.

namespace Engine {

	namespace Test {

		bool RegisterDeathTest(std::string_view /*name*/, DeathTestBody /*body*/, const char* /*file*/, int /*line*/)
		{
			return true;
		}

		DeathTestBody FindDeathTest(std::string_view /*name*/)
		{
			return nullptr;
		}

		std::vector<std::string> GetDeathTestNames()
		{
			return {};
		}

		int RunDeathTestBody(std::string_view /*name*/)
		{
			return 2;
		}

		Result<DeathTestResult> RunDeathTest(std::string_view /*name*/, std::chrono::milliseconds /*timeout*/)
		{
			return MakeError(ErrorCode::Unsupported, "Test::RunDeathTest is an M1 contract stub");
		}

		void CheckDeath(std::string_view /*name*/, std::string_view /*expectedSubstring*/, const std::source_location& location)
		{
			ADD_FAIL_CHECK_AT(location.file_name(), static_cast<int>(location.line()), "Test::CheckDeath is an M1 contract stub");
		}

	}

}
