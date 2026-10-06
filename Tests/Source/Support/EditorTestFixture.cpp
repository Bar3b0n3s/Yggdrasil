#include "TestsPCH.h"
#include "Support/EditorTestFixture.h"

#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/TestData.h"

namespace Engine {

	namespace Test {

		EditorTestFixture::EditorTestFixture(std::string_view label, CommandHistoryLimits historyLimits)
			: m_Directory(label)
		{
			std::error_code error;
			std::filesystem::create_directories(m_Directory / "UserData", error);
			REQUIRE_MESSAGE(!error, error.message());

			Result<Scope<EngineContext>> engine = EngineContext::Create({
				.WorkerCount = 0,
				.UserDataDirectory = m_Directory / "UserData",
				.RegisterTypes = &RegisterEditorMethodTypes,
			});
			REQUIRE_MESSAGE(engine.has_value(), engine.error().ToString());
			m_Engine = std::move(*engine);

			Result<Scope<EditorContext>> editor = EditorContext::Create(*m_Engine, {
																					   .IdGeneratorState = EditorTestIdState,
																					   .TemplatesDirectory = GetRepositoryRoot() / "Resources" / "Templates" / "Projects",
																					   .ReadOnlyCacheRoot = m_Directory / "ReadOnlyCache",
																					   .HistoryLimits = historyLimits,
																				   });
			REQUIRE_MESSAGE(editor.has_value(), editor.error().ToString());
			m_Editor = std::move(*editor);
		}

		EditorTestFixture::~EditorTestFixture()
		{
			// The editor holds the project lock and the project:// mount: release both before the directory goes.
			m_Editor.reset();
			m_Engine.reset();
		}

		std::filesystem::path EditorTestFixture::GetProjectRoot(std::string_view name) const
		{
			return m_Directory / name;
		}

		void EditorTestFixture::CreateAndOpenProject(std::string_view name)
		{
			const Result<CreatedProject> created = ProjectManager::CreateProject(
				{
					.Directory = GetProjectRoot(name),
					.Name = std::string(name),
					.Template = ProjectTemplate::Empty,
					.TemplatesDirectory = m_Editor->GetSpecification().TemplatesDirectory,
				},
				m_Engine->GetTypeRegistry());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());

			Result<Scope<LoadedProject>> project = ProjectManager::OpenProject(created->ProjectFile, {}, m_Engine->GetTypeRegistry());
			REQUIRE_MESSAGE(project.has_value(), project.error().ToString());
			const Status opened = m_Editor->OpenProject(std::move(*project));
			REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
		}

		void EditorTestFixture::CreateAndOpenScene(std::string_view path)
		{
			REQUIRE(m_Editor->HasProject());
			Result<VfsPath> scenePath = VfsPath::Create("project", path);
			REQUIRE_MESSAGE(scenePath.has_value(), scenePath.error().ToString());

			Scope<Scene> scene = m_Editor->CreateScene(std::string(scenePath->GetStem()));
			const Result<std::string> text = SceneSerializer::SaveToString(*scene);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			const Status written = m_Editor->WriteProjectFile(*scenePath, std::as_bytes(std::span(text->data(), text->size())));
			REQUIRE_MESSAGE(written.has_value(), written.error().ToString());
			m_Editor->SetScene(std::move(scene), *scenePath);
		}

	}

}
