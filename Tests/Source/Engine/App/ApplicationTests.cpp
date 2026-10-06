#include "TestsPCH.h"

#include "Engine/App/Application.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/ProcessContext.h"
#include "Support/ExpectLog.h"
#include "Support/Utf8Path.h"

namespace Engine {

	namespace {

		// Records its hooks; OnInitialize can fail or request an exit.
		class RecordingApplication final : public Application
		{
		public:
			explicit RecordingApplication(ApplicationSpecification specification)
				: Application(std::move(specification))
			{
			}
		public:
			std::vector<std::string> Calls;
			bool FailInitialize = false;
			std::optional<int> ExitDuringInitialize;
			std::optional<uint64_t> ExitAtFrame; // RequestExit(ExitCode::Timeout) from the OnUpdate of this frame, twice
			bool ProcessContextWasCurrent = false;
		protected:
			Status OnInitialize() override
			{
				Calls.push_back("initialize");
				// The context exists during OnInitialize.
				Calls.push_back(GetContext().GetWindow() != nullptr ? "window" : "no window");
				// The process's context is reachable for process-level additions such as the fatal-error hook.
				ProcessContextWasCurrent = &GetProcessContext() == ProcessContext::GetCurrent();
				if (ExitDuringInitialize.has_value())
					RequestExit(*ExitDuringInitialize);
				if (FailInitialize)
					return MakeError(ErrorCode::NotFound, "the test application's data is missing");
				return {};
			}

			void OnShutdown() override
			{
				Calls.push_back("shutdown");
			}

			void OnFixedStep(const SimStep& step) override
			{
				Calls.push_back(std::format("step {}", step.Tick));
			}

			void OnUpdate(const FrameTime& frame) override
			{
				Calls.push_back(std::format("update {}", frame.FrameIndex));
				if (ExitAtFrame == frame.FrameIndex)
				{
					RequestExit(ExitCode::Timeout);
					RequestExit(ExitCode::Failed); // the first request wins
				}
			}
		};

	}

	// A headless application, which the headless Tests process can run in place.
	static ApplicationSpecification MakeHeadlessSpecification(std::optional<uint64_t> maxFrames)
	{
		ApplicationSpecification specification;
		specification.Name = "ApplicationTests";
		specification.Window = WindowMode::Headless;
		specification.WindowSettings = { .Title = "Application test", .Width = 64, .Height = 64 };
		specification.Clock = ClockKind::Manual;
		specification.MaxFrames = maxFrames;
		specification.WorkerCount = 0;
		specification.ThrottleHeadless = false; // test runs are unthrottled (§4.2)
		return specification;
	}

	TEST_SUITE("App")
	{
		TEST_CASE("Application: hooks run in order and --frames ends the run with Success")
		{
			RecordingApplication application(MakeHeadlessSpecification(3));
			CHECK(application.Run() == ExitCode::Success);
			const std::vector<std::string> expected = {
				"initialize",
				"window",
				"step 0",
				"update 0",
				"step 1",
				"update 1",
				"step 2",
				"update 2",
				"shutdown",
			};
			CHECK(application.Calls == expected);
			CHECK(application.ProcessContextWasCurrent);
		}

		TEST_CASE("Application: a failing OnInitialize returns InitFailed and runs no frame")
		{
			RecordingApplication application(MakeHeadlessSpecification(3));
			application.FailInitialize = true;
			Test::ExpectLog expected(LogLevel::Error, "the test application's data is missing");
			CHECK(application.Run() == ExitCode::InitFailed);
			const std::vector<std::string> calls = { "initialize", "window" };
			CHECK(application.Calls == calls);
		}

		TEST_CASE("Application: RequestExit during OnInitialize skips the loop but not OnShutdown")
		{
			RecordingApplication application(MakeHeadlessSpecification(std::nullopt));
			application.ExitDuringInitialize = ExitCode::Failed;
			CHECK(application.Run() == ExitCode::Failed);
			const std::vector<std::string> calls = { "initialize", "window", "shutdown" };
			CHECK(application.Calls == calls);
		}

		TEST_CASE("Application: RequestExit from OnUpdate ends the run after that frame with the first code")
		{
			RecordingApplication application(MakeHeadlessSpecification(std::nullopt));
			application.ExitAtFrame = 1;
			CHECK(application.Run() == ExitCode::Timeout);
			const std::vector<std::string> expected = {
				"initialize",
				"window",
				"step 0",
				"update 0",
				"step 1",
				"update 1",
				"shutdown",
			};
			CHECK(application.Calls == expected);
		}

		TEST_CASE("Application: a scripted clock cannot be chosen through the specification")
		{
			ApplicationSpecification specification = MakeHeadlessSpecification(1);
			specification.Clock = ClockKind::Scripted;
			RecordingApplication application(std::move(specification));
			Test::ExpectLog expected(LogLevel::Error, "Scripted");
			CHECK(application.Run() == ExitCode::InitFailed);
			CHECK(application.Calls.empty());
		}

		TEST_CASE("ApplyEngineCommandLine: --headless selects the null platform and ManualClock and --frames sets MaxFrames")
		{
			const std::span<const CommandLineOption> options = GetEngineCommandLineOptions();
			const auto declares = [&options](std::string_view name)
			{
				return std::ranges::any_of(options, [name](const CommandLineOption& option)
				{
					return option.Name == name;
				});
			};
			CHECK(declares("--headless"));
			CHECK(declares("--frames"));
			CHECK(declares("--user-data-dir")); // Tests never build Dist, where the option does not exist

			const std::vector<std::string> arguments = { "--headless", "--frames", "10" };
			const Result<CommandLine> commandLine = CommandLine::Parse(arguments, options);
			REQUIRE(commandLine.has_value());
			ApplicationSpecification specification;
			REQUIRE(ApplyEngineCommandLine(*commandLine, specification).has_value());
			CHECK(specification.Window == WindowMode::Headless);
			CHECK(specification.Clock == ClockKind::Manual);
			CHECK(specification.MaxFrames == std::optional<uint64_t>(10));
			CHECK(specification.Args.Has("--headless"));

			// Without options: windowed, system clock, no frame limit.
			const Result<CommandLine> none = CommandLine::Parse({}, options);
			REQUIRE(none.has_value());
			ApplicationSpecification defaults;
			REQUIRE(ApplyEngineCommandLine(*none, defaults).has_value());
			CHECK(defaults.Window == WindowMode::Windowed);
			CHECK(defaults.Clock == ClockKind::System);
			CHECK_FALSE(defaults.MaxFrames.has_value());
			CHECK(defaults.UserDataRoot.empty());
			CHECK(defaults.ThrottleHeadless);
		}

		TEST_CASE("ApplyEngineCommandLine: --user-data-dir sets an absolute user-data root")
		{
			std::error_code error;
			const std::filesystem::path root = std::filesystem::temp_directory_path(error) / "EngineTests" / "UserData";
			REQUIRE_FALSE(error);
			const std::vector<std::string> arguments = { "--user-data-dir", Test::PathToUtf8(root) };
			const Result<CommandLine> commandLine = CommandLine::Parse(arguments, GetEngineCommandLineOptions());
			REQUIRE(commandLine.has_value());
			ApplicationSpecification specification;
			REQUIRE(ApplyEngineCommandLine(*commandLine, specification).has_value());
			CHECK(specification.UserDataRoot == root);

			const std::vector<std::string> relative = { "--user-data-dir=relative/folder" };
			const Result<CommandLine> relativeLine = CommandLine::Parse(relative, GetEngineCommandLineOptions());
			REQUIRE(relativeLine.has_value());
			ApplicationSpecification rejected;
			const Status applied = ApplyEngineCommandLine(*relativeLine, rejected);
			REQUIRE_FALSE(applied.has_value());
			CHECK(applied.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(applied.error().GetMessageText().contains("--user-data-dir"));
		}

		TEST_CASE("ApplyEngineCommandLine: a --user-data-dir that is not valid UTF-8 is InvalidArgument")
		{
			// An absolute path on every host, except for the ill-formed byte 0xFF.
			std::error_code error;
			const std::filesystem::path temporary = std::filesystem::temp_directory_path(error);
			REQUIRE_FALSE(error);
			const std::string root = Test::PathToUtf8(temporary.root_path()) + "Data\xff";
			const std::vector<std::string> arguments = { "--user-data-dir", root };
			const Result<CommandLine> commandLine = CommandLine::Parse(arguments, GetEngineCommandLineOptions());
			REQUIRE(commandLine.has_value());
			ApplicationSpecification specification;
			const Status applied = ApplyEngineCommandLine(*commandLine, specification);
			REQUIRE_FALSE(applied.has_value());
			CHECK(applied.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(applied.error().GetMessageText().contains("--user-data-dir"));
			CHECK(specification.UserDataRoot.empty());
		}

		TEST_CASE("ApplyEngineCommandLine: --frames 0 is InvalidArgument")
		{
			const std::vector<std::string> arguments = { "--frames", "0" };
			const Result<CommandLine> commandLine = CommandLine::Parse(arguments, GetEngineCommandLineOptions());
			REQUIRE(commandLine.has_value());
			ApplicationSpecification specification;
			const Status applied = ApplyEngineCommandLine(*commandLine, specification);
			REQUIRE_FALSE(applied.has_value());
			CHECK(applied.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(applied.error().GetMessageText().contains("--frames"));
		}
	}

}
