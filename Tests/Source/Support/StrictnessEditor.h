#pragma once

#include "EditorCore/EditorContext.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <filesystem>
#include <string_view>

// An editor with or without --strict, for the tests of what a strict editor refuses (Architecture §6 "Strict reader",
// EditorContextSpecification::StrictUnknowns); EditorTestFixture's editor is never strict.

namespace Engine {

	namespace Test {

		// An editor of its own engine context (inline jobs, user:// in <directory>/UserData, RegisterEditorMethodTypes), with or
		// without --strict, for the projects of `directory`, which the test owns (a TempDirectory). A failed setup step fails
		// the running test case. Not copyable or movable (the editor points at the engine context).
		class StrictnessEditor
		{
		public:
			StrictnessEditor(const std::filesystem::path& directory, bool strict);
			~StrictnessEditor();

			StrictnessEditor(const StrictnessEditor&) = delete;
			StrictnessEditor& operator=(const StrictnessEditor&) = delete;

			[[nodiscard]] EditorContext& GetEditor() { return *m_Editor; }

			// ProjectManager::OpenProject with the editor's strictness, as every opener passes it, then OpenProject.
			[[nodiscard]] Status Open(const std::filesystem::path& projectFile);

			// Creates the project `name` from the Empty template in <directory>/<name> and opens it with Open; fails the test
			// case on error.
			void CreateAndOpenProject(std::string_view name);
		private:
			std::filesystem::path m_Directory;
			Scope<EngineContext> m_Engine;
			Scope<EditorContext> m_Editor;
		};

	}

}
