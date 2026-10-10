#pragma once

#include "EditorCore/EditorContext.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/Base.h"
#include "Support/TempDirectory.h"

#include <filesystem>
#include <optional>
#include <string_view>

// Shared setup for EditorCore tests (Roadmap M4): an engine context without a window whose type registry holds the editor's
// automation types, an EditorContext over it with a fixed id generator state, and a temporary directory for projects. A
// failed setup step fails the running test case, or, in a death-test child (no test case), ends the process with
// FatalError, so the fixture also serves death tests.

namespace Engine {

	namespace Test {

		// The fixed generator state of every fixture, so entity ids are reproducible across runs.
		inline constexpr Random::State EditorTestIdState{ 0x9e3779b97f4a7c15ull, 0xbf58476d1ce4e5b9ull, 0x94d049bb133111ebull, 1 };

		// One editor of one test. Not copyable or movable (the editor points at the engine context).
		class EditorTestFixture
		{
		public:
			// An engine context (inline jobs, user:// in <temp>/UserData, `registerTypes` or, when null,
			// RegisterEditorMethodTypes) and an editor in the launcher state with `historyLimits`. `label` names the temporary
			// directory. A test that needs types of its own (a component with an EntityRef field) passes a function that calls
			// RegisterEditorMethodTypes and adds them. `audio` gives the engine context an AudioEngine (M12; tests pass a device-less,
			// deterministic one), so the editor has an audio preview and its play sessions and audio.stats have audio; without it
			// they have none. `engineResources` mounts the repository's read-only resources plus a per-test cooked cache,
			// for tests using shipped templates or file-backed built-ins.
			explicit EditorTestFixture(std::string_view label = "Editor", CommandHistoryLimits historyLimits = {},
				RegisterTypesFunction registerTypes = nullptr, std::optional<AudioEngineSpecification> audio = std::nullopt, bool engineResources = false);
			~EditorTestFixture();

			EditorTestFixture(const EditorTestFixture&) = delete;
			EditorTestFixture& operator=(const EditorTestFixture&) = delete;

			[[nodiscard]] EngineContext& GetEngine() { return *m_Engine; }
			[[nodiscard]] EditorContext& GetEditor() { return *m_Editor; }
			[[nodiscard]] const TempDirectory& GetDirectory() const { return m_Directory; }

			// <temp>/<name>: where CreateAndOpenProject creates a project of that name.
			[[nodiscard]] std::filesystem::path GetProjectRoot(std::string_view name = "TestProject") const;

			// Creates a project from the Empty template (ProjectManager::CreateProject with the repository's templates) and
			// opens it (EditorContext::OpenProject); fails the test case on error.
			void CreateAndOpenProject(std::string_view name = "TestProject");

			// Creates an empty scene, writes it to `path` (project-relative) through EditorContext::WriteProjectFile and makes it
			// the open scene; fails the test case on error. Requires an open project.
			void CreateAndOpenScene(std::string_view path = "Assets/Scenes/Main.scene");
		private:
			TempDirectory m_Directory;
			Scope<EngineContext> m_Engine;
			Scope<EditorContext> m_Editor;
		};

	}

}
