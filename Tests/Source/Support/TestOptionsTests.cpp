#include "TestsPCH.h"

#include "Support/TestOptions.h"

#include "Support/ChildProcess.h"
#include "Support/Utf8Path.h"

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("TestOptions: parses the death-test and timeout options and ignores doctest's")
		{
			const std::array<const char*, 5> argv = {
				"Tests",
				"--test-case=Core*",
				"--death-test=Core/AssertFires",
				"--test-timeout=2.5",
				"-s",
			};
			const Result<Test::TestOptions> options = Test::ParseTestOptions(static_cast<int>(argv.size()), argv.data());
			REQUIRE(options.has_value());
			CHECK(options->DeathTest == "Core/AssertFires");
			CHECK(options->DefaultTimeoutSeconds == 2.5);
			// ExecutablePath comes from the OS, not from argv[0] (which is a bare name without ".exe" here).
			const Result<std::filesystem::path> running = Test::GetCurrentExecutablePath();
			REQUIRE(running.has_value());
			CHECK(options->ExecutablePath == *running);
			CHECK(options->ExecutablePath.is_absolute());
			CHECK(options->ExecutablePath.stem() == "Tests");
		}

		TEST_CASE("TestOptions: defaults apply when no engine option is given")
		{
			const std::array<const char*, 1> argv = { "Tests" };
			const Result<Test::TestOptions> options = Test::ParseTestOptions(static_cast<int>(argv.size()), argv.data());
			REQUIRE(options.has_value());
			CHECK(options->DeathTest.empty());
			CHECK(options->WindowedChild.empty());
			CHECK_FALSE(options->CrashChild);
			CHECK(options->UserDataDirectory.empty());
			CHECK(options->ChildArgument.empty());
			CHECK_FALSE(options->ChildProcess);
			CHECK_FALSE(options->IsChildMode());
			CHECK_FALSE(options->IsChildProcess());
			CHECK(options->DefaultTimeoutSeconds == 120.0);
		}

		TEST_CASE("TestOptions: parses the windowed and crash child modes, the user-data root and the child argument")
		{
			std::error_code error;
			const std::filesystem::path root = std::filesystem::temp_directory_path(error) / "EngineTests" / "UserData";
			REQUIRE_FALSE(error);
			const std::string userDataOption = "--user-data-dir=" + Test::PathToUtf8(root);

			const std::array<const char*, 4> windowed = {
				"Tests",
				"--windowed-child=Window: a native window opens",
				userDataOption.c_str(),
				"--child-argument=some text=with an equals sign",
			};
			const Result<Test::TestOptions> windowedOptions = Test::ParseTestOptions(static_cast<int>(windowed.size()), windowed.data());
			REQUIRE(windowedOptions.has_value());
			CHECK(windowedOptions->WindowedChild == "Window: a native window opens");
			CHECK(windowedOptions->UserDataDirectory == root);
			CHECK(windowedOptions->ChildArgument == "some text=with an equals sign");
			CHECK_FALSE(windowedOptions->CrashChild);
			CHECK(windowedOptions->IsChildMode());
			CHECK(windowedOptions->IsChildProcess()); // a child mode is a child process even without --child-process

			const std::array<const char*, 2> crash = { "Tests", "--crash-child" };
			const Result<Test::TestOptions> crashOptions = Test::ParseTestOptions(static_cast<int>(crash.size()), crash.data());
			REQUIRE(crashOptions.has_value());
			CHECK(crashOptions->CrashChild);
			CHECK(crashOptions->WindowedChild.empty());
			CHECK(crashOptions->IsChildMode());
		}

		TEST_CASE("TestOptions: --child-process marks a child process that runs no child mode")
		{
			const std::array<const char*, 4> argv = { "Tests", "--no-skip", "--test-case=Some case", "--child-process" };
			const Result<Test::TestOptions> options = Test::ParseTestOptions(static_cast<int>(argv.size()), argv.data());
			REQUIRE(options.has_value());
			CHECK(options->ChildProcess);
			CHECK_FALSE(options->IsChildMode());
			CHECK(options->IsChildProcess());
			CHECK(options->UserDataDirectory.empty());
		}

		TEST_CASE("TestOptions: MakeTestsChildSpecification starts this executable and appends --child-process")
		{
			const ProcessSpecification specification = Test::MakeTestsChildSpecification({ "--death-test=Core/AssertFires", "-s" });
			CHECK(specification.Executable == Test::GetTestOptions().ExecutablePath);
			const std::vector<std::string> expected = { "--death-test=Core/AssertFires", "-s", Test::ChildProcessOption };
			CHECK(specification.Arguments == expected);
			CHECK(specification.WorkingDirectory.empty());
			CHECK(specification.Environment.empty());
		}

		TEST_CASE("TestOptions: more than one child mode is InvalidArgument")
		{
			const std::array<std::array<const char*, 3>, 3> combinations = { {
				{ "Tests", "--death-test=Core/AssertFires", "--crash-child" },
				{ "Tests", "--windowed-child=Window: a native window opens", "--crash-child" },
				{ "Tests", "--death-test=Core/AssertFires", "--windowed-child=Window: a native window opens" },
			} };
			for (const std::array<const char*, 3>& argv : combinations)
			{
				const Result<Test::TestOptions> options = Test::ParseTestOptions(static_cast<int>(argv.size()), argv.data());
				INFO("arguments: ", argv[1], " ", argv[2]);
				REQUIRE_FALSE(options.has_value());
				CHECK(options.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(options.error().GetMessageText().contains("child modes"));
			}
		}

		TEST_CASE("TestOptions: GetBuiltExecutablePath finds the executables built next to Tests")
		{
			const std::filesystem::path& tests = Test::GetTestOptions().ExecutablePath;

			const Result<std::filesystem::path> runtime = Test::GetBuiltExecutablePath("Runtime");
			REQUIRE_MESSAGE(runtime.has_value(), runtime.error().ToString());
			CHECK(runtime->parent_path().parent_path() == tests.parent_path().parent_path());
			CHECK(runtime->stem() == "Runtime");
			CHECK(runtime->extension() == tests.extension());

			const Result<std::filesystem::path> missing = Test::GetBuiltExecutablePath("NoSuchProject");
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			CHECK(missing.error().GetHint().contains("Scripts/Build.py"));
			CHECK(missing.error().GetHint().contains("--project NoSuchProject"));
		}

		TEST_CASE("TestOptions: the last occurrence wins and longer option names are not engine options")
		{
			const std::array<const char*, 6> argv = {
				"Tests",
				"--test-timeout=3",
				"--test-timeout=1e1",
				"--death-test=Core/First",
				"--death-test=Core/Second",
				"--death-testing=Core/NotAnOption",
			};
			const Result<Test::TestOptions> options = Test::ParseTestOptions(static_cast<int>(argv.size()), argv.data());
			REQUIRE(options.has_value());
			CHECK(options->DefaultTimeoutSeconds == 10.0);
			CHECK(options->DeathTest == "Core/Second");
		}

		TEST_CASE("TestOptions: malformed values are InvalidArgument")
		{
			const std::array<const char*, 17> malformed = {
				"--death-test=",
				"--death-test",
				"--windowed-child=",
				"--windowed-child",
				"--crash-child=1",
				"--child-process=yes",
				"--user-data-dir=",
				"--user-data-dir=relative/folder",
				"--test-timeout=0",
				"--test-timeout=-1",
				"--test-timeout=soon",
				"--test-timeout",
				"--test-timeout=inf",
				"--test-timeout=nan",
				"--test-timeout= 1",
				"--test-timeout=1s",
				"--test-timeout=0x10",
			};
			for (const char* bad : malformed)
			{
				const std::array<const char*, 2> argv = { "Tests", bad };
				const Result<Test::TestOptions> options = Test::ParseTestOptions(static_cast<int>(argv.size()), argv.data());
				INFO("argument: ", std::string(bad));
				REQUIRE_FALSE(options.has_value());
				CHECK(options.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(options.error().GetMessageText().contains(std::string_view(bad).substr(0, std::string_view(bad).find('='))));
			}
		}

		TEST_CASE("TestOptions: the Tests main stored the options of this run")
		{
			const Result<std::filesystem::path> running = Test::GetCurrentExecutablePath();
			REQUIRE(running.has_value());
			CHECK(Test::GetTestOptions().ExecutablePath == *running);
			CHECK(Test::GetTestOptions().DeathTest.empty());
			CHECK(Test::GetTestOptions().DefaultTimeoutSeconds > 0.0);
		}

		TEST_CASE("TestOptions: a --user-data-dir that is not valid UTF-8 is InvalidArgument")
		{
			// An absolute path on every host, except for the ill-formed byte 0xFF.
			std::error_code error;
			const std::filesystem::path temporary = std::filesystem::temp_directory_path(error);
			REQUIRE_FALSE(error);
			const std::string option = "--user-data-dir=" + Test::PathToUtf8(temporary.root_path()) + "Data\xff";
			const std::array<const char*, 2> argv = { "Tests", option.c_str() };
			const Result<Test::TestOptions> options = Test::ParseTestOptions(static_cast<int>(argv.size()), argv.data());
			REQUIRE_FALSE(options.has_value());
			CHECK(options.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(options.error().GetMessageText().contains("--user-data-dir"));
		}
	}

}
