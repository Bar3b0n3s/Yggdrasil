#include "TestsPCH.h"

#include "EditorCore/Project/ProjectManager.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Platform/Process.h"
#include "Engine/Project/ProjectSerializer.h"
#include "Support/EditorTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static ProjectCreateSpecification MakeCreateSpecification(const Test::EditorTestFixture& fixture, std::string_view name)
	{
		return { .Directory = fixture.GetProjectRoot(name),
			.Name = std::string(name),
			.Template = ProjectTemplate::Empty,
			.TemplatesDirectory = Test::GetRepositoryRoot() / "Resources" / "Templates" / "Projects" };
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ProjectManager: a relative create path returns an absolute project file")
		{
			Test::EditorTestFixture fixture("ProjectRelativeCreate");
			auto specification = MakeCreateSpecification(fixture, "RelativeGame");
			std::error_code error;
			const auto workingDirectory = std::filesystem::current_path(error);
			REQUIRE_FALSE(error);
			const auto relative = specification.Directory.lexically_relative(workingDirectory);
			// Windows permits the per-test temp directory on another drive, which has no relative spelling.
			if (!relative.empty())
				specification.Directory = relative;
			const auto created = ProjectManager::CreateProject(specification, fixture.GetEngine().GetTypeRegistry());
			REQUIRE(created);
			CHECK(created->ProjectFile.is_absolute());
			CHECK(created->ProjectFile == fixture.GetProjectRoot("RelativeGame") / "RelativeGame.eproj");
			CHECK(std::filesystem::current_path(error) == workingDirectory);
			CHECK_FALSE(error);
		}

		TEST_CASE("ProjectManager: the Empty template creates the folder skeleton and a canonical .eproj")
		{
			Test::EditorTestFixture fixture("ProjectCreate");
			const TypeRegistry& registry = fixture.GetEngine().GetTypeRegistry();
			const Result<CreatedProject> created = ProjectManager::CreateProject(MakeCreateSpecification(fixture, "Tetris"), registry);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			const std::filesystem::path root = fixture.GetProjectRoot("Tetris");
			CHECK(created->ProjectFile == root / "Tetris.eproj");

			for (const std::string_view folder : { "Scenes", "Scripts", "Prefabs", "Materials", "Models", "Textures", "Audio", "Fonts", "Tests" })
			{
				std::error_code error;
				INFO(std::string(folder));
				CHECK(std::filesystem::is_directory(root / "Assets" / folder, error));
			}
			for (const std::string_view file : { ".luaurc", ".gitignore", "AGENTS.md" })
			{
				INFO(std::string(file));
				CHECK(FileSystem::Exists(root / file));
			}
			const Result<std::string> gitignore = FileSystem::ReadText(root / ".gitignore");
			REQUIRE(gitignore.has_value());
			CHECK(gitignore->contains("Library/"));
			const Result<std::string> agents = FileSystem::ReadText(root / "AGENTS.md");
			REQUIRE(agents.has_value());
			CHECK(agents->contains("Tetris"));
			CHECK_FALSE(agents->contains("{{Name}}"));

			// The .eproj is the canonical document of default settings named after the project.
			const Result<std::string> eproj = FileSystem::ReadText(created->ProjectFile);
			REQUIRE(eproj.has_value());
			ProjectLoadReport report;
			const Result<ProjectSettings> settings = ProjectSerializer::LoadFromString(*eproj, registry, {}, report);
			REQUIRE(settings.has_value());
			CHECK(settings->Name == "Tetris");
			const Result<std::string> canonical = ProjectSerializer::SaveToString(*settings, registry);
			REQUIRE(canonical.has_value());
			CHECK(*canonical == *eproj);

			REQUIRE(created->RecordedFiles.size() == 1);
			CHECK(created->RecordedFiles[0].Path == "Tetris.eproj");
		}

		TEST_CASE("ProjectManager: creating into a non-empty directory is AlreadyExists and an invalid name Validation")
		{
			Test::EditorTestFixture fixture("ProjectCreateErrors");
			const TypeRegistry& registry = fixture.GetEngine().GetTypeRegistry();
			REQUIRE(FileSystem::CreateDirectories(fixture.GetProjectRoot("Busy")).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(fixture.GetProjectRoot("Busy") / "keep.txt", std::as_bytes(std::span("x", 1))).has_value());
			const Result<CreatedProject> busy = ProjectManager::CreateProject(MakeCreateSpecification(fixture, "Busy"), registry);
			REQUIRE_FALSE(busy.has_value());
			CHECK(busy.error().GetCode() == ErrorCode::AlreadyExists);

			ProjectCreateSpecification badName = MakeCreateSpecification(fixture, "Fine");
			badName.Name = "CON";
			const Result<CreatedProject> invalid = ProjectManager::CreateProject(badName, registry);
			REQUIRE_FALSE(invalid.has_value());
			CHECK(invalid.error().GetCode() == ErrorCode::Validation);
			CHECK_FALSE(FileSystem::Exists(fixture.GetProjectRoot("Fine")));
		}

		TEST_CASE("ProjectManager: FindProjectFile accepts a .eproj or a directory with exactly one")
		{
			Test::EditorTestFixture fixture("ProjectFind");
			const TypeRegistry& registry = fixture.GetEngine().GetTypeRegistry();
			REQUIRE(ProjectManager::CreateProject(MakeCreateSpecification(fixture, "Game"), registry).has_value());
			const std::filesystem::path root = fixture.GetProjectRoot("Game");
			CHECK(ProjectManager::FindProjectFile(root) == root / "Game.eproj");
			CHECK(ProjectManager::FindProjectFile(root / "Game.eproj") == root / "Game.eproj");
			CHECK(ProjectManager::FindProjectFile(root / "Missing.eproj").error().GetCode() == ErrorCode::NotFound);
			CHECK(ProjectManager::FindProjectFile(root / "AGENTS.md").error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE(FileSystem::WriteFileAtomic(root / "Second.eproj", std::as_bytes(std::span("{}", 2))).has_value());
			const Result<std::filesystem::path> ambiguous = ProjectManager::FindProjectFile(root);
			REQUIRE_FALSE(ambiguous.has_value());
			CHECK(ambiguous.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(ambiguous.error().GetMessageText().contains("Second.eproj"));
		}

		TEST_CASE("ProjectManager: opening takes the lock and a second open fails naming the pid")
		{
			Test::EditorTestFixture fixture("ProjectLock");
			const TypeRegistry& registry = fixture.GetEngine().GetTypeRegistry();
			REQUIRE(ProjectManager::CreateProject(MakeCreateSpecification(fixture, "Locked"), registry).has_value());
			Result<Scope<LoadedProject>> first = ProjectManager::OpenProject(fixture.GetProjectRoot("Locked"), {}, registry);
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			CHECK_FALSE((*first)->IsReadOnly());
			CHECK((*first)->GetRoot() == fixture.GetProjectRoot("Locked"));
			CHECK((*first)->GetCacheDirectory() == fixture.GetProjectRoot("Locked") / "Library" / "Cache");
			CHECK(ProjectLock::ReadHolderPid(fixture.GetProjectRoot("Locked") / "Library" / "Editor.lock") == Process::GetCurrentId());

			const Result<Scope<LoadedProject>> second = ProjectManager::OpenProject(fixture.GetProjectRoot("Locked"), {}, registry);
			REQUIRE_FALSE(second.has_value());
			CHECK(second.error().GetCode() == ErrorCode::AlreadyExists);
			CHECK(second.error().GetMessageText().contains(std::to_string(Process::GetCurrentId())));

			first->reset();
			CHECK(ProjectManager::OpenProject(fixture.GetProjectRoot("Locked"), {}, registry).has_value());
		}

		TEST_CASE("ProjectManager: a read-only open takes no lock, writes nothing and uses the private cache")
		{
			Test::EditorTestFixture fixture("ProjectReadOnly");
			const TypeRegistry& registry = fixture.GetEngine().GetTypeRegistry();
			REQUIRE(ProjectManager::CreateProject(MakeCreateSpecification(fixture, "Shared"), registry).has_value());
			Result<Scope<LoadedProject>> writer = ProjectManager::OpenProject(fixture.GetProjectRoot("Shared"), {}, registry);
			REQUIRE(writer.has_value());
			const ProjectOpenOptions readOnly{ .ReadOnly = true, .StrictUnknowns = false, .ReadOnlyCacheDirectory = fixture.GetDirectory() / "Private" };
			Result<Scope<LoadedProject>> reader = ProjectManager::OpenProject(fixture.GetProjectRoot("Shared"), readOnly, registry);
			REQUIRE_MESSAGE(reader.has_value(), reader.error().ToString());
			CHECK((*reader)->IsReadOnly());
			CHECK((*reader)->GetCacheDirectory() == fixture.GetDirectory() / "Private");
		}

		TEST_CASE("ProjectManager: a read-only project's private cache starts empty and is removed when the project closes")
		{
			Test::EditorTestFixture fixture("ProjectReadOnlyCache");
			const TypeRegistry& registry = fixture.GetEngine().GetTypeRegistry();
			REQUIRE(ProjectManager::CreateProject(MakeCreateSpecification(fixture, "Shared"), registry).has_value());
			// The leftover of a read-only editor that crashed with the process id this one has now.
			const std::filesystem::path cache = fixture.GetDirectory() / "ReadOnlyCache" / "4242";
			REQUIRE(FileSystem::CreateDirectories(cache / "Stale").has_value());
			REQUIRE(FileSystem::WriteFileAtomic(cache / "Stale" / "Thumbnail.bin", std::as_bytes(std::span("old", 3))).has_value());

			const ProjectOpenOptions readOnly{ .ReadOnly = true, .StrictUnknowns = false, .ReadOnlyCacheDirectory = cache };
			{
				Result<Scope<LoadedProject>> reader = ProjectManager::OpenProject(fixture.GetProjectRoot("Shared"), readOnly, registry);
				REQUIRE_MESSAGE(reader.has_value(), reader.error().ToString());
				const Result<std::vector<std::filesystem::path>> entries = FileSystem::ListDirectory(cache);
				REQUIRE(entries.has_value());
				CHECK(entries->empty());
			}
			CHECK_FALSE(FileSystem::Exists(cache));

			// A writable project's cache is the project's own Library/Cache, which stays.
			{
				Result<Scope<LoadedProject>> writer = ProjectManager::OpenProject(fixture.GetProjectRoot("Shared"), {}, registry);
				REQUIRE(writer.has_value());
			}
			CHECK(FileSystem::Exists(fixture.GetProjectRoot("Shared") / "Library" / "Cache"));
		}

		TEST_CASE("ProjectManager: a malformed .eproj fails and releases the lock")
		{
			Test::EditorTestFixture fixture("ProjectMalformed");
			const TypeRegistry& registry = fixture.GetEngine().GetTypeRegistry();
			REQUIRE(ProjectManager::CreateProject(MakeCreateSpecification(fixture, "Broken"), registry).has_value());
			const std::filesystem::path file = fixture.GetProjectRoot("Broken") / "Broken.eproj";
			REQUIRE(FileSystem::WriteFileAtomic(file, std::as_bytes(std::span("{\"Format\":", 10))).has_value());
			const Result<Scope<LoadedProject>> opened = ProjectManager::OpenProject(file, {}, registry);
			REQUIRE_FALSE(opened.has_value());
			CHECK(opened.error().GetCode() == ErrorCode::Parse);
			CHECK(ProjectLock::IsHeld(fixture.GetProjectRoot("Broken") / "Library" / "Editor.lock") == false);
		}

		TEST_CASE("ProjectManager: the recent list keeps ten projects, most recent first")
		{
			Test::EditorTestFixture fixture("ProjectRecent");
			VirtualFileSystem& vfs = fixture.GetEngine().GetVfs();
			const Result<std::vector<std::filesystem::path>> empty = ProjectManager::ReadRecentProjects(vfs);
			REQUIRE(empty.has_value());
			CHECK(empty->empty());
			for (int index = 0; index < 12; ++index)
				REQUIRE(ProjectManager::AddRecentProject(vfs, fixture.GetDirectory() / std::format("P{}/P{}.eproj", index, index)).has_value());
			REQUIRE(ProjectManager::AddRecentProject(vfs, fixture.GetDirectory() / "P5/P5.eproj").has_value());
			const Result<std::vector<std::filesystem::path>> recent = ProjectManager::ReadRecentProjects(vfs);
			REQUIRE(recent.has_value());
			REQUIRE(recent->size() == ProjectManager::MaxRecentProjects);
			CHECK(recent->front() == fixture.GetDirectory() / "P5/P5.eproj");
			CHECK((*recent)[1] == fixture.GetDirectory() / "P11/P11.eproj");
		}

		TEST_CASE("ProjectTemplate: names print and parse case-insensitively")
		{
			CHECK(ProjectTemplateToString(ProjectTemplate::Empty) == "Empty");
			CHECK(ProjectTemplateFromString("empty") == ProjectTemplate::Empty);
			CHECK(ProjectTemplateFromString("EMPTY") == ProjectTemplate::Empty);
			CHECK(ProjectTemplateToString(ProjectTemplate::Basic3D) == "Basic3D");
			CHECK(ProjectTemplateFromString("Basic3D") == ProjectTemplate::Basic3D);
			CHECK(ProjectTemplateFromString("basic3d") == ProjectTemplate::Basic3D);
			CHECK(ProjectTemplateFromString("BASIC3D") == ProjectTemplate::Basic3D);
			CHECK_FALSE(ProjectTemplateFromString("UnknownTemplate").has_value());
		}

		TEST_CASE("ProjectManager: a missing template is NotFound and creates nothing")
		{
			Test::EditorTestFixture fixture("ProjectMissingTemplate");
			ProjectCreateSpecification specification = MakeCreateSpecification(fixture, "Orphan");
			specification.TemplatesDirectory = fixture.GetDirectory() / "NoTemplates";
			const Result<CreatedProject> created = ProjectManager::CreateProject(specification, fixture.GetEngine().GetTypeRegistry());
			REQUIRE_FALSE(created.has_value());
			CHECK(created.error().GetCode() == ErrorCode::NotFound);
			CHECK_FALSE(FileSystem::Exists(fixture.GetProjectRoot("Orphan")));
		}

		TEST_CASE("ProjectManager: creating into an existing empty directory keeps it")
		{
			Test::EditorTestFixture fixture("ProjectCreateEmptyDirectory");
			REQUIRE(FileSystem::CreateDirectories(fixture.GetProjectRoot("Ready")).has_value());
			const Result<CreatedProject> created = ProjectManager::CreateProject(MakeCreateSpecification(fixture, "Ready"), fixture.GetEngine().GetTypeRegistry());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			CHECK(FileSystem::Exists(fixture.GetProjectRoot("Ready") / "Ready.eproj"));
		}

		TEST_CASE("ProjectManager: the recent list keeps other preferences and rejects a malformed file")
		{
			Test::EditorTestFixture fixture("ProjectRecentPreferences");
			VirtualFileSystem& vfs = fixture.GetEngine().GetVfs();
			const std::filesystem::path preferences = fixture.GetDirectory() / "UserData" / "Editor.json";
			const std::string_view existing = "{\"Format\": \"EditorPreferences\", \"Version\": 1, \"Theme\": \"Dark\", \"RecentProjects\": []}";
			REQUIRE(FileSystem::WriteFileAtomic(preferences, std::as_bytes(std::span(existing.data(), existing.size()))).has_value());
			REQUIRE(ProjectManager::AddRecentProject(vfs, fixture.GetDirectory() / "A/A.eproj").has_value());
			const Result<std::string> written = FileSystem::ReadText(preferences);
			REQUIRE(written.has_value());
			CHECK(written->contains("\"Theme\": \"Dark\""));
			const Result<std::vector<std::filesystem::path>> recent = ProjectManager::ReadRecentProjects(vfs);
			REQUIRE(recent.has_value());
			REQUIRE(recent->size() == 1);
			CHECK(recent->front() == fixture.GetDirectory() / "A/A.eproj");

			const std::string_view malformed = "{\"Format\": \"EditorPreferences\", \"Version\": 1, \"RecentProjects\": [3]}";
			REQUIRE(FileSystem::WriteFileAtomic(preferences, std::as_bytes(std::span(malformed.data(), malformed.size()))).has_value());
			const Result<std::vector<std::filesystem::path>> rejected = ProjectManager::ReadRecentProjects(vfs);
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::Validation);
			CHECK_FALSE(ProjectManager::AddRecentProject(vfs, fixture.GetDirectory() / "B/B.eproj").has_value());
		}

		TEST_CASE("ProjectManager: without user:// the recent list is empty and cannot be saved")
		{
			VirtualFileSystem vfs;
			const Result<std::vector<std::filesystem::path>> recent = ProjectManager::ReadRecentProjects(vfs);
			REQUIRE(recent.has_value());
			CHECK(recent->empty());
			const Status added = ProjectManager::AddRecentProject(vfs, "Game.eproj");
			REQUIRE_FALSE(added.has_value());
			CHECK(added.error().GetCode() == ErrorCode::InvalidState);
		}
	}

}
