#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Jobs/MainThreadQueue.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VirtualFileSystem.h"
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

	// The steps that build an EngineContext, in order (§4.1: VFS -> JobSystem -> Window -> ...). Later milestones append
	// theirs where §4.1 puts them: the GraphicsDevice after the Window, then the injected AssetManager and the AudioEngine.
	// The TypeRegistry is infallible and therefore part of Services (Docs/Decisions/0008-m4-decisions.md decision 2).
	enum class EngineContextStep : uint8_t
	{
		// The infallible services, constructed in member order: VirtualFileSystem, MainThreadQueue, JobSystem, EventLog,
		// InputState, and the TypeRegistry (built-in components, project settings types, RegisterTypes, then frozen).
		Services,
		// Mounts user:// when UserDataDirectory is set.
		UserData,
		// Window::Create when Window is set.
		Window
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
		// The context's window, created on the process's GLFW platform (native when windowed, null when headless). Without
		// it the context has no window and needs no GLFW, which lets tests build several contexts side by side.
		std::optional<WindowSpecification> Window{};
		// Called once by the constructor after RegisterBuiltinComponents and RegisterProjectSettingsTypes and before
		// TypeRegistry::Freeze; null adds nothing.
		RegisterTypesFunction RegisterTypes = nullptr;
	};

	// One engine context. Not copyable or movable. Tests may build several side by side in one process, all on that
	// process's GLFW platform (§4.1).
	//
	// Create runs the EngineContextStep steps in order, each returning Status. When a step fails, everything built so far
	// is destroyed in reverse order and Create returns the error with the step as context. Destruction always runs in
	// reverse member order: the window first, the JobSystem before the MainThreadQueue its continuations post to (queued
	// jobs are cancelled, running ones finish; ~JobSystem), the VFS last.
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
		// or Io when UserDataDirectory cannot be mounted; for the window, InvalidState without an initialized GLFW (no
		// ProcessContext) and Unsupported when GLFW cannot create it.
		[[nodiscard]] static Result<Scope<EngineContext>> Create(const EngineContextSpecification& specification);

		[[nodiscard]] VirtualFileSystem& GetVfs() { return m_Vfs; }
		[[nodiscard]] const VirtualFileSystem& GetVfs() const { return m_Vfs; }
		[[nodiscard]] MainThreadQueue& GetMainThreadQueue() { return m_MainThreadQueue; }
		[[nodiscard]] JobSystem& GetJobSystem() { return m_JobSystem; }
		[[nodiscard]] EventLog& GetEventLog() { return m_EventLog; }
		[[nodiscard]] const EventLog& GetEventLog() const { return m_EventLog; }
		[[nodiscard]] InputState& GetInputState() { return m_InputState; }
		[[nodiscard]] const InputState& GetInputState() const { return m_InputState; }
		// The context's type registry (§4.1, §5.4): every built-in component, the project settings types and what the
		// specification's RegisterTypes added, frozen, so every const member is thread-safe.
		[[nodiscard]] const TypeRegistry& GetTypeRegistry() const { return m_TypeRegistry; }

		// The window; nullptr when the specification had none.
		[[nodiscard]] Window* GetWindow() { return m_Window ? &*m_Window : nullptr; }
		[[nodiscard]] const Window* GetWindow() const { return m_Window ? &*m_Window : nullptr; }
	private:
		// Declaration order is construction order; destruction runs in reverse.
		VirtualFileSystem m_Vfs;
		MainThreadQueue m_MainThreadQueue;
		JobSystem m_JobSystem; // posts continuations to m_MainThreadQueue
		EventLog m_EventLog;
		InputState m_InputState;
		TypeRegistry m_TypeRegistry; // frozen by the constructor
		std::optional<Window> m_Window;
	};

	// "Services", "UserData" or "Window".
	[[nodiscard]] std::string_view EngineContextStepToString(EngineContextStep step);

}
