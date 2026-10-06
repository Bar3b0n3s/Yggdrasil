#include "TestsPCH.h"
#include "Support/ChildProcess.h"

// M1 contract stub (Roadmap rule 3): stream E implements process spawning with captured output and a timeout.

namespace Engine {

	namespace Test {

		Result<ChildProcessResult> RunChildProcess(const std::filesystem::path& /*executable*/, std::span<const std::string> /*arguments*/,
			std::chrono::milliseconds /*timeout*/)
		{
			return MakeError(ErrorCode::Unsupported, "Test::RunChildProcess is an M1 contract stub");
		}

		Result<std::filesystem::path> GetCurrentExecutablePath()
		{
			return MakeError(ErrorCode::Unsupported, "Test::GetCurrentExecutablePath is an M1 contract stub");
		}

	}

}
