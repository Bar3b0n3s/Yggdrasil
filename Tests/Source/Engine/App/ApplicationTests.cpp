#include "TestsPCH.h"

#include "Engine/App/Application.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/Platform/Process.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"
#include "Support/WindowedChild.h"

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

		// What OnRender received, copied while the frame's pointers are valid.
		struct RenderedFrame
		{
			uint64_t FrameIndex = 0;
			uint32_t FrameSlot = 0;
			uint32_t Width = 0;
			uint32_t Height = 0;
			uint32_t TargetWidth = 0;
			uint32_t FramebufferWidth = 0;
			nvrhi::Format TargetFormat = nvrhi::Format::UNKNOWN;
			bool HasDevice = false;
			bool HasCommandList = false;
			bool HasProfiler = false;
		};

		// Records the rendering hooks with their order; in a windowed process it can minimize and restore its window from
		// the update of given frames.
		class RenderingApplication final : public Application
		{
		public:
			explicit RenderingApplication(ApplicationSpecification specification)
				: Application(std::move(specification))
			{
			}
		public:
			std::vector<std::string> Calls;
			std::vector<RenderedFrame> Frames;
			std::optional<uint64_t> MinimizeAtFrame;
			std::optional<uint64_t> RestoreAtFrame;
			bool HadImGuiLayer = false;
			bool WindowChangesArrived = true;
		protected:
			Status OnInitialize() override
			{
				HadImGuiLayer = GetImGuiLayer() != nullptr;
				return {};
			}

			void OnUpdate(const FrameTime& frame) override
			{
				Calls.push_back(std::format("update {}", frame.FrameIndex));
				Window& window = *GetContext().GetWindow();
				// Minimizing and restoring are asynchronous on X11 and macOS: wait for each, so the next frame sees it.
				if (MinimizeAtFrame == frame.FrameIndex)
				{
					window.Minimize();
					WindowChangesArrived = Test::WaitUntilMinimized(window, true) && WindowChangesArrived;
				}
				if (RestoreAtFrame == frame.FrameIndex)
				{
					window.Restore();
					WindowChangesArrived = Test::WaitUntilMinimized(window, false) && WindowChangesArrived;
				}
			}

			void OnRender(RenderContext& context) override
			{
				Calls.push_back(std::format("render {}", context.FrameIndex));
				Frames.push_back({
					.FrameIndex = context.FrameIndex,
					.FrameSlot = context.FrameSlot,
					.Width = context.Width,
					.Height = context.Height,
					.TargetWidth = context.Target != nullptr ? context.Target->getDesc().width : 0,
					.FramebufferWidth = context.Framebuffer != nullptr ? context.Framebuffer->getFramebufferInfo().width : 0,
					.TargetFormat = context.Target != nullptr ? context.Target->getDesc().format : nvrhi::Format::UNKNOWN,
					.HasDevice = context.Device == GetContext().GetGraphicsDevice() && context.Device != nullptr,
					.HasCommandList = context.CommandList != nullptr,
					.HasProfiler = context.Profiler != nullptr,
				});
			}

			void OnImGuiRender() override
			{
				Calls.push_back("imgui");
			}
		};

		// Reports one GPU message of `Severity` through its device's diagnostics during its first update, as a validation
		// message would; or, with LeakAtShutdown, leaves a host image counted as alive, which only the device's teardown
		// reports.
		class GpuErrorApplication final : public Application
		{
		public:
			explicit GpuErrorApplication(ApplicationSpecification specification)
				: Application(std::move(specification))
			{
			}
		public:
			std::optional<GpuMessageSeverity> Severity = GpuMessageSeverity::Error;
			bool LeakAtShutdown = false;
		protected:
			void OnUpdate(const FrameTime& frame) override
			{
				GraphicsDevice* device = GetContext().GetGraphicsDevice();
				if (frame.FrameIndex == 0 && device != nullptr && Severity.has_value())
					device->GetDiagnostics().ReportMessage(*Severity, "ApplicationTests", "a provoked GPU message");
			}

			void OnShutdown() override
			{
				GraphicsDevice* device = GetContext().GetGraphicsDevice();
				if (LeakAtShutdown && device != nullptr)
					device->GetResourceTracker().RecordCreated(GpuResourceType::HostImage);
			}
		};

		// Clears its frames and draws nothing, so only the frame's clear uses its target. The submission of frame 0 is held
		// back on the GPU (Test::GpuSubmissionGate), the window is resized in frame 1, which replaces the frame target while
		// frame 0, which cleared the previous one, is still in flight, and the gate opens in frame 2.
		class ResizingApplication final : public Application
		{
		public:
			explicit ResizingApplication(ApplicationSpecification specification)
				: Application(std::move(specification))
			{
			}
		public:
			std::vector<uint32_t> FrameWidths;
		protected:
			Status OnInitialize() override
			{
				ENGINE_TRY_ASSIGN(m_Gate, Test::GpuSubmissionGate::Create(*GetContext().GetGraphicsDevice()));
				return {};
			}

			void OnShutdown() override
			{
				m_Gate.reset();
			}

			void OnUpdate(const FrameTime& frame) override
			{
				if (frame.FrameIndex == 1)
					GetContext().GetWindow()->SetSize(96, 80);
				if (frame.FrameIndex == 2)
					m_Gate->Open();
			}

			void OnRender(RenderContext& context) override
			{
				FrameWidths.push_back(context.Width);
				if (context.FrameIndex == 0)
					m_Gate->HoldNextSubmission();
			}
		private:
			Scope<Test::GpuSubmissionGate> m_Gate;
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

	// The Vulkan renderer as the GPU tests use it (§15.3): validation and synchronization validation on, the run's API cap,
	// and any GPU error fails the run.
	static void UseTestRenderer(ApplicationSpecification& specification)
	{
		specification.Renderer = RendererMode::Vulkan;
		specification.Graphics.Validation = true;
		specification.Graphics.SynchronizationValidation = true;
		specification.Graphics.MaxApiVersion = Test::GetTestOptions().VulkanApi;
		specification.ExpectNoGpuErrors = true;
	}

	// The calls a run of `frames` rendered frames records: each frame's update, render and, with ImGui, its UI.
	static std::vector<std::string> ExpectedRenderingCalls(uint64_t frames, bool imgui)
	{
		std::vector<std::string> calls;
		for (uint64_t frame = 0; frame < frames; ++frame)
		{
			calls.push_back(std::format("update {}", frame));
			calls.push_back(std::format("render {}", frame));
			if (imgui)
				calls.push_back("imgui");
		}
		return calls;
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
			* doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			ApplicationSpecification specification = MakeHeadlessSpecification(3);
			specification.Renderer = RendererMode::Vulkan;
			specification.Graphics.Validation = true;
			specification.Graphics.MaxApiVersion = Test::GetTestOptions().VulkanApi;

			// Without the option the error is only logged, and the run succeeds.
			{
				GpuErrorApplication application(specification);
				Test::ExpectLog reported(LogLevel::Error, "a provoked GPU message");
				CHECK(application.Run() == ExitCode::Success);
			}

			// With it the run fails, naming the counts.
			specification.ExpectNoGpuErrors = true;
			{
				GpuErrorApplication application(specification);
				Test::ExpectLog reported(LogLevel::Error, "a provoked GPU message");
				Test::ExpectLog failed(LogLevel::Error, "reported 1 error(s) and 0 warning(s) during the run (--expect-no-gpu-errors)");
				CHECK(application.Run() == ExitCode::Failed);
			}

			// A warning fails it too, like a warning fails an in-process GPU test.
			{
				GpuErrorApplication application(specification);
				application.Severity = GpuMessageSeverity::Warning;
				Test::ExpectLog reported(LogLevel::Warn, "a provoked GPU message");
				Test::ExpectLog failed(LogLevel::Error, "reported 0 error(s) and 1 warning(s) during the run (--expect-no-gpu-errors)");
				CHECK(application.Run() == ExitCode::Failed);
			}

			// So does what only the device's teardown reports: an object still alive when the device is destroyed.
			GpuErrorApplication application(std::move(specification));
			application.Severity.reset();
			application.LeakAtShutdown = true;
			Test::ExpectLog leak(LogLevel::Error, "GPU objects are still alive when the device is destroyed: HostImage: 1");
			Test::ExpectLog failed(LogLevel::Error, "reported 1 error(s) and 0 warning(s) during the run (--expect-no-gpu-errors)");
			CHECK(application.Run() == ExitCode::Failed);
		}

		TEST_CASE("Application: a headless frame target replaced by a resize outlives the frames that cleared it"
			* doctest::test_suite(Test::GpuSuite))
		{
			// The previous target is destroyed only after frame 0 completed; destroying it while frame 0 is held back on the
			// GPU would be reported by the validation layer (an image in use by a pending command buffer) and fail the run.
			if (!Test::ProbeGpuForProcess())
				return;
			ApplicationSpecification specification = MakeHeadlessSpecification(4);
			UseTestRenderer(specification);
			ResizingApplication application(std::move(specification));
			CHECK(application.Run() == ExitCode::Success);
			const std::vector<uint32_t> widths = { 64, 96, 96, 96 };
			CHECK(application.FrameWidths == widths);
		}

		TEST_CASE("Application: a headless Vulkan application renders every frame into its offscreen target"
			* doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			ApplicationSpecification specification = MakeHeadlessSpecification(3);
			UseTestRenderer(specification);
			RenderingApplication application(std::move(specification));
			CHECK(application.Run() == ExitCode::Success);
			CHECK_FALSE(application.HadImGuiLayer);
			CHECK(application.Calls == ExpectedRenderingCalls(3, false));

			// Each frame gets its open command list, the frame's profiler and the offscreen target of the window's size.
			REQUIRE(application.Frames.size() == 3);
			for (const RenderedFrame& frame : application.Frames)
			{
				CAPTURE(frame.FrameIndex);
				CHECK(frame.HasDevice);
				CHECK(frame.HasCommandList);
				CHECK(frame.HasProfiler);
				CHECK(frame.FrameSlot == frame.FrameIndex % 2);
				CHECK(frame.Width == 64);
				CHECK(frame.Height == 64);
				CHECK(frame.TargetWidth == 64);
				CHECK(frame.FramebufferWidth == 64);
				CHECK(frame.TargetFormat == nvrhi::Format::RGBA8_UNORM);
			}
		}

		TEST_CASE("Application: with ImGui each rendered frame submits its UI after OnRender"
			* doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			ApplicationSpecification specification = MakeHeadlessSpecification(3);
			UseTestRenderer(specification);
			specification.EnableImGui = true; // no ImGuiIniPath: no ini file is written
			RenderingApplication application(std::move(specification));
			CHECK(application.Run() == ExitCode::Success);
			CHECK(application.HadImGuiLayer);
			CHECK(application.Calls == ExpectedRenderingCalls(3, true));
		}

		// Runs only in a windowed child process (Support/WindowedChild.h): a windowed application on a swapchain, minimized
		// from the update of frame 3 and restored from the update of frame 6.
		TEST_CASE("Application: a windowed Vulkan application presents its frames except while minimized"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			ApplicationSpecification specification;
			specification.Name = "ApplicationTests";
			specification.Window = WindowMode::Windowed;
			specification.WindowSettings = { .Title = "Application test", .Width = 320, .Height = 240 };
			specification.Clock = ClockKind::Manual;
			specification.MaxFrames = 10;
			specification.WorkerCount = 0;
			specification.EnableImGui = true;
			UseTestRenderer(specification);
			RenderingApplication application(std::move(specification));
			application.MinimizeAtFrame = 3;
			application.RestoreAtFrame = 6;
			CHECK(application.Run() == ExitCode::Success);
			CHECK(application.WindowChangesArrived);
			CHECK(application.HadImGuiLayer);

			// Frames 0 to 2 render; the minimized frames 3 to 5 render nothing and call neither hook; after the restore at most
			// one frame (the swapchain's recreation) is skipped.
			std::vector<uint64_t> rendered;
			for (const RenderedFrame& frame : application.Frames)
			{
				rendered.push_back(frame.FrameIndex);
				CHECK(frame.HasDevice);
				CHECK(frame.HasCommandList);
				CHECK(frame.HasProfiler);
				CHECK(frame.FrameSlot == frame.FrameIndex % 2);
				CHECK(frame.Width > 0);
				CHECK(frame.TargetWidth == frame.Width);
				CHECK(frame.FramebufferWidth == frame.Width);
				CHECK((frame.TargetFormat == nvrhi::Format::BGRA8_UNORM || frame.TargetFormat == nvrhi::Format::RGBA8_UNORM));
			}
			CAPTURE(rendered.size());
			CHECK(std::ranges::find(rendered, 0u) != rendered.end());
			CHECK(std::ranges::find(rendered, 2u) != rendered.end());
			for (const uint64_t minimized : { 3u, 4u, 5u })
				CHECK(std::ranges::find(rendered, minimized) == rendered.end());
			for (const uint64_t restored : { 7u, 8u, 9u })
				CHECK(std::ranges::find(rendered, restored) != rendered.end());
			CHECK(std::ranges::count(application.Calls, std::string("imgui")) == static_cast<std::ptrdiff_t>(rendered.size()));
		}

		TEST_CASE("Application: a windowed application presents through its swapchain and skips its minimized frames"
			* doctest::test_suite(Test::GpuSuite))
		{
			std::vector<std::string> arguments = {
				"--windowed-child=Application: a windowed Vulkan application presents its frames except while minimized",
			};
			const std::vector<std::string> gpuArguments = Test::GetGpuTestsChildArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			const Result<ProcessResult> child = Process::Run(Test::MakeTestsChildSpecification(std::move(arguments)), std::chrono::seconds(60));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stdout: ", child->StandardOutput);
			INFO("child stderr: ", child->StandardError);
			CHECK(child->ExitCode == 0);
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

		TEST_CASE("ApplyEngineCommandLine: --expect-no-errors sets ExpectNoErrors")
		{
			// Implemented by the contract (the option and its parse); the check itself is stream C's (Application::Run).
			const std::vector<std::string> arguments = { "--expect-no-errors" };
			const Result<CommandLine> commandLine = CommandLine::Parse(arguments, GetEngineCommandLineOptions());
			REQUIRE(commandLine.has_value());
			ApplicationSpecification specification;
			REQUIRE(ApplyEngineCommandLine(*commandLine, specification).has_value());
			CHECK(specification.ExpectNoErrors);
		}

		TEST_CASE("Application: --expect-no-errors fails a run that logged an error" * doctest::skip(true))
		{
			// Skipped skeleton of the M7 contract (Docs/Decisions/0012-m7-decisions.md decision 15); stream C.
			class ErrorLoggingApplication final : public Application
			{
			public:
				using Application::Application;
			protected:
				void OnUpdate(const FrameTime& /*frame*/) override
				{
					ENGINE_ERROR("an error the run logs on purpose");
				}
			};
			ApplicationSpecification specification = MakeHeadlessSpecification(2);
			specification.ExpectNoErrors = true;
			ErrorLoggingApplication application(std::move(specification));
			Test::ExpectLog logged(LogLevel::Error, "an error the run logs on purpose");
			Test::ExpectLog failed(LogLevel::Error, "--expect-no-errors");
			CHECK(application.Run() == ExitCode::Failed);
		}
	}

}
