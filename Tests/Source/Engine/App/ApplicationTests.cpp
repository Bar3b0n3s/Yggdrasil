#include "TestsPCH.h"

#include "Engine/App/Application.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/Platform/Process.h"
#include "Support/ChildOutput.h"
#include "Support/DeathTest.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/TrimCounter.h"
#include "Support/Utf8Path.h"
#include "Support/WindowedChild.h"

#include <imgui.h>
#include <vulkan/vulkan.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

		// M18 (C18.3): loses its device in the update of frame `LoseAtFrame` (the test hooks' flag) and records the frames
		// it renders, the recovery hooks and the GpuDeviceLost events its run appended.
		class LosingApplication final : public Application
		{
		public:
			explicit LosingApplication(ApplicationSpecification specification)
				: Application(std::move(specification))
			{
			}
		public:
			std::vector<std::string> Calls;
			uint64_t LoseAtFrame = 1;
			size_t DeviceLostEvents = 0;
			uint32_t RecoveryCount = 0;
		protected:
			void OnUpdate(const FrameTime& frame) override
			{
				Calls.push_back(std::format("update {}", frame.FrameIndex));
				if (frame.FrameIndex == LoseAtFrame)
					GetContext().GetGraphicsDevice()->GetDiagnostics().SetDeviceLost(true);
			}

			void OnRender(RenderContext& /*context*/) override
			{
				Calls.push_back("render");
			}

			void OnDeviceLost(const GpuDeviceLossReport& report) override
			{
				Calls.push_back(report.Injected ? "lost (injected)" : "lost");
			}

			void OnDeviceRecovered(const GpuDeviceLossReport& /*report*/) override
			{
				Calls.push_back("recovered");
			}

			void OnShutdown() override
			{
				const std::array types = { EngineEventType::GpuDeviceLost };
				DeviceLostEvents = GetContext().GetEventLog().Read(0, types).Events.size();
				RecoveryCount = GetContext().GetGraphicsDevice()->GetRecoveryCount();
			}
		};

		// M18 (C18.3, decision 28): loses its device in the update of frame `LoseAtFrame` (the test hooks' flag) and clears the
		// flag itself in the next frame's update (GpuDiagnostics::ClearDeviceLost), as the recovery at the frame boundary
		// does, so what it records is the lost frame alone: the steps, updates and rendered frames, with the index and slot of
		// each rendered frame. Where the frame boundary recovers the device, the clear finds the flag cleared already.
		class LostFrameApplication final : public Application
		{
		public:
			explicit LostFrameApplication(ApplicationSpecification specification)
				: Application(std::move(specification))
			{
			}
		public:
			std::vector<std::string> Calls;
			std::vector<RenderedFrame> Frames;
			uint64_t LoseAtFrame = 1;
		protected:
			void OnFixedStep(const SimStep& step) override
			{
				Calls.push_back(std::format("step {}", step.Tick));
			}

			void OnUpdate(const FrameTime& frame) override
			{
				Calls.push_back(std::format("update {}", frame.FrameIndex));
				GpuDiagnostics& diagnostics = GetContext().GetGraphicsDevice()->GetDiagnostics();
				if (frame.FrameIndex == LoseAtFrame)
					diagnostics.SetDeviceLost(true);
				else if (frame.FrameIndex == LoseAtFrame + 1)
					diagnostics.ClearDeviceLost();
			}

			void OnRender(RenderContext& context) override
			{
				Calls.push_back("render");
				Frames.push_back({ .FrameIndex = context.FrameIndex, .FrameSlot = context.FrameSlot });
			}
		};

		// M18 (C18.3, decision 26 and ADR 0029 risk 10): in the UI frame of frame `LoseAtFrame` opens a window and throws the
		// vk::DeviceLostError NVRHI's internal calls throw on a lost device, so the frame-boundary catch ends the frame inside
		// OnImGuiRender with its UI frame begun; records the hooks, the GpuDeviceLost events and the recoveries.
		class UiLosingApplication final : public Application
		{
		public:
			explicit UiLosingApplication(ApplicationSpecification specification)
				: Application(std::move(specification))
			{
			}
		public:
			std::vector<std::string> Calls;
			uint64_t LoseAtFrame = 1;
			size_t DeviceLostEvents = 0;
			uint32_t RecoveryCount = 0;
		protected:
			void OnUpdate(const FrameTime& frame) override
			{
				Calls.push_back(std::format("update {}", frame.FrameIndex));
				m_Frame = frame.FrameIndex;
			}

			void OnRender(RenderContext& /*context*/) override
			{
				Calls.push_back("render");
			}

			void OnImGuiRender() override
			{
				Calls.push_back("imgui");
				ImGui::Begin("Interrupted");
				ImGui::TextUnformatted("A window the loss interrupts");
				if (m_Frame == LoseAtFrame)
					vk::detail::resultCheck(vk::Result::eErrorDeviceLost, "ApplicationTests: a device loss thrown out of NVRHI during the UI frame");
				ImGui::End();
			}

			void OnDeviceLost(const GpuDeviceLossReport& /*report*/) override
			{
				Calls.push_back("lost");
			}

			void OnDeviceRecovered(const GpuDeviceLossReport& /*report*/) override
			{
				Calls.push_back("recovered");
			}

			void OnShutdown() override
			{
				const std::array types = { EngineEventType::GpuDeviceLost };
				DeviceLostEvents = GetContext().GetEventLog().Read(0, types).Events.size();
				RecoveryCount = GetContext().GetGraphicsDevice()->GetRecoveryCount();
			}
		private:
			uint64_t m_Frame = 0;
		};

		// M18 (C18.3, decision 25): loses its device during OnInitialize, before its first frame; a frame that ran would log.
		class InitializeLosingApplication final : public Application
		{
		public:
			explicit InitializeLosingApplication(ApplicationSpecification specification)
				: Application(std::move(specification))
			{
			}
		protected:
			Status OnInitialize() override
			{
				GetContext().GetGraphicsDevice()->GetDiagnostics().SetDeviceLost(true);
				return {};
			}

			void OnUpdate(const FrameTime& frame) override
			{
				ENGINE_CORE_ERROR("ApplicationTests: frame {} ran after a device loss before the first frame", frame.FrameIndex);
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

		// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decisions 42 and 44): resizes its headless window in frames 1 and 3,
		// and records the width of every frame it renders and how many times the device trimmed (an owner of its own that
		// counts its TrimUnused calls).
		class OutOfMemoryResizeApplication final : public Application
		{
		public:
			explicit OutOfMemoryResizeApplication(ApplicationSpecification specification)
				: Application(std::move(specification))
			{
			}
		public:
			std::vector<uint32_t> FrameWidths;
			// The device's trims in the run (Test::TrimCounter), read at the shutdown.
			uint32_t Trims = 0;
		protected:
			Status OnInitialize() override
			{
				m_TrimCounter = CreateScope<Test::TrimCounter>(GetContext().GetGraphicsDevice()->GetRecreation());
				return {};
			}

			void OnShutdown() override
			{
				if (m_TrimCounter)
					Trims = m_TrimCounter->GetTrims();
				m_TrimCounter.reset();
			}

			void OnUpdate(const FrameTime& frame) override
			{
				if (frame.FrameIndex == 1)
					GetContext().GetWindow()->SetSize(96, 80);
				if (frame.FrameIndex == 3)
					GetContext().GetWindow()->SetSize(112, 72);
			}

			void OnRender(RenderContext& context) override
			{
				FrameWidths.push_back(context.Width);
			}
		private:
			Scope<Test::TrimCounter> m_TrimCounter;
		};

	}

	// A headless application, which the headless Tests process can run in place.
	namespace {

		// A mount that serves nothing and logs an error when the context's file system destroys it: an error logged while the
		// context tears down, after OnShutdown and the GPU services.
		class ErrorAtTeardownMount final : public IMount
		{
		public:
			~ErrorAtTeardownMount() override
			{
				ENGINE_ERROR("the test mount logged an error at teardown");
			}

			[[nodiscard]] Result<Buffer> ReadFile(const VfsPath& /*path*/) const override { return MakeError(ErrorCode::NotFound, "an empty mount"); }
			[[nodiscard]] Result<Scope<IFileStream>> Open(const VfsPath& /*path*/) const override { return MakeError(ErrorCode::NotFound, "an empty mount"); }
			[[nodiscard]] Status WriteFileAtomic(const VfsPath& /*path*/, std::span<const std::byte> /*data*/) override
			{
				return MakeError(ErrorCode::PermissionDenied, "a read-only mount");
			}
			[[nodiscard]] Status WriteNewFile(const VfsPath& /*path*/, std::span<const std::byte> /*data*/) override
			{
				return MakeError(ErrorCode::PermissionDenied, "a read-only mount");
			}
			[[nodiscard]] Result<FileInfo> GetInfo(const VfsPath& /*path*/) const override { return MakeError(ErrorCode::NotFound, "an empty mount"); }
			[[nodiscard]] Result<std::vector<VfsEntry>> List(const VfsPath& /*directory*/, bool /*recursive*/) const override
			{
				return MakeError(ErrorCode::NotFound, "an empty mount");
			}
			[[nodiscard]] Status CreateDirectories(const VfsPath& /*directory*/) override { return MakeError(ErrorCode::PermissionDenied, "a read-only mount"); }
			[[nodiscard]] Status Remove(const VfsPath& /*path*/) override { return MakeError(ErrorCode::PermissionDenied, "a read-only mount"); }
			[[nodiscard]] Status RemoveEmptyDirectory(const VfsPath& /*directory*/) override { return MakeError(ErrorCode::PermissionDenied, "a read-only mount"); }
			[[nodiscard]] Status Move(const VfsPath& /*from*/, const VfsPath& /*to*/) override { return MakeError(ErrorCode::PermissionDenied, "a read-only mount"); }
			[[nodiscard]] MountAccess GetAccess() const override { return MountAccess::ReadOnly; }
		};

	}

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

	namespace {

		// M15 (--log-level, §13.9): logs an info line, a warning and an error in each frame and records the output level the
		// run set.
		class LevelLoggingApplication final : public Application
		{
		public:
			explicit LevelLoggingApplication(ApplicationSpecification specification)
				: Application(std::move(specification))
			{
			}
		public:
			LogLevel OutputLevelDuringRun = LogLevel::Trace;
		protected:
			void OnUpdate(const FrameTime& /*frame*/) override
			{
				OutputLevelDuringRun = Log::GetOutputLevel();
				ENGINE_INFO("an info line below the output level");
				ENGINE_WARN("a warning below the output level");
				ENGINE_ERROR("an error at the output level");
			}
		};

	}

	// The whole content of a file; empty when it cannot be read.
	static std::string ReadTextFile(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	}

	// How many of `entries` contain `needle`, and how often `text` does.
	static size_t CountContaining(const std::vector<LogEntry>& entries, std::string_view needle)
	{
		return static_cast<size_t>(std::ranges::count_if(entries, [needle](const LogEntry& entry)
		{
			return entry.Message.contains(needle);
		}));
	}

	static size_t CountContaining(std::string_view text, std::string_view needle)
	{
		size_t count = 0;
		for (size_t position = text.find(needle); position != std::string_view::npos; position = text.find(needle, position + needle.size()))
			++count;
		return count;
	}

	// --log-level in a process with both output sinks (M15, §13.9): the run's output level is Error, so its info line and
	// warning reach neither stderr nor the log file, while the ring buffer keeps all three entries and --expect-no-errors
	// fails the run; the level is restored when the run returns. The log goes back to the console alone before the
	// temporary directory is removed, and the final assert reports what the file and the ring buffer received.
	ENGINE_DEATH_TEST("App/LogLevelFiltersOutputSinksOnly")
	{
		std::string report;
		{
			const Test::TempDirectory directory("LogLevel");
			const std::filesystem::path logFile = directory / "Logs/Tests.log";
			Log::Shutdown();
			const Status withFile = Log::Initialize({ .FilePath = logFile });
			ENGINE_CORE_ASSERT(withFile.has_value(), "the log with a file sink could not be initialized");
			const uint64_t cursor = Log::GetRingBuffer().GetNextSeq();

			ApplicationSpecification specification = MakeHeadlessSpecification(1);
			specification.ExpectNoErrors = true;
			specification.MinimumLogLevel = LogLevel::Error;
			LevelLoggingApplication application(std::move(specification));
			const int exitCode = application.Run();
			const LogLevel restored = Log::GetOutputLevel();
			ENGINE_CORE_INFO("an info line after the run");
			Log::Flush();

			LogQuery query;
			query.Cursor = cursor;
			query.Limit = 10000;
			const std::vector<LogEntry> entries = Log::GetRingBuffer().Read(query).Entries;
			const std::string file = ReadTextFile(logFile);
			Log::Shutdown();
			const Status consoleOnly = Log::Initialize(LogSpecification());
			ENGINE_CORE_ASSERT(consoleOnly.has_value(), "the console-only log could not be restored");
			report = std::format("exit code {}, output level {} during the run and {} after it; ring buffer info {} warn {} error {}; "
								 "file info {} warn {} error {} after {}",
				exitCode, LogLevelToString(application.OutputLevelDuringRun), LogLevelToString(restored),
				CountContaining(entries, "an info line below the output level"), CountContaining(entries, "a warning below the output level"),
				CountContaining(entries, "an error at the output level"), CountContaining(file, "an info line below the output level"),
				CountContaining(file, "a warning below the output level"), CountContaining(file, "an error at the output level"),
				CountContaining(file, "an info line after the run"));
		}
		ENGINE_CORE_ASSERT(false, "Log level report: {}", report);
	}

	// M18 (C18.3, decision 25): a device lost during OnInitialize ends the process before the first frame, through
	// GraphicsDevice::RaiseDeviceLost. Reaching the end of the body (a recovered or ignored loss) fails the death test.
	ENGINE_DEATH_TEST("App/DeviceLostBeforeFirstFrame")
	{
		ApplicationSpecification specification = MakeHeadlessSpecification(2);
		UseTestRenderer(specification);
		InitializeLosingApplication application(std::move(specification));
		const int exitCode = application.Run();
		ENGINE_CORE_ERROR("ApplicationTests: the application returned {} after a device loss before its first frame", exitCode);
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

		TEST_CASE("Application: a headless frame resize out of memory is retried after a trim, or skips its frames until the next size"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.4, Docs/Decisions/0029-m18-contract.md decisions 42, 44 and 46): under oom-rendertarget the resized
			// frame target fails once, is created again after the trim and the frames render at the new size; under
			// oom-rendertarget-persistent every frame at a size that failed renders nothing, each new size is tried again (one trim
			// each), and the run goes on to its end without a GPU message (--expect-no-gpu-errors) and without a fatal error.
			if (!Test::ProbeGpuForProcess())
				return;
			for (const GpuFault fault : { GpuFault::OomRenderTarget, GpuFault::OomRenderTargetPersistent })
			{
				CAPTURE(GpuFaultToString(fault));
				ApplicationSpecification specification = MakeHeadlessSpecification(5);
				UseTestRenderer(specification);
				specification.Graphics.InjectFault = fault;
				OutOfMemoryResizeApplication application(std::move(specification));
				CHECK(application.Run() == ExitCode::Success);
				if (fault == GpuFault::OomRenderTarget)
				{
					CHECK(application.FrameWidths == std::vector<uint32_t>{ 64, 96, 96, 112, 112 });
					CHECK(application.Trims == 1);
				}
				else
				{
					CHECK(application.FrameWidths == std::vector<uint32_t>{ 64 });
					CHECK(application.Trims == 2);
				}
			}
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

		TEST_CASE("Application: a device loss is recovered at the frame boundary and the run goes on"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.3, Docs/Decisions/0029-m18-contract.md decision 29): the lost frame renders nothing, the
			// recovery runs OnDeviceLost, recovers the device with the frame's objects and the ImGui renderer, appends one
			// GpuDeviceLost event and calls OnDeviceRecovered, and the next frames render; no GPU message, and the run succeeds.
			if (!Test::ProbeGpuForProcess())
				return;
			ApplicationSpecification specification = MakeHeadlessSpecification(4);
			UseTestRenderer(specification);
			specification.EnableImGui = true;
			LosingApplication application(std::move(specification));
			CHECK(application.Run() == ExitCode::Success);
			CHECK(application.Calls == std::vector<std::string>{ "update 0", "render", "update 1", "lost (injected)", "recovered", "update 2", "render", "update 3", "render" });
			CHECK(application.DeviceLostEvents == 1);
			CHECK(application.RecoveryCount == 1);
		}

		TEST_CASE("Application: a frame on a lost device renders nothing while its steps and update run"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3, decision 28): the simulation never needs the GPU, so a play session goes on through a loss; the lost
			// frame calls no rendering hook and submits nothing, and the frames after it render again.
			if (!Test::ProbeGpuForProcess())
				return;
			ApplicationSpecification specification = MakeHeadlessSpecification(4);
			UseTestRenderer(specification);
			LostFrameApplication application(std::move(specification));
			CHECK(application.Run() == ExitCode::Success);
			const std::vector<std::string> expected = {
				"step 0",
				"update 0",
				"render",
				"step 1",
				"update 1",
				"step 2",
				"update 2",
				"render",
				"step 3",
				"update 3",
				"render",
			};
			CHECK(application.Calls == expected);
		}

		TEST_CASE("Application: rendered frames keep growing indices that match their slots across a device loss"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3, decision 33): a recovery at the frame boundary replaces the frame's FramePacer, which counts its
			// frames from 0 again; the frames after it continue from the first multiple of the frames in flight after the last
			// index, so renderers, whose GPU timing history ignores an older frame, keep their timings, and every frame's slot
			// is its index modulo the frames in flight (2).
			if (!Test::ProbeGpuForProcess())
				return;
			ApplicationSpecification specification = MakeHeadlessSpecification(5);
			UseTestRenderer(specification);
			LostFrameApplication application(std::move(specification));
			application.LoseAtFrame = 2;
			CHECK(application.Run() == ExitCode::Success);
			REQUIRE(application.Frames.size() == 4);
			for (size_t frame = 0; frame < application.Frames.size(); ++frame)
			{
				CAPTURE(frame);
				CAPTURE(application.Frames[frame].FrameIndex);
				CHECK(application.Frames[frame].FrameSlot == application.Frames[frame].FrameIndex % 2);
				if (frame > 0)
					CHECK(application.Frames[frame].FrameIndex > application.Frames[frame - 1].FrameIndex);
			}
		}

		TEST_CASE("Application: a device loss thrown inside the UI frame is recovered, and the next UI frame begins"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3, decision 26 and ADR 0029 risk 10; through the frame-boundary catch and recovery and the
			// ImGuiRenderer owner): the frame-boundary catch reports the thrown loss and ends the frame inside OnImGuiRender,
			// with the window it opened; the "Application frame" owner's release ends that UI frame (ImGuiLayer::
			// EndInterruptedFrame), so the frames after the recovery draw their UI.
			if (!Test::ProbeGpuForProcess())
				return;
			ApplicationSpecification specification = MakeHeadlessSpecification(4);
			UseTestRenderer(specification);
			specification.EnableImGui = true;
			UiLosingApplication application(std::move(specification));
			CHECK(application.Run() == ExitCode::Success);
			const std::vector<std::string> expected = {
				"update 0",
				"render",
				"imgui",
				"update 1",
				"render",
				"imgui",
				"lost",
				"recovered",
				"update 2",
				"render",
				"imgui",
				"update 3",
				"render",
				"imgui",
			};
			CHECK(application.Calls == expected);
			CHECK(application.DeviceLostEvents == 1);
			CHECK(application.RecoveryCount == 1);
		}

		TEST_CASE("Application: a device loss before the first frame exits 4 with a crash report"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3, decision 25): the startup objects cannot be created on the lost device and nothing has rendered, so
			// the loss is not recovered: the process ends through FatalError(DeviceLost) without running a frame.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("DeviceLostBeforeFirstFrame");
			std::vector<std::string> arguments = Test::GetGpuTestsChildArguments();
			arguments.push_back("--death-test=App/DeviceLostBeforeFirstFrame");
			arguments.push_back("--user-data-dir=" + Test::PathToUtf8(userData.GetPath()));
			const Result<ProcessResult> result = Process::Run(Test::MakeTestsChildSpecification(std::move(arguments)), std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("child stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Crash);
			// No frame ran: a loss left for the frames (or for the shutdown's idle wait) would run them first.
			CHECK_FALSE(result->StandardError.contains("ran after a device loss before the first frame"));
			const Result<std::string> report = Test::ReadOnlyCrashReport(userData.GetPath());
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains("Reason: Fatal error (DeviceLost)"));
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
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
			// No test touches the machine's audio device (§15.1 T1): a device-less, deterministic engine instead of the
			// windowed default (System, Threaded).
			specification.Audio = AudioEngineSpecification{};
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

		// Runs only in a windowed child process (Support/WindowedChild.h), M18 (C18.3, Docs/Decisions/0029-m18-contract.md
		// decisions 29 and 32): a windowed application on a swapchain loses its device in the update of frame 2. The recovery
		// releases the "Application frame" owner's swapchain and surface before the old VkInstance is destroyed, creates the
		// device again with Recover(window) (presentation support scored again) and the swapchain on a new surface, and the
		// frames after it present. ExpectNoGpuErrors fails the run on any GPU message of the device's whole life.
		TEST_CASE("Application: a windowed run loses its device and presents again after the recovery"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			ApplicationSpecification specification;
			specification.Name = "ApplicationTests";
			specification.Window = WindowMode::Windowed;
			specification.WindowSettings = { .Title = "Application test", .Width = 320, .Height = 240 };
			specification.Clock = ClockKind::Manual;
			specification.MaxFrames = 6;
			specification.WorkerCount = 0;
			specification.EnableImGui = true;
			// No test touches the machine's audio device (§15.1 T1).
			specification.Audio = AudioEngineSpecification{};
			UseTestRenderer(specification);
			LosingApplication application(std::move(specification));
			application.LoseAtFrame = 2;
			CHECK(application.Run() == ExitCode::Success);
			const auto lost = std::ranges::find(application.Calls, std::string("lost (injected)"));
			REQUIRE(lost != application.Calls.end());
			REQUIRE(std::next(lost) != application.Calls.end());
			CHECK(*std::next(lost) == "recovered");
			// Frames 3 to 5 render after the recovery; at most one is skipped while the swapchain is created again.
			CHECK(std::count(lost, application.Calls.end(), std::string("render")) >= 2);
			CHECK(application.DeviceLostEvents == 1);
			CHECK(application.RecoveryCount == 1);
		}

		TEST_CASE("Application: a windowed run recovers its swapchain after an injected loss and presents the next frame without a GPU message"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.3): the recovery of a presenting device, which the headless cases and the shared loss-test
			// helper (Support/DeviceLossTest.h, Recover(nullptr)) cannot reach: the swapchain's and the surface's release and
			// creation across the recreated VkInstance, and Recover(window).
			std::vector<std::string> arguments = {
				"--windowed-child=Application: a windowed run loses its device and presents again after the recovery",
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

		// M15 (§13.9; Docs/Decisions/0024-m15-contract.md decision 17, stream D).
		TEST_CASE("ApplyEngineCommandLine: --log-level sets the output sinks' minimum level and refuses other spellings")
		{
			const std::span<const CommandLineOption> options = GetEngineCommandLineOptions();
			REQUIRE(std::ranges::any_of(options, [](const CommandLineOption& option)
			{
				return option.Name == "--log-level";
			}));
			for (const auto& [text, level] : std::vector<std::pair<std::string, LogLevel>>{
					 { "trace", LogLevel::Trace }, { "info", LogLevel::Info }, { "warn", LogLevel::Warn }, { "error", LogLevel::Error }, { "critical", LogLevel::Critical } })
			{
				CAPTURE(text);
				const std::vector<std::string> arguments = { "--headless", "--log-level", text };
				const Result<CommandLine> commandLine = CommandLine::Parse(arguments, options);
				REQUIRE(commandLine.has_value());
				ApplicationSpecification specification;
				REQUIRE(ApplyEngineCommandLine(*commandLine, specification).has_value());
				CHECK(specification.MinimumLogLevel == std::optional<LogLevel>(level));
			}
			const std::vector<std::string> invalid = { "--log-level", "verbose" };
			const Result<CommandLine> commandLine = CommandLine::Parse(invalid, options);
			REQUIRE(commandLine.has_value());
			ApplicationSpecification specification;
			const Status applied = ApplyEngineCommandLine(*commandLine, specification);
			REQUIRE_FALSE(applied.has_value());
			CHECK(applied.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(applied.error().GetMessageText().contains("--log-level"));
			CHECK(applied.error().GetMessageText().contains("'critical'"));

			// The spellings are exact, like every engine option's values; without the option nothing is set.
			for (const std::string_view spelling : { "Error", "WARN", "warning", "debug" })
			{
				CAPTURE(std::string(spelling));
				const std::vector<std::string> arguments = { std::format("--log-level={}", spelling) };
				const Result<CommandLine> parsed = CommandLine::Parse(arguments, options);
				REQUIRE(parsed.has_value());
				ApplicationSpecification rejected;
				CHECK_FALSE(ApplyEngineCommandLine(*parsed, rejected).has_value());
				CHECK_FALSE(rejected.MinimumLogLevel.has_value());
			}
			const Result<CommandLine> none = CommandLine::Parse({}, options);
			REQUIRE(none.has_value());
			ApplicationSpecification defaults;
			REQUIRE(ApplyEngineCommandLine(*none, defaults).has_value());
			CHECK_FALSE(defaults.MinimumLogLevel.has_value());
		}

		TEST_CASE("Application: --log-level filters the console and file sinks only, so --expect-no-errors still counts errors")
		{
			// In place: the run sets the output level and restores the previous one when it returns, while the log listeners
			// (Test::ExpectLog, the --expect-no-errors counter) receive every entry, so the filtered error still fails the run.
			{
				ApplicationSpecification specification = MakeHeadlessSpecification(1);
				specification.ExpectNoErrors = true;
				specification.MinimumLogLevel = LogLevel::Critical;
				LevelLoggingApplication application(std::move(specification));
				const LogLevel before = Log::GetOutputLevel();
				Test::ExpectLog logged(LogLevel::Error, "an error at the output level");
				Test::ExpectLog failed(LogLevel::Error, "--expect-no-errors");
				CHECK(application.Run() == ExitCode::Failed);
				CHECK(application.OutputLevelDuringRun == LogLevel::Critical);
				CHECK(Log::GetOutputLevel() == before);
			}

			// In a process with a console and a log file: what each output sink and the ring buffer received.
			const Result<Test::DeathTestResult> result = Test::RunDeathTest("App/LogLevelFiltersOutputSinksOnly");
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("child stderr: ", result->StandardError);
			CHECK(result->ExitCode == FatalCrashExitCode);
			CHECK(result->StandardError.contains("Log level report: exit code 1, output level Error during the run and Trace after it; "
												 "ring buffer info 1 warn 1 error 1; file info 0 warn 0 error 1 after 1"));
			CHECK(result->StandardError.contains("an error at the output level"));
			CHECK(result->StandardError.contains("error(s) were logged during the run (--expect-no-errors)"));
			CHECK(result->StandardError.contains("an info line after the run"));
			CHECK_FALSE(result->StandardError.contains("an info line below the output level"));
			CHECK_FALSE(result->StandardError.contains("a warning below the output level"));
		}

		TEST_CASE("Application: an executable's --log-level filters its console and log file from the process's first line")
		{
			// RunApplication hands MinimumLogLevel to the ProcessContext, so the process's own lines ("Starting ...", "Process
			// context: Log initialized") follow it too. A logic-only editor of two frames, without the option, with
			// --log-level warn and with a spelling the option refuses.
			const Result<std::filesystem::path> editor = Test::GetBuiltExecutablePath("Editor");
			REQUIRE_MESSAGE(editor.has_value(), editor.error().ToString());
			const auto run = [&editor](const Test::TempDirectory& userData, std::vector<std::string> extra)
			{
				std::vector<std::string> arguments = {
					"--headless",
					"--renderer",
					"none",
					"--frames",
					"2",
					"--user-data-dir=" + Test::PathToUtf8(userData.GetPath()),
					"--engine-cache-dir=" + Test::PathToUtf8(userData / "EngineCache"),
				};
				arguments.insert(arguments.end(), extra.begin(), extra.end());
				return Process::Run({ .Executable = *editor, .Arguments = Test::WithoutAudioDevice(std::move(arguments)) }, std::chrono::seconds(120));
			};
			const std::string logFile = std::string(ENGINE_PRODUCT_NAME) + "/Logs/Editor.log";

			const Test::TempDirectory plainData("LogLevelPlain");
			const Result<ProcessResult> plain = run(plainData, {});
			REQUIRE_MESSAGE(plain.has_value(), plain.error().ToString());
			INFO("plain stderr: ", plain->StandardError);
			CHECK(plain->ExitCode == ExitCode::Success);
			CHECK(plain->StandardError.contains("[info] Process context: Log initialized"));
			CHECK(ReadTextFile(plainData / logFile).contains("[info] Process context: Log initialized"));

			const Test::TempDirectory warnData("LogLevelWarn");
			const Result<ProcessResult> warn = run(warnData, { "--log-level", "warn" });
			REQUIRE_MESSAGE(warn.has_value(), warn.error().ToString());
			INFO("warn stderr: ", warn->StandardError);
			CHECK(warn->ExitCode == ExitCode::Success);
			CHECK_FALSE(warn->StandardError.contains("[info]"));
			CHECK_FALSE(ReadTextFile(warnData / logFile).contains("[info]"));

			const Test::TempDirectory refusedData("LogLevelRefused");
			const Result<ProcessResult> refused = run(refusedData, { "--log-level=verbose" });
			REQUIRE_MESSAGE(refused.has_value(), refused.error().ToString());
			CHECK(refused->ExitCode == ExitCode::UsageError);
			CHECK(refused->StandardError.contains("--log-level"));
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

		TEST_CASE("ApplyEngineCommandLine: --gpu-inject-fault accepts and documents every fault")
		{
			const std::span<const CommandLineOption> options = GetEngineCommandLineOptions();
			const auto option = std::ranges::find(options, std::string_view("--gpu-inject-fault"), &CommandLineOption::Name);
			REQUIRE(option != options.end());
			for (const GpuFault fault : { GpuFault::DeviceLost, GpuFault::OomTexture, GpuFault::Hang, GpuFault::OomSpecialization,
					 GpuFault::DeviceLostUnrecoverable, GpuFault::HangReset, GpuFault::HangResetPersistent, GpuFault::OomRenderTarget,
					 GpuFault::OomRenderTargetPersistent, GpuFault::OomPipeline, GpuFault::OomPipelinePersistent, GpuFault::OomSwapchain,
					 GpuFault::OomSwapchainPersistent })
			{
				const std::string spelled(GpuFaultToString(fault));
				CAPTURE(spelled);
				CHECK(option->ValueName.contains(spelled));
				const std::vector<std::string> arguments = { "--gpu-inject-fault", spelled };
				const Result<CommandLine> commandLine = CommandLine::Parse(arguments, options);
				REQUIRE_MESSAGE(commandLine.has_value(), commandLine.error().ToString());
				ApplicationSpecification specification;
				REQUIRE(ApplyEngineCommandLine(*commandLine, specification).has_value());
				CHECK(specification.Graphics.InjectFault == fault);
			}

			// The refusal names every accepted spelling.
			const std::vector<std::string> invalid = { "--gpu-inject-fault", "crash" };
			const Result<CommandLine> commandLine = CommandLine::Parse(invalid, options);
			REQUIRE_MESSAGE(commandLine.has_value(), commandLine.error().ToString());
			ApplicationSpecification specification;
			const Status applied = ApplyEngineCommandLine(*commandLine, specification);
			REQUIRE_FALSE(applied.has_value());
			CHECK(applied.error().GetMessageText().contains("'oom-specialization'"));
			CHECK(applied.error().GetMessageText().contains("'oom-swapchain-persistent'"));
		}

		// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decision 41): the machine's GPU reset window, an engine setting.
		TEST_CASE("ApplyEngineCommandLine: --gpu-reset-wait sets the reset wait and refuses values outside 0 to 120000")
		{
			const std::span<const CommandLineOption> options = GetEngineCommandLineOptions();
			REQUIRE(std::ranges::any_of(options, [](const CommandLineOption& option)
			{
				return option.Name == "--gpu-reset-wait";
			}));
			for (const auto& [text, milliseconds] : std::vector<std::pair<std::string, uint32_t>>{
					 { "0", 0u }, { "2500", 2'500u }, { "120000", MaxGpuResetWaitMilliseconds } })
			{
				CAPTURE(text);
				const std::vector<std::string> arguments = { "--headless", "--gpu-reset-wait", text };
				const Result<CommandLine> commandLine = CommandLine::Parse(arguments, options);
				REQUIRE_MESSAGE(commandLine.has_value(), commandLine.error().ToString());
				ApplicationSpecification specification;
				REQUIRE(ApplyEngineCommandLine(*commandLine, specification).has_value());
				CHECK(specification.Graphics.ResetWaitMilliseconds == milliseconds);
			}

			// Absent: the default, long enough for a Windows TDR and a Linux GPU reset.
			const Result<CommandLine> none = CommandLine::Parse({}, options);
			REQUIRE(none.has_value());
			ApplicationSpecification defaults;
			REQUIRE(ApplyEngineCommandLine(*none, defaults).has_value());
			CHECK(defaults.Graphics.ResetWaitMilliseconds == DefaultGpuResetWaitMilliseconds);

			for (const std::string text : { "120001", "-1", "15s", "1.5" })
			{
				CAPTURE(text);
				const std::vector<std::string> arguments = { "--gpu-reset-wait=" + text };
				const Result<CommandLine> commandLine = CommandLine::Parse(arguments, options);
				REQUIRE_MESSAGE(commandLine.has_value(), commandLine.error().ToString());
				ApplicationSpecification specification;
				const Status applied = ApplyEngineCommandLine(*commandLine, specification);
				REQUIRE_FALSE(applied.has_value());
				CHECK(applied.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(applied.error().GetMessageText().contains("--gpu-reset-wait"));
				CHECK(specification.Graphics.ResetWaitMilliseconds == DefaultGpuResetWaitMilliseconds);
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
			const std::vector<std::string> arguments = { "--expect-no-errors" };
			const Result<CommandLine> commandLine = CommandLine::Parse(arguments, GetEngineCommandLineOptions());
			REQUIRE(commandLine.has_value());
			ApplicationSpecification specification;
			REQUIRE(ApplyEngineCommandLine(*commandLine, specification).has_value());
			CHECK(specification.ExpectNoErrors);
		}

		TEST_CASE("Application: --expect-no-errors keeps the exit code of a run without errors")
		{
			// Warnings do not count (Docs/Decisions/0012-m7-decisions.md decision 15).
			class WarningLoggingApplication final : public Application
			{
			public:
				using Application::Application;
			protected:
				void OnUpdate(const FrameTime& /*frame*/) override
				{
					ENGINE_WARN("a warning the run logs on purpose");
				}
			};
			ApplicationSpecification specification = MakeHeadlessSpecification(2);
			specification.ExpectNoErrors = true;
			WarningLoggingApplication application(std::move(specification));
			CHECK(application.Run() == ExitCode::Success);
		}

		TEST_CASE("Application: --expect-no-errors fails a run that logged an error")
		{
			// Docs/Decisions/0012-m7-decisions.md decision 15: every logger counts, from the start of Run.
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

		TEST_CASE("Application: --expect-no-errors counts the errors logged while the context tears down")
		{
			// The check runs after the whole context is gone (its file system, job system and event log), not before.
			class TeardownErrorApplication final : public Application
			{
			public:
				using Application::Application;
			protected:
				Status OnInitialize() override
				{
					return GetContext().GetVfs().Mount("teardown", CreateScope<ErrorAtTeardownMount>());
				}
			};
			ApplicationSpecification specification = MakeHeadlessSpecification(2);
			specification.ExpectNoErrors = true;
			TeardownErrorApplication application(std::move(specification));
			Test::ExpectLog logged(LogLevel::Error, "the test mount logged an error at teardown");
			Test::ExpectLog failed(LogLevel::Error, "--expect-no-errors");
			CHECK(application.Run() == ExitCode::Failed);
		}

		TEST_CASE("Application: windowed runs default to the system audio device and headless runs to none")
		{
			// M12 (Architecture §10.1; Docs/Decisions/0015-m12-decisions.md).
			const AudioEngineSpecification windowed = GetDefaultAudioSpecification(WindowMode::Windowed);
			CHECK(windowed.Device == AudioDeviceKind::System);
			CHECK(windowed.Decoding == AudioDecoding::Threaded);
			const AudioEngineSpecification headless = GetDefaultAudioSpecification(WindowMode::Headless);
			CHECK(headless.Device == AudioDeviceKind::None);
			CHECK(headless.Decoding == AudioDecoding::Deterministic);
			CHECK(windowed.InjectedDeviceCreationFailures == 0);
			CHECK_FALSE(ApplicationSpecification{}.Audio.has_value());
		}

		TEST_CASE("ApplyEngineCommandLine: --audio-device chooses the audio device and keeps the window mode's decoding")
		{
			// M12: windowed Editor and Runtime processes that tests start pass --audio-device none, so no test opens the
			// machine's audio device (§15.1 T1).
			const auto apply = [](const std::vector<std::string>& arguments) -> Result<ApplicationSpecification>
			{
				ENGINE_TRY_ASSIGN(const CommandLine commandLine, CommandLine::Parse(arguments, GetEngineCommandLineOptions()));
				ApplicationSpecification specification;
				ENGINE_TRY(ApplyEngineCommandLine(commandLine, specification));
				return specification;
			};

			const Result<ApplicationSpecification> windowed = apply({ "--audio-device", "none" });
			REQUIRE_MESSAGE(windowed.has_value(), windowed.error().ToString());
			REQUIRE(windowed->Audio.has_value());
			CHECK(windowed->Audio->Device == AudioDeviceKind::None);
			CHECK(windowed->Audio->Decoding == AudioDecoding::Threaded);

			// The option may come before --headless: the decoding mode is still the headless one.
			const Result<ApplicationSpecification> headless = apply({ "--audio-device=null", "--headless" });
			REQUIRE_MESSAGE(headless.has_value(), headless.error().ToString());
			REQUIRE(headless->Audio.has_value());
			CHECK(headless->Audio->Device == AudioDeviceKind::Null);
			CHECK(headless->Audio->Decoding == AudioDecoding::Deterministic);

			const Result<ApplicationSpecification> system = apply({ "--audio-device", "system" });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			REQUIRE(system->Audio.has_value());
			CHECK(system->Audio->Device == AudioDeviceKind::System);

			// Without the option the application keeps its default (GetDefaultAudioSpecification of its window mode).
			const Result<ApplicationSpecification> absent = apply({});
			REQUIRE_MESSAGE(absent.has_value(), absent.error().ToString());
			CHECK_FALSE(absent->Audio.has_value());

			for (const std::string_view spelling : { "System", "wasapi", "" })
			{
				CAPTURE(std::string(spelling));
				const Result<ApplicationSpecification> rejected = apply({ std::format("--audio-device={}", spelling) });
				REQUIRE_FALSE(rejected.has_value());
				CHECK(rejected.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(rejected.error().GetMessageText().contains("--audio-device"));
			}
		}

		TEST_CASE("Application: a headless application's audio engine pulls the frames of every frame's unscaled delta")
		{
			// M12 (§10.1 "Headless runs ... pull frames themselves"; Docs/Decisions/0015-m12-decisions.md decision 14): the
			// application updates its device-less engine after each frame's OnUpdate, so voices advance in headless runs.
			class AudioApplication final : public Application
			{
			public:
				explicit AudioApplication(ApplicationSpecification specification)
					: Application(std::move(specification))
				{
				}
			public:
				double UnscaledSeconds = 0.0;
				uint64_t PulledFrames = 0;
				bool HasEngine = false;
				AudioDeviceState DeviceState = AudioDeviceState::Running;
				AudioTimeSource TimeSource = AudioTimeSource::Device;
			protected:
				void OnUpdate(const FrameTime& frame) override { UnscaledSeconds += frame.UnscaledDeltaTime; }

				void OnShutdown() override
				{
					const AudioEngine* audio = GetContext().GetAudioEngine();
					HasEngine = audio != nullptr;
					if (audio == nullptr)
						return;
					const AudioEngineStats stats = audio->GetStats();
					PulledFrames = stats.PulledFrames;
					DeviceState = stats.DeviceState;
					TimeSource = stats.TimeSource;
				}
			};

			AudioApplication application(MakeHeadlessSpecification(30));
			CHECK(application.Run() == ExitCode::Success);
			REQUIRE(application.HasEngine);
			CHECK(application.DeviceState == AudioDeviceState::None);
			CHECK(application.TimeSource == AudioTimeSource::Host);
			CHECK(application.UnscaledSeconds > 0.0);
			// The engine's accumulator rounds the running total, so the frames match the summed deltas exactly.
			const auto expected = static_cast<uint64_t>(std::floor(application.UnscaledSeconds * 48000.0 + 0.5));
			CHECK(application.PulledFrames == expected);
		}
	}

}
