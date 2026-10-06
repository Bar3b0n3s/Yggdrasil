#include "TestsPCH.h"

#include "Engine/App/EntryPoint.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Platform/Process.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

// RunApplication owns a whole process (it creates the ProcessContext), so it is tested through the Runtime executable,
// whose main is RunApplication with the runtime's factory. Every run that gets as far as the ProcessContext is given
// --user-data-dir, so nothing is written into the real user-data folder.

namespace Engine {

	static Result<ProcessResult> RunRuntime(std::vector<std::string> arguments)
	{
		ENGINE_TRY_ASSIGN(std::filesystem::path runtime, Test::GetBuiltExecutablePath("Runtime"));
		return Process::Run({ .Executable = std::move(runtime), .Arguments = std::move(arguments) }, std::chrono::seconds(60));
	}

	static std::string UserDataOption(const Test::TempDirectory& directory)
	{
		return "--user-data-dir=" + Test::PathToUtf8(directory.GetPath());
	}

	TEST_SUITE("App")
	{
		TEST_CASE("RunApplication: an unknown option exits with UsageError and names it")
		{
			Test::TempDirectory userData("UnknownOption");
			const Result<ProcessResult> result = RunRuntime({ "--headless", "--no-such-option", UserDataOption(userData) });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK(result->ExitCode == ExitCode::UsageError);
			CHECK(result->StandardError.contains("--no-such-option"));

			// A single-dash spelling and a stray positional argument are usage errors too, never silently ignored.
			const std::array<std::string, 2> mistyped = { "-headless", "extra" };
			for (const std::string& argument : mistyped)
			{
				CAPTURE(argument);
				const Result<ProcessResult> rejected = RunRuntime({ argument, "--frames", "1", UserDataOption(userData) });
				REQUIRE_MESSAGE(rejected.has_value(), rejected.error().ToString());
				CHECK(rejected->ExitCode == ExitCode::UsageError);
				CHECK(rejected->StandardError.contains(argument));
			}
		}

		TEST_CASE("RunApplication: a malformed --frames value exits with UsageError")
		{
			const std::array<std::vector<std::string>, 3> invalid = { {
				{ "--headless", "--frames" },
				{ "--headless", "--frames", "0" },
				{ "--headless", "--frames", "ten" },
			} };
			Test::TempDirectory userData("MalformedFrames");
			for (const std::vector<std::string>& arguments : invalid)
			{
				CAPTURE(arguments.back());
				std::vector<std::string> withUserData = { UserDataOption(userData) };
				withUserData.insert(withUserData.end(), arguments.begin(), arguments.end());
				const Result<ProcessResult> result = RunRuntime(std::move(withUserData));
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				CHECK(result->ExitCode == ExitCode::UsageError);
				CHECK(result->StandardError.contains("--frames"));
			}
		}

		TEST_CASE("RunApplication: a user-data root that cannot be created exits with InitFailed")
		{
			// The root is a regular file, so ProcessContext::Create cannot create <root>/<AppName>/Logs.
			Test::TempDirectory directory("BlockedUserData");
			const std::filesystem::path blocked = directory / "NotADirectory";
			const std::string_view content = "a file where the user-data root should be";
			REQUIRE(FileSystem::WriteFileAtomic(blocked, std::as_bytes(std::span(content.data(), content.size()))).has_value());
			const std::string blockedOption = "--user-data-dir=" + Test::PathToUtf8(blocked);
			const Result<ProcessResult> result = RunRuntime({ "--headless", "--frames", "1", blockedOption });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK(result->ExitCode == ExitCode::InitFailed);
			CHECK(result->StandardError.contains("NotADirectory"));
		}
	}

}
