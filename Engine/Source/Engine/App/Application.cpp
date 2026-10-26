#include "EnginePCH.h"
#include "Engine/App/Application.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GpuDiagnostics.h"
#include "Engine/Graphics/GpuOutOfMemory.h"
#include "Engine/Graphics/GpuProfiler.h"
#include "Engine/Graphics/GpuRecreation.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/Graphics/Swapchain.h"
#include "Engine/Platform/ErrorDialog.h"

// Dist builds have no ImGui (§14.4; Docs/Decisions/0024-m15-contract.md decision 11).
#if !defined(ENGINE_DIST)
	#include "Engine/ImGui/ImGuiLayer.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

		constexpr std::string_view HeadlessOption = "--headless";
		constexpr std::string_view FramesOption = "--frames";
		constexpr std::string_view ExpectNoErrorsOption = "--expect-no-errors";
		constexpr std::string_view LogLevelOption = "--log-level";
		// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decision 41): every configuration, Dist included.
		constexpr std::string_view GpuResetWaitOption = "--gpu-reset-wait";
#if !defined(ENGINE_DIST)
		constexpr std::string_view UserDataDirectoryOption = "--user-data-dir";
		constexpr std::string_view RendererOption = "--renderer";
		constexpr std::string_view GpuValidationOption = "--gpu-validation";
		constexpr std::string_view GpuValidationSync = "sync";
		constexpr std::string_view ExpectNoGpuErrorsOption = "--expect-no-gpu-errors";
		constexpr std::string_view VulkanApiOption = "--vulkan-api";
		constexpr std::string_view GpuOption = "--gpu";
		constexpr std::string_view GpuInjectFaultOption = "--gpu-inject-fault";
		constexpr std::string_view AudioDeviceOption = "--audio-device";
#endif

		// Every option of GetEngineCommandLineOptions. Dist honours only the options §13.9 lists, so --user-data-dir is
		// an unknown option there.
		constexpr std::array EngineCommandLineOptions = {
			CommandLineOption{
				.Name = HeadlessOption,
				.Description = "Run without a display on GLFW's null platform, one fixed step per frame (ManualClock).",
			},
			CommandLineOption{
				.Name = FramesOption,
				.Value = CommandLineValue::Required,
				.ValueName = "N",
				.Description = "Exit with code 0 after N frames (N >= 1).",
			},
			CommandLineOption{
				.Name = ExpectNoErrorsOption,
				.Description = "Exit with code 1 when any error was logged during the run.",
			},
			CommandLineOption{
				.Name = LogLevelOption,
				.Value = CommandLineValue::Required,
				.ValueName = "trace|info|warn|error|critical",
				.Description = "Write only entries of this level and above to the console and the log file (--expect-no-errors "
							   "still counts every error).",
			},
			CommandLineOption{
				.Name = GpuResetWaitOption,
				.Value = CommandLineValue::Required,
				.ValueName = "milliseconds",
				.Description = "Wait this long (0 to 120000, default 15000) for the driver or the operating system to reset a hung "
							   "GPU before the hang is fatal, and before retrying the device after a device loss.",
			},
#if !defined(ENGINE_DIST)
			CommandLineOption{
				.Name = UserDataDirectoryOption,
				.Value = CommandLineValue::Required,
				.ValueName = "path",
				.Description = "Keep logs and crash reports under this absolute directory instead of the user's data folder.",
			},
			CommandLineOption{
				.Name = RendererOption,
				.Value = CommandLineValue::Required,
				.ValueName = "vulkan|none",
				.Description = "Render with Vulkan (default), or not at all (logic only, no GPU needed).",
			},
			CommandLineOption{
				.Name = GpuValidationOption,
				.Value = CommandLineValue::Optional,
				.ValueName = "sync",
				.Description = "Enable the Vulkan validation layer and NVRHI validation (on by default in Debug); =sync adds "
							   "synchronization validation.",
			},
			CommandLineOption{
				.Name = ExpectNoGpuErrorsOption,
				.Description = "Exit with code 1 when the GPU device reported any validation, NVRHI or resource-leak error or "
							   "warning during the run, its teardown included.",
			},
			CommandLineOption{
				.Name = VulkanApiOption,
				.Value = CommandLineValue::Required,
				.ValueName = "1.3|1.4",
				.Description = "Request at most this Vulkan API version (1.3 forces the fallbacks of the 1.4 paths).",
			},
			CommandLineOption{
				.Name = GpuOption,
				.Value = CommandLineValue::Required,
				.ValueName = "index|name",
				.Description = "Use the GPU with this index or a name containing this text (overrides ENGINE_GPU).",
			},
			CommandLineOption{
				.Name = GpuInjectFaultOption,
				.Value = CommandLineValue::Required,
				.ValueName = "device-lost|oom-texture|hang|oom-specialization|device-lost-unrecoverable|hang-reset|"
							 "hang-reset-persistent|oom-rendertarget|oom-rendertarget-persistent|oom-pipeline|oom-pipeline-persistent|"
							 "oom-swapchain|oom-swapchain-persistent",
				.Description = "Force a GPU fault path for tests: device loss, texture out-of-memory, a GPU hang, "
							   "shader-specialization out-of-memory, a device loss whose recovery fails, a hang the driver resets "
							   "(once, or every time), or render-target, pipeline or swapchain out-of-memory after startup (once, "
							   "or every time).",
			},
			CommandLineOption{
				.Name = AudioDeviceOption,
				.Value = CommandLineValue::Required,
				.ValueName = "system|null|none",
				.Description = "Play through the system's default audio device (the windowed default), miniaudio's Null "
							   "backend, or no device (the headless default; tests never open a real device).",
			},
#endif
		};

#if !defined(ENGINE_DIST)
		// The --audio-device spellings.
		[[nodiscard]] static std::optional<AudioDeviceKind> AudioDeviceKindFromCommandLine(std::string_view text)
		{
			if (text == "system")
				return AudioDeviceKind::System;
			if (text == "null")
				return AudioDeviceKind::Null;
			if (text == "none")
				return AudioDeviceKind::None;
			return std::nullopt;
		}
#endif

		// The --log-level spellings. Trace logging is compiled out of Dist (Log.h), where "trace" stores Info, the lowest
		// level such a build logs.
		[[nodiscard]] static std::optional<LogLevel> LogLevelFromCommandLine(std::string_view text)
		{
			if (text == "trace")
				return Detail::LogTraceEnabled ? LogLevel::Trace : LogLevel::Info;
			if (text == "info")
				return LogLevel::Info;
			if (text == "warn")
				return LogLevel::Warn;
			if (text == "error")
				return LogLevel::Error;
			if (text == "critical")
				return LogLevel::Critical;
			return std::nullopt;
		}

		// The startup rule of §8.14 item 7 for the frame's rendering objects: a Gpu error creating one means the device is
		// out of memory and ends the process with FatalError(OutOfMemory) (exit code 4); any other error is returned with
		// `context`, and Run reports it as InitFailed. M18 (Docs/Decisions/0029-m18-contract.md decision 28): on a lost device
		// a Gpu error is the loss, never out-of-memory, so it is returned, and Run ends a loss before the first frame through
		// EndDeviceLossBeforeFirstFrame.
		template<typename T>
		static Result<T> CheckRenderingObject(Result<T> result, const GraphicsDevice& device, std::string_view fatalMessage, std::string context)
		{
			if (!result.has_value() && result.error().GetCode() == ErrorCode::Gpu && !device.IsLost())
				FatalError(FatalErrorKind::OutOfMemory, std::format("{}: {}", fatalMessage, result.error().ToString()));
			return WithContext(std::move(result), std::move(context));
		}

		// A frame target created or resized during the frame loop that fails otherwise than out of memory ends the process:
		// OutOfMemory for a Gpu error (§8.14 item 7), Gpu for anything else. M18 (C18.4, decision 44): the frame's resize is a
		// request (RetryAfterTrim), whose out-of-memory failure skips its frames instead, so a Gpu error no longer reaches
		// here on a healthy device. Never called on a lost device, whose failures are the loss (decision 28): the frame
		// renders nothing, and the frame boundary recovers the device.
		[[noreturn]] static void EndRenderingObjectFailure(const Error& error, std::string_view message)
		{
			const FatalErrorKind kind = error.GetCode() == ErrorCode::Gpu ? FatalErrorKind::OutOfMemory : FatalErrorKind::Gpu;
			FatalError(kind, std::format("{}: {}", message, error.ToString()));
		}

		// M18 (decision 25): a device loss before the application's first frame is fatal. The startup objects cannot be
		// created on the lost device and nothing has rendered, so there is nothing to recover to: the process ends through
		// GraphicsDevice::RaiseDeviceLost (the editor's fatal hook, a crash report, exit code 4). Does nothing while the
		// device is not lost, or without a device.
		static void EndDeviceLossBeforeFirstFrame(EngineContext& context)
		{
			GraphicsDevice* device = context.GetGraphicsDevice();
			if (device == nullptr || !device->IsLost())
				return;
			const std::optional<GpuDeviceLossReport> report = device->GetLossReport();
			device->RaiseDeviceLost(std::format("{}, before the application's first frame", report.has_value() ? report->Context : "the startup"));
		}

		// M18 (decision 33): the frame index a recovery's new FramePacer continues from: the first multiple of the frames in
		// flight at or after `nextFrameIndex`, so the frames' indices keep growing across the recovery (renderers keep their
		// GPU timing history, which ignores an older frame) and RenderContext::FrameSlot stays FrameIndex modulo the frames
		// in flight.
		[[nodiscard]] static uint64_t GetRecoveredFrameIndexOffset(uint64_t nextFrameIndex, uint64_t framesInFlight)
		{
			ENGINE_CORE_ASSERT(framesInFlight >= 1, "a frame has at least one frame in flight");
			return (nextFrameIndex + framesInFlight - 1) / framesInFlight * framesInFlight;
		}

		// The rule of the --expect-no-gpu-errors check: any error or warning in the device's whole life fails the run.
		[[nodiscard]] static bool HasGpuMessages(const GpuMessageCounts& counts)
		{
			return counts.Errors > 0 || counts.Warnings > 0;
		}

	}

	namespace {

		// ApplicationSpecification::MinimumLogLevel for one Run: sets the output sinks' minimum level when the run has one,
		// and restores the previous level when the run ends.
		class LogOutputLevelScope
		{
		public:
			explicit LogOutputLevelScope(std::optional<LogLevel> level)
				: m_Previous(Log::GetOutputLevel()), m_IsActive(level.has_value())
			{
				if (level.has_value())
					Log::SetOutputLevel(*level);
			}

			~LogOutputLevelScope()
			{
				if (m_IsActive)
					Log::SetOutputLevel(m_Previous);
			}

			LogOutputLevelScope(const LogOutputLevelScope&) = delete;
			LogOutputLevelScope& operator=(const LogOutputLevelScope&) = delete;
		private:
			LogLevel m_Previous = LogLevel::Trace;
			bool m_IsActive = false;
		};

	}

	// The frame's rendering objects with a device (Application.h, step 1), from InitializeRendering to ShutdownRendering.
	struct Application::FrameRendering
	{
		Scope<Swapchain> WindowSwapchain;      // windowed processes
		Scope<OffscreenTarget> OffscreenFrame; // headless processes (§8.13)
		Scope<FramePacer> Pacer;
		Scope<GpuProfiler> Profiler;
		nvrhi::CommandListHandle CommandList{};
		// Headless: per frame slot, the frame target (its framebuffer, which references its textures) of the slot's last
		// submission, dropped when the slot's next frame begins, after FramePacer::BeginFrame has waited for that
		// submission. NVRHI's command lists do not reference textures used only by clears (ADR 0009 decision 31), so
		// without this a resize could destroy a target that a frame still in flight clears. The swapchain needs none: its
		// recreation waits for idle.
		std::vector<nvrhi::FramebufferHandle> SlotTargets;
		// The frame clock's time since the last UI frame (ImGuiLayer::BeginFrame), accumulated over skipped frames.
		double UiDeltaSeconds = 0.0;
		bool UiFrameRequested = false;
		uint32_t LastWidth = 1;
		uint32_t LastHeight = 1;
		bool IsImGuiFailing = false; // the last UI frame failed to render, so the next failure is not logged again
		// M18 (C18.3, decision 29): CommandList is open, from its open() to its close() in OnFrameRender. A device loss that
		// the frame-boundary catch reported ends the frame in between, and OnFrameDeviceLost closes and executes the list on
		// the lost device: an opened NVRHI list that is never executed keeps itself, and what it recorded, alive.
		bool IsRecording = false;
		// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision 33): the frame index is FrameIndexOffset plus the pacer's;
		// a recovery creates a new pacer, which counts from 0 again, and moves the offset past NextFrameIndex (the index after
		// the last frame that began), so the frames' indices keep growing (Utils::GetRecoveredFrameIndexOffset).
		uint64_t FrameIndexOffset = 0;
		uint64_t NextFrameIndex = 0;
		// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decision 44): the size at which the offscreen frame target (headless,
		// or the forced UI target) ran out of memory after its trim and retry (0x0: none). Frames at that size render nothing,
		// without a creation or a trim, until the size changes or a frame sees the window minimized, as the swapchain skips the
		// size its recreation failed at (headless equals windowed).
		uint32_t FailedWidth = 0;
		uint32_t FailedHeight = 0;
		// The "Application frame" owner of the device's recreation registry: its Release destroys the objects above, its
		// Recreate creates them again (Application::ReleaseFrameObjects, CreateFrameObjects). Registered by
		// InitializeRendering once they exist, before the ImGui layer's renderer.
		GpuRecreationRegistration Recreation;
	};

	// --expect-no-errors (Application.h, step 3): counts every Error and Critical entry of every logger while it lives. The
	// listener runs on whichever thread logs (jobs included), so the count is atomic; RemoveListener in the destructor
	// returns only once the listener can no longer run.
	struct Application::LoggedErrorCounter
	{
		LoggedErrorCounter()
		{
			ListenerId = Log::AddListener([this](const LogEntry& entry)
			{
				if (entry.Level == LogLevel::Error || entry.Level == LogLevel::Critical)
					Count.fetch_add(1, std::memory_order_relaxed);
			});
		}

		~LoggedErrorCounter()
		{
			Log::RemoveListener(ListenerId);
		}

		LoggedErrorCounter(const LoggedErrorCounter&) = delete;
		LoggedErrorCounter& operator=(const LoggedErrorCounter&) = delete;

		std::atomic<uint64_t> Count{ 0 };
		uint64_t ListenerId = 0;
	};

	AudioEngineSpecification GetDefaultAudioSpecification(WindowMode window)
	{
		if (window == WindowMode::Windowed)
			return AudioEngineSpecification{ .Device = AudioDeviceKind::System, .Decoding = AudioDecoding::Threaded };
		return AudioEngineSpecification{ .Device = AudioDeviceKind::None, .Decoding = AudioDecoding::Deterministic };
	}

	std::span<const CommandLineOption> GetEngineCommandLineOptions()
	{
		return Utils::EngineCommandLineOptions;
	}

	Status ApplyEngineCommandLine(const CommandLine& commandLine, ApplicationSpecification& specification)
	{
		if (commandLine.Has(Utils::HeadlessOption))
		{
			specification.Window = WindowMode::Headless;
			specification.Clock = ClockKind::Manual; // every headless run uses ManualClock (§13.9)
		}

		ENGINE_TRY_ASSIGN(const std::optional<uint64_t> frames, commandLine.GetUnsigned(Utils::FramesOption));
		if (frames.has_value())
		{
			if (*frames == 0)
				return MakeError(ErrorCode::InvalidArgument, "option '{}' needs a number of frames >= 1, got 0", Utils::FramesOption);
			specification.MaxFrames = *frames;
		}

#if !defined(ENGINE_DIST)
		if (const std::optional<std::string_view> directory = commandLine.GetValue(Utils::UserDataDirectoryOption))
		{
			// std::filesystem::path throws on ill-formed UTF-8 on Windows, and the option is external input.
			if (!IsValidUtf8(*directory))
				return MakeError(ErrorCode::InvalidArgument, "option '{}' needs a path in UTF-8", Utils::UserDataDirectoryOption);
			std::filesystem::path root(std::u8string_view(reinterpret_cast<const char8_t*>(directory->data()), directory->size()));
			if (!root.is_absolute())
			{
				return MakeError(ErrorCode::InvalidArgument, "option '{}' needs an absolute directory, got '{}'",
					Utils::UserDataDirectoryOption, *directory);
			}
			specification.UserDataRoot = std::move(root);
		}

		if (const std::optional<std::string_view> renderer = commandLine.GetValue(Utils::RendererOption))
		{
			const std::optional<RendererMode> mode = RendererModeFromCommandLine(*renderer);
			if (!mode.has_value())
				return MakeError(ErrorCode::InvalidArgument, "option '{}' takes 'vulkan' or 'none', got '{}'", Utils::RendererOption, *renderer);
			specification.Renderer = *mode;
		}

		if (commandLine.Has(Utils::GpuValidationOption))
		{
			const std::optional<std::string_view> validation = commandLine.GetValue(Utils::GpuValidationOption);
			if (validation.has_value() && *validation != Utils::GpuValidationSync)
			{
				return MakeError(ErrorCode::InvalidArgument, "option '{}' takes no value or '{}', got '{}'", Utils::GpuValidationOption,
					Utils::GpuValidationSync, *validation);
			}
			specification.Graphics.Validation = true;
			specification.Graphics.SynchronizationValidation = validation.has_value();
		}

		if (commandLine.Has(Utils::ExpectNoGpuErrorsOption))
			specification.ExpectNoGpuErrors = true;

		if (const std::optional<std::string_view> api = commandLine.GetValue(Utils::VulkanApiOption))
		{
			const std::optional<VulkanApiVersion> version = VulkanApiVersionFromString(*api);
			if (!version.has_value())
				return MakeError(ErrorCode::InvalidArgument, "option '{}' takes '1.3' or '1.4', got '{}'", Utils::VulkanApiOption, *api);
			specification.Graphics.MaxApiVersion = *version;
		}

		// CommandLine::Parse never yields an empty value for a Required option.
		if (const std::optional<std::string_view> gpu = commandLine.GetValue(Utils::GpuOption))
			specification.Graphics.GpuOverride = std::string(*gpu);

		if (const std::optional<std::string_view> fault = commandLine.GetValue(Utils::GpuInjectFaultOption))
		{
			const std::optional<GpuFault> injected = GpuFaultFromString(*fault);
			if (!injected.has_value() || *injected == GpuFault::None)
			{
				return MakeError(ErrorCode::InvalidArgument,
					"option '{}' takes 'device-lost', 'oom-texture', 'hang', 'oom-specialization', 'device-lost-unrecoverable', "
					"'hang-reset', 'hang-reset-persistent', 'oom-rendertarget', 'oom-rendertarget-persistent', 'oom-pipeline', "
					"'oom-pipeline-persistent', 'oom-swapchain' or 'oom-swapchain-persistent', got '{}'",
					Utils::GpuInjectFaultOption, *fault);
			}
			specification.Graphics.InjectFault = *injected;
		}

		// After --headless, so the decoding mode is the final window mode's.
		if (const std::optional<std::string_view> device = commandLine.GetValue(Utils::AudioDeviceOption))
		{
			const std::optional<AudioDeviceKind> kind = Utils::AudioDeviceKindFromCommandLine(*device);
			if (!kind.has_value())
			{
				return MakeError(ErrorCode::InvalidArgument, "option '{}' takes 'system', 'null' or 'none', got '{}'",
					Utils::AudioDeviceOption, *device);
			}
			AudioEngineSpecification audio = specification.Audio.value_or(GetDefaultAudioSpecification(specification.Window));
			audio.Device = *kind;
			specification.Audio = audio;
		}
#endif

		if (commandLine.Has(Utils::ExpectNoErrorsOption))
			specification.ExpectNoErrors = true;

		if (const std::optional<std::string_view> level = commandLine.GetValue(Utils::LogLevelOption))
		{
			const std::optional<LogLevel> minimum = Utils::LogLevelFromCommandLine(*level);
			if (!minimum.has_value())
			{
				return MakeError(ErrorCode::InvalidArgument, "option '{}' takes 'trace', 'info', 'warn', 'error' or 'critical', got '{}'",
					Utils::LogLevelOption, *level);
			}
			specification.MinimumLogLevel = *minimum;
		}

		// M18 (C18.4, decision 41): the machine's GPU reset window, in every configuration.
		ENGINE_TRY_ASSIGN(const std::optional<uint64_t> resetWait, commandLine.GetUnsigned(Utils::GpuResetWaitOption, MaxGpuResetWaitMilliseconds));
		if (resetWait.has_value())
			specification.Graphics.ResetWaitMilliseconds = static_cast<uint32_t>(*resetWait);

		specification.Args = commandLine;
		return {};
	}

	Application::Application(ApplicationSpecification specification)
		: m_Specification(std::move(specification))
	{
	}

	Application::~Application() = default;

	int Application::Run()
	{
		ENGINE_CORE_VERIFY(!m_HasRun, "Application::Run may be called once per Application");
		m_HasRun = true;

		ProcessContext* process = ProcessContext::GetCurrent();
		ENGINE_CORE_VERIFY(process != nullptr, "Application::Run needs a live ProcessContext");
		ENGINE_CORE_ASSERT(process->GetSpecification().Window == m_Specification.Window,
			"the application is {} but the process is {}: a process is windowed or headless, never both",
			WindowModeToString(m_Specification.Window), WindowModeToString(process->GetSpecification().Window));

		// --log-level (M15, §13.9) from here to the end of Run, the teardown and the --expect-no-errors line included.
		const LogOutputLevelScope outputLevel(m_Specification.MinimumLogLevel);

		if (m_Specification.Clock == ClockKind::Scripted)
		{
			ENGINE_CORE_ERROR("Cannot run the application '{}': the Scripted clock needs a delta table and is chosen per "
							  "FeatureTest suite, never through ApplicationSpecification::Clock",
				m_Specification.Name);
			return ExitCode::InitFailed;
		}

		// --expect-no-errors counts from here: an error logged while the context, the rendering objects or the application
		// initialize fails the run too.
		if (m_Specification.ExpectNoErrors)
			m_LoggedErrors = CreateScope<LoggedErrorCounter>();

		// 1. Per-context initialization (§4.1 level 2), then the application's own.
		WindowSpecification window = m_Specification.WindowSettings;
		if (window.Title.empty())
			window.Title = m_Specification.Name;
		const EngineContextSpecification contextSpecification = {
			.WorkerCount = m_Specification.WorkerCount.value_or(JobSystem::GetDefaultWorkerCount()),
			.UserDataDirectory = process->GetUserDataPaths().Root,
			.EngineResourcesDirectory = m_Specification.EngineResourcesDirectory,
			.EngineCacheDirectory = m_Specification.EngineCacheDirectory,
			.Window = std::move(window),
			.RegisterTypes = m_Specification.RegisterTypes,
			.Graphics = m_Specification.Renderer == RendererMode::Vulkan ? std::optional<GraphicsSpecification>(m_Specification.Graphics)
																		 : std::nullopt,
			.EnginePak = m_Specification.EnginePak,
			.Audio = m_Specification.Audio.value_or(GetDefaultAudioSpecification(m_Specification.Window)),
		};
		Result<Scope<EngineContext>> context = EngineContext::Create(contextSpecification);
		if (!context.has_value())
		{
			ReportInitializationFailure(*process, context.error());
			return ExitCode::InitFailed;
		}
		m_Context = std::move(*context);

		// A device lost before the first frame ends the process (decision 25), whether or not the step failed because of it.
		const Status rendering = InitializeRendering();
		Utils::EndDeviceLossBeforeFirstFrame(*m_Context);
		if (!rendering.has_value())
		{
			ReportInitializationFailure(*process, rendering.error());
			ShutdownRendering();
			m_Context.reset();
			return ExitCode::InitFailed;
		}

		const Status initialized = OnInitialize();
		Utils::EndDeviceLossBeforeFirstFrame(*m_Context);
		if (!initialized.has_value())
		{
			ReportInitializationFailure(*process, initialized.error());
			m_PendingExitCode.reset();
			ShutdownRendering();
			m_Context.reset();
			return ExitCode::InitFailed;
		}

		// 2. The frame loop. An exit requested during OnInitialize makes it run no frame.
		const FrameLoopSpecification loopSpecification = {
			.Loop = m_Specification.Loop,
			.MaxFrames = m_Specification.MaxFrames,
			.ThrottleToFixedHz = m_Specification.Window == WindowMode::Headless && m_Specification.ThrottleHeadless,
		};
		IFrameLoopClient& client = *this; // the base is private, so convert here rather than inside CreateScope
		m_FrameLoop = CreateScope<FrameLoop>(*m_Context, client, CreateClock(), loopSpecification);
		if (m_PendingExitCode.has_value())
			m_FrameLoop->RequestExit(*m_PendingExitCode);
		int exitCode = m_FrameLoop->Run();
		m_FrameLoop.reset();

		// 3. Shutdown in reverse order. The GPU services go before the rest of the context, so a run that expects no GPU
		// messages fails when its device reported any error or warning in its whole life, its teardown included (leaks, the
		// validation layer's reports at vkDestroyDevice, §15.3); an earlier failure keeps its code.
		OnShutdown();
		m_PendingExitCode.reset();
		ShutdownRendering();
		const bool hadDevice = m_Context->GetGraphicsDevice() != nullptr;
		const GpuMessageCounts gpuMessages = m_Context->DestroyGraphics();
		if (m_Specification.ExpectNoGpuErrors && hadDevice && exitCode == ExitCode::Success && Utils::HasGpuMessages(gpuMessages))
		{
			ENGINE_CORE_ERROR("The GPU device reported {} error(s) and {} warning(s) during the run (--expect-no-gpu-errors)",
				gpuMessages.Errors, gpuMessages.Warnings);
			exitCode = ExitCode::Failed;
		}
		// The rest of the context goes before --expect-no-errors decides, so an error logged while it tears down (the job
		// system, the mounts, the event log) fails the run too; the counter is a process-level log listener.
		m_Context.reset();
		exitCode = ApplyExpectNoErrors(exitCode);
		m_LoggedErrors.reset();
		return exitCode;
	}

	void Application::RequestExit(int exitCode)
	{
		if (m_FrameLoop != nullptr)
			m_FrameLoop->RequestExit(exitCode);
		else if (!m_PendingExitCode.has_value())
			m_PendingExitCode = exitCode;
	}

	EngineContext& Application::GetContext()
	{
		ENGINE_CORE_ASSERT(m_Context != nullptr, "Application::GetContext outside initialization and shutdown");
		return *m_Context;
	}

	ProcessContext& Application::GetProcessContext()
	{
		ENGINE_CORE_ASSERT(m_Context != nullptr, "Application::GetProcessContext outside initialization and shutdown");
		ProcessContext* process = ProcessContext::GetCurrent();
		ENGINE_CORE_VERIFY(process != nullptr, "Application::GetProcessContext without a ProcessContext");
		return *process;
	}

	void Application::OnRenderSubmitted(uint64_t, uint64_t)
	{
	}

	Status Application::RequestOffscreenUiFrame()
	{
#if defined(ENGINE_DIST)
		return MakeError(ErrorCode::Unsupported, "a fresh UI frame requires an ImGui layer, which Dist builds do not have");
#else
		if (m_Rendering == nullptr || m_ImGuiLayer == nullptr)
			return MakeError(ErrorCode::Unsupported, "a fresh UI frame requires an active renderer and ImGui layer");
		m_Rendering->UiFrameRequested = true;
		return {};
#endif
	}

	FrameLoopStatistics Application::GetFrameStatistics() const
	{
		return m_FrameLoop != nullptr ? m_FrameLoop->GetStatistics() : FrameLoopStatistics{};
	}

	void Application::OnFrameEvent(Event& event)
	{
		// Resizes need no call here: the swapchain compares the framebuffer size with the size it was created for when the
		// next frame acquires its image, so the several events of one resize (window size, framebuffer size, restore)
		// cause one recreation, never one per event.
		OnEvent(event);
	}

	void Application::OnFrameSafePoint()
	{
		OnSafePoint();
	}

	void Application::OnFrameFixedStep(const SimStep& step)
	{
		OnFixedStep(step);
	}

	void Application::OnFrameUpdate(const FrameTime& frame)
	{
		OnUpdate(frame);
		// M12 (§10.1): the audio engine's frame, after the frame phase's AudioSystem::Update (inside OnUpdate): device
		// notifications and re-creation, finished voices, and the host pull of a device-less engine. Real time, so unscaled.
		if (AudioEngine* audio = m_Context->GetAudioEngine(); audio != nullptr)
		{
			m_AudioClockSeconds += frame.UnscaledDeltaTime;
			audio->Update(m_AudioClockSeconds, frame.UnscaledDeltaTime);
		}
	}

	void Application::OnFrameRender(const FrameTime& frame)
	{
		// Without a device (RendererMode::None) there is nothing to render.
		if (m_Rendering == nullptr)
			return;
		FrameRendering& rendering = *m_Rendering;
		GraphicsDevice& device = *m_Context->GetGraphicsDevice();
		Window& window = *m_Context->GetWindow();

		// The UI clock runs on the frame clock, also across the frames that render nothing (ImGuiLayer::BeginFrame).
		rendering.UiDeltaSeconds += frame.UnscaledDeltaTime;

		// M18 (decision 28): a lost device renders nothing, as a frame the swapchain skips: no wait, no OnRender or
		// OnImGuiRender, no submission or present. The frame boundary recovers the device (OnFrameDeviceLost).
		if (device.IsLost())
			return;

		// §8.2: wait (bounded) until this frame's slot is free, then pick the target. The slot's previous submission has
		// completed, so the target it held may go (FrameRendering::SlotTargets).
		rendering.Pacer->BeginFrame();
		const uint32_t frameSlot = rendering.Pacer->GetFrameSlot();
		const uint64_t frameIndex = rendering.FrameIndexOffset + rendering.Pacer->GetFrameIndex();
		rendering.NextFrameIndex = frameIndex + 1;
		rendering.Profiler->BeginFrame(frameSlot, frameIndex);
		if (frameSlot < rendering.SlotTargets.size())
			rendering.SlotTargets[frameSlot] = nullptr;

		nvrhi::IFramebuffer* framebuffer = nullptr;
		nvrhi::ITexture* target = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		const bool minimized = window.IsMinimized() || window.GetFramebufferWidth() == 0 || window.GetFramebufferHeight() == 0;
		bool acquiredSwapchain = false;
		if (rendering.WindowSwapchain != nullptr && !(rendering.UiFrameRequested && minimized))
		{
			Swapchain& swapchain = *rendering.WindowSwapchain;
			const Result<SwapchainAcquireStatus> status = swapchain.AcquireNextImage(frameSlot);
			// A failure on a lost device (one the acquire met included) is the loss: the frame is skipped. M18 (C18.4,
			// decision 44): a recreation that ran out of memory after its trim and retry fails this frame alone, and the
			// swapchain skips the frames at that size until it changes; any other failure of the recreation stays fatal, as Gpu
			// (Swapchain.h: it is not out of memory).
			if (!status.has_value() && !device.IsLost())
			{
				if (status.error().GetCode() != ErrorCode::OutOfMemory)
					FatalError(FatalErrorKind::Gpu, std::format("Cannot recreate the swapchain: {}", status.error().ToString()));
				ENGINE_CORE_WARN("Skipping the frames at the window's framebuffer size {}x{} until it changes: {}", window.GetFramebufferWidth(),
					window.GetFramebufferHeight(), status.error());
			}
			if (status.has_value() && *status == SwapchainAcquireStatus::Acquired)
			{
				acquiredSwapchain = true;
				framebuffer = swapchain.GetCurrentFramebuffer();
				target = swapchain.GetCurrentTexture();
				width = swapchain.GetWidth();
				height = swapchain.GetHeight();
			}
		}
		// M18 (C18.4, decision 44): a frame that sees the window minimized, and so renders no target of the window's size,
		// forgets the size that failed, so restoring the window to it retries, as the swapchain does.
		if (minimized && !rendering.UiFrameRequested)
		{
			rendering.FailedWidth = 0;
			rendering.FailedHeight = 0;
		}
		if (framebuffer == nullptr && !device.IsLost() && ((rendering.WindowSwapchain == nullptr && !minimized) || rendering.UiFrameRequested))
		{
			// A forced frame also works before a windowed swapchain has ever acquired an image. Keep its resources
			// per slot, just like headless targets; minimized frames retain the last rendered nonzero extent.
			width = minimized ? rendering.LastWidth : window.GetFramebufferWidth();
			height = minimized ? rendering.LastHeight : window.GetFramebufferHeight();
			// After startup the target's creation and resize are a request (decisions 42 and 44): an out-of-memory creation is
			// trimmed for and retried once, and a second one skips this frame and every later one at this size, without a
			// creation or a trim, until the size changes. Nothing records meanwhile: the frame's list is not open yet.
			const auto isCurrent = [&rendering, width, height]()
			{
				return rendering.OffscreenFrame != nullptr && rendering.OffscreenFrame->GetWidth() == width && rendering.OffscreenFrame->GetHeight() == height;
			};
			if (!isCurrent() && (width != rendering.FailedWidth || height != rendering.FailedHeight))
			{
				const Status prepared = RetryAfterTrim(device, std::format("the frame's {}x{} target", width, height),
					[&rendering, &device, width, height]() -> Status
				{
					if (rendering.OffscreenFrame == nullptr)
					{
						ENGINE_TRY_ASSIGN(OffscreenTarget created, OffscreenTarget::Create(device, {
																									   .Width = width,
																									   .Height = height,
																									   .ColorFormat = nvrhi::Format::RGBA8_UNORM,
																									   .Depth = false,
																									   .DebugName = "ForcedUiFrame",
																								   }));
						rendering.OffscreenFrame = CreateScope<OffscreenTarget>(std::move(created));
						return {};
					}
					// Created aside: a failed resize keeps the previous textures (OffscreenTarget::Resize).
					return rendering.OffscreenFrame->Resize(device, width, height);
				});
				if (prepared.has_value())
				{
					rendering.FailedWidth = 0;
					rendering.FailedHeight = 0;
				}
				else if (!device.IsLost())
				{
					if (prepared.error().GetCode() != ErrorCode::OutOfMemory)
						Utils::EndRenderingObjectFailure(prepared.error(), "Cannot create or resize the frame's offscreen target");
					ENGINE_CORE_WARN("Skipping the frames at the size {}x{} until it changes: {}", width, height, prepared.error());
					rendering.FailedWidth = width;
					rendering.FailedHeight = height;
				}
			}
			if (isCurrent())
			{
				OffscreenTarget& offscreen = *rendering.OffscreenFrame;
				framebuffer = offscreen.GetFramebuffer();
				target = offscreen.GetColorTexture();
				rendering.SlotTargets[frameSlot] = framebuffer;
			}
		}

		// A skipped frame (minimized, a recreated swapchain, an acquire that timed out, a target the lost device could not
		// give) records and submits nothing.
		if (framebuffer == nullptr || target == nullptr)
		{
			rendering.Pacer->EndFrame(device.GetLastSubmissionID());
			device.RunGarbageCollection();
			return;
		}
		rendering.LastWidth = width;
		rendering.LastHeight = height;
		// Consume only when recording starts. A request made by a render hook belongs to the following frame.
		rendering.UiFrameRequested = false;

		nvrhi::ICommandList& commandList = *rendering.CommandList;
		commandList.open();
		rendering.IsRecording = true;
		commandList.clearTextureFloat(target, nvrhi::AllSubresources,
			nvrhi::Color(FrameClearColor[0], FrameClearColor[1], FrameClearColor[2], FrameClearColor[3]));

		RenderContext context = {
			.Device = &device,
			.CommandList = &commandList,
			.Framebuffer = framebuffer,
			.Target = target,
			.Width = width,
			.Height = height,
			.FrameSlot = frameSlot,
			.FrameIndex = frameIndex,
			.Profiler = rendering.Profiler.get(),
		};
		OnRender(context);

#if !defined(ENGINE_DIST)
		if (m_ImGuiLayer != nullptr)
		{
			m_ImGuiLayer->BeginFrame(rendering.UiDeltaSeconds, frameSlot, minimized);
			rendering.UiDeltaSeconds = 0.0;
			OnImGuiRender();
			m_ImGuiLayer->EndFrame();

			GpuProfileScope scope(*rendering.Profiler, commandList, "ImGui");
			const Status rendered = m_ImGuiLayer->Render(commandList, *framebuffer);
			// A Gpu error means that a pipeline (created per target format on first use, ADR 0009 decision 25), a geometry
			// buffer or a binding set could not be created: the device is out of memory, which ends the process (§8.14
			// item 7). Texture failures never come back here (the renderer logs them and skips their draws). Any other
			// failure is logged once per run of failing frames, because the UI is recorded every frame. On a device lost
			// during the frame, a failure is the loss (decision 28), which the frame boundary recovers: nothing ends or logs.
			const bool isLost = device.IsLost();
			if (!rendered.has_value() && rendered.error().GetCode() == ErrorCode::Gpu && !isLost)
				FatalError(FatalErrorKind::OutOfMemory, std::format("Cannot render the ImGui frame: {}", rendered.error().ToString()));
			if (!rendered.has_value() && !isLost && !rendering.IsImGuiFailing)
				ENGINE_CORE_ERROR("Cannot render the ImGui frame: {}", rendered.error());
			if (!isLost)
				rendering.IsImGuiFailing = !rendered.has_value();
		}
#endif
		commandList.close();
		rendering.IsRecording = false;

		// On a device lost during the frame the submission reaches no queue (GraphicsDevice::ExecuteCommandLists) and the
		// present presents nothing; the hooks still pair, and the frame boundary recovers the device.
		if (acquiredSwapchain)
			rendering.WindowSwapchain->QueueFrameSemaphores();
		const uint64_t submission = device.ExecuteCommandList(commandList);
		OnRenderSubmitted(frameIndex, submission);
		if (acquiredSwapchain)
			rendering.WindowSwapchain->Present();
		rendering.Pacer->EndFrame(device.GetLastSubmissionID());
		device.RunGarbageCollection();
	}

	void Application::OnFrameDeviceLost()
	{
		GraphicsDevice* device = m_Context->GetGraphicsDevice();
		if (device == nullptr || !device->IsLost())
			return;

		// A loss whose flag alone was set (NVRHI's "Device Removed!" outside a submission, a test hook) is reported here, so
		// OnDeviceLost and the event name where it surfaced. The recovery forgets the report, so it is kept first.
		if (!device->GetLossReport().has_value())
			device->ReportDeviceLost("the frame boundary");
		const std::optional<GpuDeviceLossReport> lost = device->GetLossReport();
		ENGINE_CORE_VERIFY(lost.has_value(), "GraphicsDevice::ReportDeviceLost recorded no report of the lost device");
		const GpuDeviceLossReport report = *lost;

		// A loss that the frame-boundary catch reported ended the frame while it recorded: the frame's list is closed and
		// executed on the lost device, which runs nothing (GraphicsDevice::ExecuteCommandLists) but lets NVRHI retire the
		// list with the lost device's submissions in the recovery. Dropped open, the list would keep itself and everything it
		// recorded alive past the old device.
		if (m_Rendering != nullptr && m_Rendering->IsRecording)
		{
			m_Rendering->CommandList->close();
			m_Rendering->IsRecording = false;
			device->ExecuteCommandList(*m_Rendering->CommandList);
		}

		// The autosave first (the editor writes it, CPU only), then the recovery: every registered owner releases its GPU
		// objects, the device is created again and the owners create theirs (decision 29). The play session is not touched.
		OnDeviceLost(report);
		const Status recovered = m_Context->RecoverGraphics();
		if (!recovered.has_value())
		{
			// A device creation that failed again after its retry, a second loss, an owner's failed creation or an object an
			// unregistered owner kept: the process ends (the editor's fatal hook writes the autosave again, decision 25).
			device->RaiseDeviceLost(std::format("{}; the recovery failed: {}", report.Context, recovered.error().ToString()));
		}

		m_Context->GetEventLog().Append(EngineEvent{
			.Seq = 0,
			.Tick = std::nullopt,
			.Type = EngineEventType::GpuDeviceLost,
			.Id = UUID(),
			.Path = {},
			.Name = {},
			.Message = std::format("{}: device lost; {}", report.Context, report.FaultDescription),
			.Dirty = false,
		});
		OnDeviceRecovered(report);
	}

	Status Application::CreateFrameObjects()
	{
		FrameRendering& rendering = *m_Rendering;
		GraphicsDevice& device = *m_Context->GetGraphicsDevice();
		Window& window = *m_Context->GetWindow();
		const uint32_t framesInFlight = device.GetFramesInFlight();
		if (m_Specification.Window == WindowMode::Windowed)
		{
			// A minimized window gets the swapchain's surface now and its images when it is restored (Swapchain::Create).
			ENGINE_TRY_ASSIGN(rendering.WindowSwapchain,
				WithContext(Swapchain::Create(device, window,
								{
									.VSync = device.GetSpecification().VSync,
									.FramesInFlight = framesInFlight,
								}),
					"while creating the swapchain"));
		}
		else
		{
			// Headless (§8.13): the frames land in an offscreen target of the window's framebuffer size.
			const OffscreenTargetSpecification specification = {
				.Width = std::max(window.GetFramebufferWidth(), 1u),
				.Height = std::max(window.GetFramebufferHeight(), 1u),
				.ColorFormat = nvrhi::Format::RGBA8_UNORM,
				.Depth = false,
				.ClearColor = nvrhi::Color(FrameClearColor[0], FrameClearColor[1], FrameClearColor[2], FrameClearColor[3]),
				.DebugName = "FrameTarget",
			};
			ENGINE_TRY_ASSIGN(OffscreenTarget offscreen, WithContext(OffscreenTarget::Create(device, specification), "while creating the frame's offscreen target"));
			rendering.OffscreenFrame = CreateScope<OffscreenTarget>(std::move(offscreen));
		}
		ENGINE_TRY_ASSIGN(rendering.CommandList, WithContext(device.CreateCommandList(), "while creating the frame's command list"));
		rendering.Pacer = CreateScope<FramePacer>(device, framesInFlight);
		rendering.Profiler = CreateScope<GpuProfiler>(device, framesInFlight);
		return {};
	}

	void Application::ReleaseFrameObjects()
	{
		FrameRendering& rendering = *m_Rendering;
#if !defined(ENGINE_DIST)
		// A device loss that the frame-boundary catch reported ends its frame where it was thrown, possibly inside
		// OnImGuiRender with the UI frame begun (decision 26; ADR 0029 risk 10): the next frame begins a new one.
		if (m_ImGuiLayer != nullptr)
			m_ImGuiLayer->EndInterruptedFrame();
#endif
		// What the frames recorded for submissions that will never complete goes with the pacer and the slot targets; the
		// next pacer's frames continue the frame indices.
		rendering.FrameIndexOffset = Utils::GetRecoveredFrameIndexOffset(rendering.NextFrameIndex, m_Context->GetGraphicsDevice()->GetFramesInFlight());
		rendering.NextFrameIndex = rendering.FrameIndexOffset;
		rendering.Profiler.reset();
		rendering.CommandList = nullptr;
		rendering.IsRecording = false;
		rendering.WindowSwapchain.reset();
		std::ranges::fill(rendering.SlotTargets, nullptr);
		rendering.OffscreenFrame.reset();
		rendering.Pacer.reset();
	}

	Status Application::InitializeRendering()
	{
		// RendererMode::None renders nothing and needs none of the rendering objects.
		GraphicsDevice* device = m_Context->GetGraphicsDevice();
		if (device == nullptr)
			return {};
		Window* window = m_Context->GetWindow();
		ENGINE_CORE_VERIFY(window != nullptr, "the application's context has no window");
		const uint32_t framesInFlight = device->GetFramesInFlight();

		m_Rendering = CreateScope<FrameRendering>();
		FrameRendering& rendering = *m_Rendering;
		rendering.LastWidth = std::max(window->GetFramebufferWidth(), 1u);
		rendering.LastHeight = std::max(window->GetFramebufferHeight(), 1u);
		rendering.SlotTargets.resize(framesInFlight);
		// §8.14 item 7: a Gpu error creating one of them at startup is the device out of memory.
		const Status created = CreateFrameObjects();
		if (!created.has_value() && created.error().GetCode() == ErrorCode::Gpu && !device->IsLost())
			FatalError(FatalErrorKind::OutOfMemory, std::format("Cannot create the frame's rendering objects: {}", created.error().ToString()));
		ENGINE_TRY(created);
		// The frame's objects are one owner of the device's GPU objects (decision 33), registered before the ImGui layer's
		// renderer, which so is released before them and created again after them.
		rendering.Recreation = device->GetRecreation().Register({
			.Name = "Application frame",
			.Release = [this]()
		{
			ReleaseFrameObjects();
		},
			.Recreate = [this]()
		{
			return CreateFrameObjects();
		},
		});

#if !defined(ENGINE_DIST)
		if (m_Specification.EnableImGui)
		{
			// imgui.ini lives under the user-data folder (user://) while no project is open (§8.11).
			std::filesystem::path iniFilePath;
			if (!m_Specification.ImGuiIniPath.empty())
			{
				ENGINE_CORE_ASSERT(IsValidUtf8(m_Specification.ImGuiIniPath), "ApplicationSpecification::ImGuiIniPath must be UTF-8");
				const std::string& iniPath = m_Specification.ImGuiIniPath;
				iniFilePath = GetProcessContext().GetUserDataPaths().Root
					/ std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(iniPath.data()), iniPath.size()));
			}
			ENGINE_TRY_ASSIGN(m_ImGuiLayer,
				Utils::CheckRenderingObject(ImGuiLayer::Create(*window, *device, *m_Context->GetPipelineFactory(),
												{
													.IniFilePath = std::move(iniFilePath),
													.FramesInFlight = framesInFlight,
												}),
					*device, "Cannot create the ImGui layer", "while creating the ImGui layer"));
		}
#endif
		return {};
	}

	void Application::ShutdownRendering()
	{
#if defined(ENGINE_DIST)
		if (m_Rendering == nullptr)
			return;
#else
		if (m_Rendering == nullptr && m_ImGuiLayer == nullptr)
			return;
#endif
		// §8.14 item 4: nothing the rendering objects own may still be in use by the GPU when they go.
		if (GraphicsDevice* device = m_Context->GetGraphicsDevice())
			device->WaitForIdle();
#if !defined(ENGINE_DIST)
		m_ImGuiLayer.reset();
#endif
		if (m_Rendering != nullptr)
		{
			m_Rendering->Recreation.Reset();
			m_Rendering->Profiler.reset();
			m_Rendering->CommandList = nullptr;
			m_Rendering->WindowSwapchain.reset();
			m_Rendering->SlotTargets.clear();
			m_Rendering->OffscreenFrame.reset();
			m_Rendering->Pacer.reset();
			m_Rendering.reset();
		}
	}

	void Application::ReportInitializationFailure(const ProcessContext& process, const Error& error) const
	{
		ENGINE_CORE_ERROR("Cannot initialize the application '{}': {}", m_Specification.Name, error);
		if (process.GetSpecification().ShowErrorDialogs && m_Specification.Window == WindowMode::Windowed)
			ShowErrorDialog(m_Specification.Name, std::format("Cannot start: {}", error.ToString()));
	}

	Scope<Clock> Application::CreateClock() const
	{
		if (m_Specification.Clock == ClockKind::Manual)
			return CreateScope<ManualClock>(m_Specification.Loop.GetFixedDelta());
		return CreateScope<SystemClock>();
	}

	void Application::SetFrameTimeScale(double timeScale)
	{
		ENGINE_CORE_ASSERT(m_FrameLoop != nullptr, "Application::SetFrameTimeScale needs the running frame loop");
		m_FrameLoop->SetTimeScale(timeScale);
	}

	void Application::SetFrameThrottleSuspended(bool suspended)
	{
		ENGINE_CORE_ASSERT(m_FrameLoop != nullptr, "Application::SetFrameThrottleSuspended needs the running frame loop");
		m_FrameLoop->SetThrottleSuspended(suspended);
	}

	void Application::SetFrameLoopConfig(const FrameLoopConfig& config)
	{
		ENGINE_CORE_ASSERT(m_FrameLoop != nullptr, "Application::SetFrameLoopConfig needs the running frame loop");
		m_FrameLoop->SetLoopConfig(config);
	}

	int Application::ApplyExpectNoErrors(int exitCode) const
	{
		if (m_LoggedErrors == nullptr || exitCode != ExitCode::Success)
			return exitCode;
		const uint64_t errors = m_LoggedErrors->Count.load(std::memory_order_relaxed);
		if (errors == 0)
			return exitCode;
		ENGINE_CORE_ERROR("{} error(s) were logged during the run (--expect-no-errors)", errors);
		return ExitCode::Failed;
	}

}
