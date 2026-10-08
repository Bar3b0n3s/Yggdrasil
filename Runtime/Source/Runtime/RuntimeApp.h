#pragma once

#include "Engine/App/Application.h"
#include "Engine/App/CommandLine.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Project/GameManifest.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace Engine {

	class AssetLoaderRegistry;
	class RuntimeAssetManager;

	// The Runtime executable's own options, next to the engine's (GetEngineCommandLineOptions; §13.9), all absent from Dist
	// builds (§13.9: Dist honours only --headless, --frames, --expect-no-errors, --feature-test, --filter and --log-level):
	//   --manifest <path>          read this Game.json (and the paks it names, relative to its directory) instead of the
	//                              one next to the executable: the C++ tests run the built Runtime against test games
	//                              without copying it (Docs/Decisions/0012-m7-decisions.md decision 10)
	//   --screenshot-at <T>:<path> after the frame in which the play session's tick reaches T (T ticks have run), write the
	//                              game view, re-rendered at the window's framebuffer size, as a PNG to <path> (§8.13)
	//   --automation[=<port>]      serve the Runtime subset of automation (RuntimeAutomationServer, §13.5), on <port> or an
	//                              OS-assigned one
	//   --paused                   create the play session paused at tick 0, so an automation client starts from a known
	//                              tick: play.step advances the paused session (the paused-session rule of play.step,
	//                              Docs/Decisions/0012-m7-decisions.md decision 8) and its state hashes match the editor's
	//                              lockstep session of the same game and seed tick for tick; play.resume lets it run
	struct RuntimeOptions
	{
		struct ScreenshotAt
		{
			uint64_t Tick = 0;
			std::filesystem::path Path{};
		};

		// The manifest to read: --manifest, else Game.json next to the executable (Process::GetCurrentExecutablePath).
		std::filesystem::path ManifestPath{};
		std::optional<ScreenshotAt> Screenshot{};
		bool Automation = false;
		uint16_t AutomationPort = 0; // 0: OS-assigned
		bool StartPaused = false;    // --paused
	};

	// The options above, for CommandLine::Parse next to GetEngineCommandLineOptions (empty in Dist). Static storage.
	[[nodiscard]] std::span<const CommandLineOption> GetRuntimeCommandLineOptions();

	// Reads the Runtime options of `commandLine`; `executablePath` is the running executable (the default manifest's
	// directory). Errors: InvalidArgument naming the option for a --manifest that is not a path in valid UTF-8, a
	// --screenshot-at that is not "<tick>:<path>" with a decimal tick, or an --automation port that is not 1 to 65535.
	[[nodiscard]] Result<RuntimeOptions> ParseRuntimeOptions(const CommandLine& commandLine, const std::filesystem::path& executablePath);

	// The runtime application that runs exported games (Architecture §14.3). The factory reads the manifest (GameManifest,
	// Game.json) before anything else and names the application after its Name, so logs, crash reports and user:// live in
	// <UserData>/<Name>/ (§4.4, "test_exported_user_data_folder_uses_manifest_name"); a missing or invalid manifest ends the
	// process with ExitCode::InitFailed (3) and a message naming the file ("test_runtime_missing_manifest_exits_3"). The
	// window comes from the manifest's Window (Title, Width, Height, VSync, Fullscreen, Resizable) and the loop from its
	// Simulation (FixedHz, MaxStepsPerFrame).
	//
	// OnInitialize: the engine context already mounted Data/Engine.pak as engine:// (ApplicationSpecification::EnginePak);
	// the Runtime checks each pak's XXH64 against the manifest (a mismatch is InitFailed naming the pak), mounts Game.pak as
	// project://, builds the RuntimeAssetManager with the built-in loaders over both paks and injects it
	// (EngineContext::SetAssetManager, §3 rule 4), reads the project settings from Game.pak's TOC metadata, loads the start
	// scene (its cooked SceneData) and starts a PlaySession through the same SceneSerializer::FromJson path Play uses
	// (§14.3), seeded with ComputeSessionSeed(Simulation.Seed, the scene's Seed), from the TOC's ProjectSettings
	// (PlaySessionSpecification::Project), paused at tick 0 with --paused. With a device it creates the GpuResourceCache,
	// the SceneRendererPipelines (once), the SceneRenderer of the game view, the BlitPass for the frame target's format and,
	// for --screenshot-at and viewport.screenshot, a ViewportCapture over the same pipelines (a Gpu error there is
	// FatalError(OutOfMemory), §8.14 item 7); with --automation (non-Dist) the RuntimeAutomationServer.
	//
	// The frame: OnEvent queues the window's input events for the session's next tick (PlayInput::QueueDeviceEvent);
	// OnSafePoint pumps the server and applies the session's time scale and stepping flag to the loop
	// (SetFrameTimeScale, SetFrameThrottleSuspended); OnFixedStep and OnUpdate drive the session
	// (PlaySession::AdvanceLoopStep, AdvanceLoopFrame); OnRender renders the session's last snapshot with the SceneRenderer
	// and blits it into the frame target, and after the tick of --screenshot-at writes the screenshot of the session's
	// current view (PlaySession::ExtractView at the window's framebuffer size; a failure is logged at Error level and ends
	// the run with ExitCode::Failed). The run ends at window close, --frames, session.shutdown, or
	// Application.Quit (M13). Escape does nothing by default (§14.3). OnShutdown releases, in order, the server, the
	// capture, the renderer and blit pass, the pipelines, the session, the GPU cache, then removes and destroys the asset
	// manager, while the context still exists.
	class RuntimeApp final : public Application
	{
	public:
		RuntimeApp(ApplicationSpecification specification, RuntimeOptions options, GameManifest manifest);
		~RuntimeApp() override;
	private:
		[[nodiscard]] Status OnInitialize() override;
		void OnShutdown() override;
		void OnEvent(Event& event) override;
		void OnSafePoint() override;
		void OnFixedStep(const SimStep& step) override;
		void OnUpdate(const FrameTime& frame) override;
		void OnRender(RenderContext& context) override;
	private:
		// The options, the manifest, the session, the renderer objects, the capture and the automation server (RuntimeApp.cpp).
		struct State;
	private:
		// From OnInitialize to OnShutdown; the manager uses the loaders and the context's services, so it goes first.
		Scope<AssetLoaderRegistry> m_Loaders;
		Scope<RuntimeAssetManager> m_Assets;
		Scope<State> m_State;
	};

	// The runtime's ApplicationFactory (§14.3): parses the engine options and GetRuntimeCommandLineOptions, reads the
	// manifest (RuntimeOptions::ManifestPath) and names the application after the manifest's Name; sets the window and the
	// loop from it and EnginePak to the manifest's first pak. Errors: InvalidArgument for a bad command line, including any
	// positional argument (the runtime declares none); NotFound, Io, Parse, Validation or UnsupportedVersion for the
	// manifest (RunApplication maps them to ExitCode::InitFailed, 3).
	[[nodiscard]] Result<Scope<Application>> CreateRuntimeApp(std::span<const std::string> arguments);

}
