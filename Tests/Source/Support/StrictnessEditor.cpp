#include "TestsPCH.h"
#include "Support/StrictnessEditor.h"

#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/FileSystem.h"
#include "Support/EditorTestFixture.h"
#include "Support/TestData.h"

namespace Engine {

	namespace Test {

		StrictnessEditor::StrictnessEditor(const std::filesystem::path& directory, bool strict)
			: m_Directory(directory)
		{
			REQUIRE(FileSystem::CreateDirectories(directory / "UserData").has_value());
			Result<Scope<EngineContext>> engine = EngineContext::Create({ .WorkerCount = 0, .UserDataDirectory = directory / "UserData", .RegisterTypes = &RegisterEditorMethodTypes });
			REQUIRE_MESSAGE(engine.has_value(), engine.error().ToString());
			m_Engine = std::move(*engine);
			Result<Scope<EditorContext>> editor = EditorContext::Create(*m_Engine, {
																					   .IdGeneratorState = EditorTestIdState,
																					   .TemplatesDirectory = GetRepositoryRoot() / "Resources" / "Templates" / "Projects",
																					   .ReadOnlyCacheRoot = directory / "ReadOnlyCache",
																					   .HistoryLimits = {},
																					   .EnvironmentBaker = nullptr,
																					   .StrictUnknowns = strict,
																				   });
			REQUIRE_MESSAGE(editor.has_value(), editor.error().ToString());
			m_Editor = std::move(*editor);
		}

		StrictnessEditor::~StrictnessEditor()
		{
			// The editor holds the project lock and the project:// mount: release both before the engine context goes.
			m_Editor.reset();
			m_Engine.reset();
		}

		Status StrictnessEditor::Open(const std::filesystem::path& projectFile)
		{
			ENGINE_TRY_ASSIGN(Scope<LoadedProject> project,
				ProjectManager::OpenProject(projectFile, { .StrictUnknowns = m_Editor->GetSpecification().StrictUnknowns }, m_Editor->GetTypeRegistry()));
			return m_Editor->OpenProject(std::move(project));
		}

		void StrictnessEditor::CreateAndOpenProject(std::string_view name)
		{
			const Result<CreatedProject> created = ProjectManager::CreateProject(
				{
					.Directory = m_Directory / name,
					.Name = std::string(name),
					.Template = ProjectTemplate::Empty,
					.TemplatesDirectory = m_Editor->GetSpecification().TemplatesDirectory,
				},
				m_Engine->GetTypeRegistry());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			const Status opened = Open(created->ProjectFile);
			REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
		}

	}

}
