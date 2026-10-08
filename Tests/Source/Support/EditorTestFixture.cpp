#include "TestsPCH.h"
#include "Support/EditorTestFixture.h"

#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/TestCaseTracker.h"
#include "Support/TestData.h"

namespace Engine {

	namespace Test {

		namespace Utils {

			// A failed setup step: inside a test case it fails the case (doctest FAIL stops it); outside one (a death-test
			// child, where doctest's assertion macros cannot run) the process cannot continue either.
			[[noreturn]] static void FailEditorFixture(const std::string& message)
			{
				if (GetRunningTestCase().has_value())
					FAIL(message);
				FatalError(FatalErrorKind::InitFailed, message);
			}

			template<typename T>
			static void RequireEditorStep(const Result<T>& result, std::string_view step)
			{
				if (!result)
					FailEditorFixture(std::format("EditorTestFixture: {} failed: {}", step, result.error().ToString()));
			}

		}

		EditorTestFixture::EditorTestFixture(std::string_view label, CommandHistoryLimits historyLimits, RegisterTypesFunction registerTypes,
			std::optional<AudioEngineSpecification> audio)
			: m_Directory(label)
		{
			std::error_code error;
			std::filesystem::create_directories(m_Directory / "UserData", error);
			if (error)
				Utils::FailEditorFixture(std::format("EditorTestFixture: creating the user-data directory failed: {}", error.message()));

			Result<Scope<EngineContext>> engine = EngineContext::Create({
				.WorkerCount = 0,
				.UserDataDirectory = m_Directory / "UserData",
				.RegisterTypes = registerTypes != nullptr ? registerTypes : &RegisterEditorMethodTypes,
				.Audio = audio,
			});
			Utils::RequireEditorStep(engine, "creating the engine context");
			m_Engine = std::move(*engine);

			Result<Scope<EditorContext>> editor = EditorContext::Create(*m_Engine, {
																					   .IdGeneratorState = EditorTestIdState,
																					   .TemplatesDirectory = GetRepositoryRoot() / "Resources" / "Templates" / "Projects",
																					   .ReadOnlyCacheRoot = m_Directory / "ReadOnlyCache",
																					   .HistoryLimits = historyLimits,
																				   });
			Utils::RequireEditorStep(editor, "creating the editor context");
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
			Utils::RequireEditorStep(created, "creating the project");

			Result<Scope<LoadedProject>> project = ProjectManager::OpenProject(created->ProjectFile, {}, m_Engine->GetTypeRegistry());
			Utils::RequireEditorStep(project, "opening the project");
			Utils::RequireEditorStep(m_Editor->OpenProject(std::move(*project)), "opening the project in the editor");
		}

		void EditorTestFixture::CreateAndOpenScene(std::string_view path)
		{
			if (!m_Editor->HasProject())
				Utils::FailEditorFixture("EditorTestFixture::CreateAndOpenScene needs an open project");
			const Result<VfsPath> scenePath = VfsPath::Create("project", path);
			Utils::RequireEditorStep(scenePath, "parsing the scene path");

			Scope<Scene> scene = m_Editor->CreateScene(std::string(scenePath->GetStem()));
			const Result<std::string> text = SceneSerializer::SaveToString(*scene);
			Utils::RequireEditorStep(text, "serializing the scene");
			Utils::RequireEditorStep(m_Editor->WriteProjectFile(*scenePath, std::as_bytes(std::span(text->data(), text->size()))), "writing the scene");
			m_Editor->SetScene(std::move(scene), *scenePath);
		}

	}

}
