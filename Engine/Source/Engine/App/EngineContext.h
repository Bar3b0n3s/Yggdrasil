#pragma once

#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Jobs/MainThreadQueue.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/GraphicsSpecification.h"
#include "Engine/Platform/Input/InputState.h"
#include "Engine/Platform/Window.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

// The per-context services (Architecture §4.1 level 2). Engine code receives the EngineContext, or the one service it
// needs, explicitly; there is no Application::Get() and no global service locator.

namespace Engine {

	class AssetManager;
	class GraphicsDevice;
	class PakReader;
	class PipelineFactory;
	class ShaderLibrary;
	struct GpuMessageCounts;

	// The steps that build an EngineContext, in order (§4.1: VFS -> JobSystem -> Window -> GraphicsDevice -> AssetManager ->
	// AudioEngine -> registries). The AssetManager is not a step: the application builds its manager on the context's
	// services and injects it (SetAssetManager, §3 rule 4). The TypeRegistry is infallible and therefore part of Services
	// (Docs/Decisions/0008-m4-decisions.md decision 2).
	enum class EngineContextStep : uint8_t
	{
		// The infallible services, constructed in member order: VirtualFileSystem, MainThreadQueue, JobSystem, EventLog,
		// InputState, and the TypeRegistry (built-in components, project settings types, the Asset module's types
		// (RegisterAssetTypes), RegisterTypes, then frozen).
		Services,
		// Mounts user:// when UserDataDirectory is set.
		UserData,
		// Mounts engine:// (read-only) at EngineResourcesDirectory and enginecache:// (read-write) at EngineCacheDirectory, each
		// when set (§4.10, §7.5; ADR 0010). Development builds of the editor pass <repo>/Resources and <repo>/bin/EngineCache;
		// exported games mount Engine.pak instead (M7: EnginePak, opened with PakReader, its TOC hash verified, and mounted
		// read-only as engine:// through PakMount; EnginePak and EngineResourcesDirectory exclude each other).
		EngineResources,
		// Window::Create when Window is set.
		Window,
		// When Graphics is set (RendererMode::Vulkan): in development builds the read-only mount of the compiled shaders,
		// ENGINE_SHADER_DIRECTORY, as shaders:// (ShaderLibrary.h); GraphicsDevice::Create, presenting to the window when
		// the process is windowed; the ShaderLibrary and the PipelineFactory. The ShaderLibrary's root is engine://Shaders
		// when EnginePak is set (exported games, in every configuration: the pak holds the target configuration's SPIR-V,
		// §14.1; no shaders:// is mounted then), else shaders://. A Dist context without EnginePak has no shaders.
		Graphics,
		// M12: AudioEngine::Create with the specification's Audio over the context's VFS, when Audio is set (§4.1, §10.1).
		Audio
	};

	// Registers an application's own reflected types into the context's TypeRegistry before it is frozen: the editor's
	// automation param and result structs (Architecture §5.4, §13.4; EditorCore's RegisterEditorMethodTypes). It runs on
	// the constructing thread, only registers types, and must not keep the reference.
	using RegisterTypesFunction = void (*)(TypeRegistry& registry);

	struct EngineContextSpecification
	{
		// JobSystem worker threads; 0 runs jobs inline (deterministic tests, §4.11). Applications pass
		// JobSystem::GetDefaultWorkerCount().
		uint32_t WorkerCount = 0;
		// Mounted as user:// when set (a NativeDirectoryMount without .bak files, ADR 0003 decision 16); the directory must
		// exist. Applications pass ProcessContext's <UserData>/<AppName>.
		std::filesystem::path UserDataDirectory{};
		// engine:// for development builds (§2.2: "Dev builds mount engine:// at <repo>/Resources"): mounted read-only when set;
		// the directory must exist. Empty: not mounted (tests, and exported games, which mount Engine.pak, M7).
		std::filesystem::path EngineResourcesDirectory{};
		// enginecache:// (§4.10, §7.5: bin/EngineCache, dev builds only): mounted read-write, without .bak files, when set; the
		// directory is created when missing. Empty: not mounted.
		std::filesystem::path EngineCacheDirectory{};
		// The context's window, created on the process's GLFW platform (native when windowed, null when headless). Without
		// it the context has no window and needs no GLFW, which lets tests build several contexts side by side.
		std::optional<WindowSpecification> Window{};
		// Called once by the constructor after RegisterBuiltinComponents and RegisterProjectSettingsTypes and before
		// TypeRegistry::Freeze; null adds nothing.
		RegisterTypesFunction RegisterTypes = nullptr;
		// The GPU device's settings (RendererMode::Vulkan); nullopt: no GraphicsDevice (RendererMode::None, §4.1). Needs the
		// process's Vulkan loader (ProcessContext). At most one context of a process may have a device at a time
		// (GraphicsDevice.h).
		std::optional<GraphicsSpecification> Graphics{};
		// M7: an exported game's Engine.pak (a native path), mounted as engine:// by the EngineResources step (see there) and
		// the root of the ShaderLibrary's SPIR-V; must not be set together with EngineResourcesDirectory. Empty: none.
		std::filesystem::path EnginePak{};
		// M12: the context's AudioEngine (§4.1, §10.1): Application passes its ApplicationSpecification::Audio (windowed runs a
		// System device and threaded decoding, headless runs no device and deterministic decoding). nullopt: no AudioEngine
		// (GetAudioEngine returns null; most in-process tests), so play sessions are silent.
		std::optional<AudioEngineSpecification> Audio{};
	};

	// One engine context. Not copyable or movable. Tests may build several side by side in one process, all on that
	// process's GLFW platform (§4.1).
	//
	// Create runs the EngineContextStep steps in order, each returning Status. When a step fails, everything built so far
	// is destroyed in reverse order and Create returns the error with the step as context. Destruction always runs in
	// reverse member order: the GPU services first (pipeline factory, shader library, device), then the window, the
	// JobSystem before the MainThreadQueue its continuations post to (queued jobs are cancelled, running ones finish;
	// ~JobSystem), the VFS last. The AudioEngine (M12), the last member, goes first: its device thread stops before the window,
	// the jobs and the VFS it reads go, and its owners (play sessions, the editor's preview) have released their voices by
	// then.
	//
	// Thread safety: create, use and destroy it on the main thread, which becomes the main thread of the MainThreadQueue
	// and the EventLog. The services document their own rules (VirtualFileSystem, JobSystem and MainThreadQueue::Post are
	// thread-safe; EventLog, InputState and Window are main-thread only).
	class EngineContext
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class EngineContext;
		};

		// Use Create. Constructs the infallible services (the Services step).
		EngineContext(ConstructionKey key, const EngineContextSpecification& specification);
		~EngineContext();

		EngineContext(const EngineContext&) = delete;
		EngineContext& operator=(const EngineContext&) = delete;

		// Builds the context (see the class comment). Errors: those of the failed step, with the step as context: NotFound
		// or Io when UserDataDirectory cannot be mounted; NotFound or Io when EngineResourcesDirectory or EngineCacheDirectory
		// cannot be mounted (context "EngineResources"); for the window, InvalidState without an initialized GLFW (no
		// ProcessContext) and Unsupported when GLFW cannot create it; for graphics, NotFound when the shader directory of a
		// development build does not exist (the Shaders project did not run) and the errors of GraphicsDevice::Create; for
		// audio, the errors of AudioEngine::Create (a missing audio device is not one: the engine then runs device-less with
		// a warning, §10.1).
		[[nodiscard]] static Result<Scope<EngineContext>> Create(const EngineContextSpecification& specification);

		[[nodiscard]] VirtualFileSystem& GetVfs() { return m_Vfs; }
		[[nodiscard]] const VirtualFileSystem& GetVfs() const { return m_Vfs; }
		[[nodiscard]] MainThreadQueue& GetMainThreadQueue() { return m_MainThreadQueue; }
		[[nodiscard]] JobSystem& GetJobSystem() { return m_JobSystem; }
		[[nodiscard]] EventLog& GetEventLog() { return m_EventLog; }
		[[nodiscard]] const EventLog& GetEventLog() const { return m_EventLog; }
		[[nodiscard]] InputState& GetInputState() { return m_InputState; }
		[[nodiscard]] const InputState& GetInputState() const { return m_InputState; }
		// The context's type registry (§4.1, §5.4): every built-in component, the project settings types, the Asset module's
		// types (RegisterAssetTypes) and what the specification's RegisterTypes added, frozen, so every const member is
		// thread-safe.
		[[nodiscard]] const TypeRegistry& GetTypeRegistry() const { return m_TypeRegistry; }

		// The window; nullptr when the specification had none.
		[[nodiscard]] Window* GetWindow() { return m_Window ? &*m_Window : nullptr; }
		[[nodiscard]] const Window* GetWindow() const { return m_Window ? &*m_Window : nullptr; }

		// The opened EnginePak (M7), for the Runtime's RuntimeAssetManager (RuntimeAssetManager::AddPak), so the pak is opened
		// and its TOC verified once; null without EnginePak.
		[[nodiscard]] const Ref<const PakReader>& GetEnginePak() const { return m_EnginePak; }

		// The context's asset manager (§3 rule 4, §4.1 "AssetManager& (injected)"): the application's EditorAssetManager or
		// RuntimeAssetManager, which it owns and builds on this context's services; nullptr until one is injected.
		[[nodiscard]] AssetManager* GetAssetManager() const { return m_AssetManager; }
		// Injects `manager` (a documented back-reference the application owns); nullptr removes it. The application removes it
		// before destroying the manager, and destroys the manager before the context (the manager uses the context's VFS,
		// JobSystem and MainThreadQueue). Main thread.
		void SetAssetManager(AssetManager* manager) { m_AssetManager = manager; }

		// M12: the context's audio engine (§4.1, §10.1); nullptr when the specification had no Audio.
		[[nodiscard]] AudioEngine* GetAudioEngine() { return m_AudioEngine.get(); }
		[[nodiscard]] const AudioEngine* GetAudioEngine() const { return m_AudioEngine.get(); }

		// The GPU services; nullptr without Graphics (RendererMode::None, §4.1).
		[[nodiscard]] GraphicsDevice* GetGraphicsDevice() { return m_GraphicsDevice.get(); }
		[[nodiscard]] ShaderLibrary* GetShaderLibrary() { return m_ShaderLibrary.get(); }
		[[nodiscard]] PipelineFactory* GetPipelineFactory() { return m_PipelineFactory.get(); }

		// Destroys the GPU services now, in the destructor's order (pipeline factory, shader library, device), and returns
		// the device's final message counts (GraphicsDevice::Destroy), which include the messages of the device's own
		// teardown. Zero counts without a device; afterwards the three getters above return nullptr. Application calls it
		// at shutdown, after its rendering objects are gone, so --expect-no-gpu-errors sees the device's whole life (§15.3).
		[[nodiscard]] GpuMessageCounts DestroyGraphics();
	private:
		// The EngineResources step (EngineContextStep::EngineResources).
		[[nodiscard]] Status MountEngineResources(const EngineContextSpecification& specification);
		// The EnginePak part of the EngineResources step (M7): opens the pak, keeps it (GetEnginePak) and mounts it as engine://.
		[[nodiscard]] Status MountEnginePak(const std::filesystem::path& pak);
		// The Graphics step (EngineContextStep::Graphics).
		[[nodiscard]] Status CreateGraphics(const GraphicsSpecification& graphics);
		// The Audio step (EngineContextStep::Audio, M12).
		[[nodiscard]] Status CreateAudio(const AudioEngineSpecification& audio);
	private:
		// Declaration order is construction order; destruction runs in reverse.
		VirtualFileSystem m_Vfs;
		MainThreadQueue m_MainThreadQueue;
		JobSystem m_JobSystem; // posts continuations to m_MainThreadQueue
		EventLog m_EventLog;
		InputState m_InputState;
		TypeRegistry m_TypeRegistry; // frozen by the constructor
		std::optional<Window> m_Window;
		// After the window, so they are destroyed before it (the device may present to it); the factory and the library
		// refer to the device.
		Scope<GraphicsDevice> m_GraphicsDevice;
		Scope<ShaderLibrary> m_ShaderLibrary;
		Scope<PipelineFactory> m_PipelineFactory;
		AssetManager* m_AssetManager = nullptr; // injected, owned by the application (SetAssetManager)
		Ref<const PakReader> m_EnginePak;       // EnginePak (M7), opened by the EngineResources step
		// M12, last, so it is destroyed first (see the class comment); it reads the VFS above.
		Scope<AudioEngine> m_AudioEngine;
	};

	// "Services", "UserData", "EngineResources", "Window", "Graphics" or "Audio".
	[[nodiscard]] std::string_view EngineContextStepToString(EngineContextStep step);

}
