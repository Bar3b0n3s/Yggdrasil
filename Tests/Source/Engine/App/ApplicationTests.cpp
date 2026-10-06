#include "TestsPCH.h"

#include "Engine/App/Application.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"
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

		// Reports one GPU error through its device's diagnostics during its first update, as a validation message would.
		class GpuErrorApplication final : public Application
		{
		public:
			explicit GpuErrorApplication(ApplicationSpecification specification)
				: Application(std::move(specification))
			{
			}
		protected:
			void OnUpdate(const FrameTime& frame) override
			{
				GraphicsDevice* device = GetContext().GetGraphicsDevice();
				if (frame.FrameIndex == 0 && device != nullptr)
					device->GetDiagnostics().ReportMessage(GpuMessageSeverity::Error, "ApplicationTests", "a provoked GPU error");
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
		specification.ThrottleHeadless = false;      // test runs are unthrottled (§4.2)
		specification.Renderer = RendererMode::None; // in place, without the GPU (the GPU suite covers rendering)
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

		TEST_CASE("Application: --expect-no-gpu-errors fails a run whose device reported an error"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			{
				Test::HeadlessGpuFixture gpu; // probes for a device; destroyed before the applications create theirs
				ENGINE_REQUIRE_GPU(gpu);
			}
			ApplicationSpecification specification = MakeHeadlessSpecification(3);
			specification.Renderer = RendererMode::Vulkan;
			specification.Graphics.Validation = true;
			specification.Graphics.MaxApiVersion = Test::GetTestOptions().VulkanApi;

			// Without the option the error is only logged, and the run succeeds.
			{
				GpuErrorApplication application(specification);
				Test::ExpectLog reported(LogLevel::Error, "a provoked GPU error");
				CHECK(application.Run() == ExitCode::Success);
			}

			// With it the run fails, naming the count.
			specification.ExpectNoGpuErrors = true;
			GpuErrorApplication application(std::move(specification));
			Test::ExpectLog reported(LogLevel::Error, "a provoked GPU error");
			Test::ExpectLog failed(LogLevel::Error, "reported 1 error(s) during the run (--expect-no-gpu-errors)");
			CHECK(application.Run() == ExitCode::Failed);
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

		TEST_CASE("ApplyEngineCommandLine: the graphics options set the renderer and the graphics specification")
		{
			const std::vector<std::string> arguments = {
				"--renderer",
				"none",
				"--gpu-validation",
				"--vulkan-api=1.3",
				"--gpu",
				"RTX",
				"--gpu-inject-fault=hang",
				"--expect-no-gpu-errors",
			};
			const Result<CommandLine> commandLine = CommandLine::Parse(arguments, GetEngineCommandLineOptions());
			REQUIRE_MESSAGE(commandLine.has_value(), commandLine.error().ToString());
			ApplicationSpecification specification;
			REQUIRE(ApplyEngineCommandLine(*commandLine, specification).has_value());
			CHECK(specification.Renderer == RendererMode::None);
			CHECK(specification.Graphics.Validation);
			CHECK_FALSE(specification.Graphics.SynchronizationValidation);
			CHECK(specification.Graphics.MaxApiVersion == VulkanApiVersion::Vulkan13);
			CHECK(specification.Graphics.GpuOverride == "RTX");
			CHECK(specification.Graphics.InjectFault == GpuFault::Hang);
			CHECK(specification.ExpectNoGpuErrors);

			// --gpu-validation=sync adds synchronization validation (§15.3).
			const std::vector<std::string> syncArguments = { "--gpu-validation=sync" };
			const Result<CommandLine> sync = CommandLine::Parse(syncArguments, GetEngineCommandLineOptions());
			REQUIRE_MESSAGE(sync.has_value(), sync.error().ToString());
			ApplicationSpecification synchronized;
			REQUIRE(ApplyEngineCommandLine(*sync, synchronized).has_value());
			CHECK(synchronized.Graphics.Validation);
			CHECK(synchronized.Graphics.SynchronizationValidation);
			CHECK_FALSE(synchronized.ExpectNoGpuErrors);

			// Without them: the Vulkan renderer, the configuration's validation default, API 1.4, no override, no fault.
			const Result<CommandLine> none = CommandLine::Parse({}, GetEngineCommandLineOptions());
			REQUIRE(none.has_value());
			ApplicationSpecification defaults;
			REQUIRE(ApplyEngineCommandLine(*none, defaults).has_value());
			CHECK(defaults.Renderer == RendererMode::Vulkan);
			CHECK(defaults.Graphics.Validation == DefaultGpuValidation);
			CHECK_FALSE(defaults.Graphics.SynchronizationValidation);
			CHECK_FALSE(defaults.ExpectNoGpuErrors);
			CHECK(defaults.Graphics.MaxApiVersion == VulkanApiVersion::Vulkan14);
			CHECK(defaults.Graphics.GpuOverride.empty());
			CHECK(defaults.Graphics.InjectFault == GpuFault::None);
			CHECK(defaults.Graphics.FramesInFlight == 2);
		}

		TEST_CASE("ApplyEngineCommandLine: malformed graphics option values are InvalidArgument naming the option")
		{
			const std::array<std::vector<std::string>, 6> invalid = { {
				{ "--renderer", "Vulkan" },
				{ "--renderer", "opengl" },
				{ "--vulkan-api", "1.2" },
				{ "--gpu-inject-fault", "none" },
				{ "--gpu-inject-fault", "crash" },
				{ "--gpu-validation=full" },
			} };
			for (const std::vector<std::string>& arguments : invalid)
			{
				const std::string spelled = arguments.front();
				CAPTURE(spelled);
				const Result<CommandLine> commandLine = CommandLine::Parse(arguments, GetEngineCommandLineOptions());
				REQUIRE_MESSAGE(commandLine.has_value(), commandLine.error().ToString());
				ApplicationSpecification specification;
				const Status applied = ApplyEngineCommandLine(*commandLine, specification);
				REQUIRE_FALSE(applied.has_value());
				CHECK(applied.error().GetCode() == ErrorCode::InvalidArgument);
				const std::string optionName = spelled.substr(0, spelled.find('='));
				CHECK(applied.error().GetMessageText().contains(optionName));
			}
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
