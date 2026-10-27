#include "TestsPCH.h"
#include "Editor/ProjectLauncher.h"

#include "Editor/AssetDesignInteraction.h"
#include "Editor/PanelInteractionFixture.h"
#include "Editor/ScriptedFileDialog.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/EditorTestFixture.h"
#include "Support/TestData.h"

#include <atomic>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		// Forwards to the user:// mount it wraps and counts every read, which drawing the launcher must never make.
		class LauncherReadCounter final : public IMount
		{
		public:
			explicit LauncherReadCounter(Scope<IMount> inner)
				: m_Inner(std::move(inner))
			{
			}
			[[nodiscard]] Result<Buffer> ReadFile(const VfsPath& path) const override
			{
				++m_Reads;
				return m_Inner->ReadFile(path);
			}
			[[nodiscard]] Result<Scope<IFileStream>> Open(const VfsPath& path) const override
			{
				++m_Reads;
				return m_Inner->Open(path);
			}
			[[nodiscard]] Status WriteFileAtomic(const VfsPath& path, std::span<const std::byte> data) override { return m_Inner->WriteFileAtomic(path, data); }
			[[nodiscard]] Status WriteNewFile(const VfsPath& path, std::span<const std::byte> data) override { return m_Inner->WriteNewFile(path, data); }
			[[nodiscard]] Result<FileInfo> GetInfo(const VfsPath& path) const override
			{
				++m_Reads;
				return m_Inner->GetInfo(path);
			}
			[[nodiscard]] Result<std::vector<VfsEntry>> List(const VfsPath& directory, bool recursive) const override
			{
				++m_Reads;
				return m_Inner->List(directory, recursive);
			}
			[[nodiscard]] Status CreateDirectories(const VfsPath& directory) override { return m_Inner->CreateDirectories(directory); }
			[[nodiscard]] Status Remove(const VfsPath& path) override { return m_Inner->Remove(path); }
			[[nodiscard]] Status RemoveEmptyDirectory(const VfsPath& directory) override { return m_Inner->RemoveEmptyDirectory(directory); }
			[[nodiscard]] Status Move(const VfsPath& from, const VfsPath& to) override { return m_Inner->Move(from, to); }
			[[nodiscard]] MountAccess GetAccess() const override { return m_Inner->GetAccess(); }
			[[nodiscard]] uint32_t GetReads() const { return m_Reads.load(); }
		private:
			Scope<IMount> m_Inner;
			mutable std::atomic<uint32_t> m_Reads{ 0 };
		};

	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("ProjectLauncher: stale recent paths and matching authored suffixes remain browsable")
		{
			Test::PanelInteractionFixture fixture("LauncherStaleRecent", false);
			auto& context = fixture.GetContext();
			REQUIRE(context.Editor.CloseProject());
			const auto& directory = fixture.GetAutomation().GetEditorFixture().GetDirectory();
			std::vector<std::filesystem::path> projects;
			for (const char* folder : { "A###Shared", "B###Shared" })
			{
				const auto created = ProjectManager::CreateProject({ .Directory = directory / folder, .Name = "Demo", .Template = ProjectTemplate::Empty, .TemplatesDirectory = context.Editor.GetSpecification().TemplatesDirectory }, context.Editor.GetTypeRegistry());
				REQUIRE(created);
				projects.push_back(created->ProjectFile);
				REQUIRE(ProjectManager::AddRecentProject(context.Editor.GetVfs(), created->ProjectFile));
			}
			const auto stale = directory / "Gone" / "Missing" / "Demo.eproj";
			REQUIRE(ProjectManager::AddRecentProject(context.Editor.GetVfs(), stale));
			const auto recent = ProjectManager::ReadRecentProjects(context.Editor.GetVfs());
			REQUIRE(recent);
			REQUIRE_FALSE(recent->empty());
			CHECK(recent->front() == stale);
			ProjectLauncher launcher;
			Test::AssetDesignInteraction ui;
			const auto draw = [&launcher, &context]()
			{
				return launcher.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			auto* root = ui.Window("Panel interaction");
			auto* landing = ui.Child(root, root->GetID("ProjectLanding"));
			SUBCASE("browse walks to an accessible parent and opens a literal folder name")
			{
				const ImGuiID tab = ImHashStr("Open project", 0, landing->GetID("ProjectChoices"));
				ui.Click(draw, landing, tab);
				ui.Click(draw, landing, ImHashStr("Browse projects...", 0, tab));
				auto* picker = ui.Window("###FolderPicker");
				auto* entries = ui.Child(picker, picker->GetID("Entries"));
				const ImGuiID first = ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "A###Shared"));
				const ImGuiID second = ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "B###Shared"));
				const ImRect firstRow = ui.Locate(draw, entries, first);
				const ImRect secondRow = ui.Locate(draw, entries, second);
				CHECK(first != second);
				CHECK(firstRow.GetCenter().y != secondRow.GetCenter().y);
				ui.Click(draw, entries, second);
				const auto unchanged = ProjectManager::ReadRecentProjects(context.Editor.GetVfs());
				REQUIRE(unchanged);
				CHECK(*unchanged == *recent);
				ui.Click(draw, entries, ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "Demo.eproj")));
				ui.Click(draw, picker, "Open file");
			}
			SUBCASE("recent project IDs include the full authored path")
			{
				const ImGuiID tab = ImHashStr("Recent", 0, landing->GetID("ProjectChoices"));
				auto* entries = ui.Child(landing, ImHashStr("RecentProjects", 0, tab));
				const ImGuiID first = ImHashStr("##RecentProject", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, FileSystem::PathToUtf8(projects[0])));
				const ImGuiID second = ImHashStr("##RecentProject", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, FileSystem::PathToUtf8(projects[1])));
				const ImRect firstRow = ui.Locate(draw, entries, first);
				const ImRect secondRow = ui.Locate(draw, entries, second);
				CHECK(first != second);
				CHECK(firstRow.GetCenter().y != secondRow.GetCenter().y);
				ui.Click(draw, entries, second);
			}
			CHECK_FALSE(context.Editor.HasProject());
			context.Actions.Pump();
			REQUIRE(context.Editor.HasProject());
			CHECK(context.Editor.GetProject().GetProjectFile() == projects[1]);
		}

		TEST_CASE("ProjectLauncher: Browse projects shows the injected native dialog and opens the chosen project")
		{
			// M18, E-DIALOGS (Docs/Decisions/0029-m18-contract.md decision 19, stream G): with a native backend in the panel
			// context, the launcher's browser is the native dialog; its answer fills the path, which project.open takes.
			Test::PanelInteractionFixture fixture("LauncherNativeDialog", false);
			auto& context = fixture.GetContext();
			REQUIRE(context.Editor.CloseProject());
			const auto& directory = fixture.GetAutomation().GetEditorFixture().GetDirectory();
			const auto created = ProjectManager::CreateProject({ .Directory = directory / "Native", .Name = "Demo", .Template = ProjectTemplate::Empty, .TemplatesDirectory = context.Editor.GetSpecification().TemplatesDirectory }, context.Editor.GetTypeRegistry());
			REQUIRE(created);
			const auto script = CreateRef<Test::ScriptedFileDialogScript>();
			script->Outcome = FileDialogOutcome{ .State = FileDialogState::Accepted, .Path = created->ProjectFile };
			context.FileDialogs = Test::MakeScriptedFileDialogBackend(script);
			ProjectLauncher launcher;
			Test::AssetDesignInteraction ui;
			const auto draw = [&launcher, &context]()
			{
				return launcher.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			auto* root = ui.Window("Panel interaction");
			auto* landing = ui.Child(root, root->GetID("ProjectLanding"));
			const ImGuiID tab = ImHashStr("Open project", 0, landing->GetID("ProjectChoices"));
			ui.Click(draw, landing, tab);
			ui.Click(draw, landing, ImHashStr("Browse projects...", 0, tab));
			REQUIRE(script->Requests.size() == 1);
			CHECK(script->Requests.front().Kind == FileDialogKind::OpenFile);
			CHECK(script->Requests.front().Extensions == std::vector<std::string>{ ".eproj" });
			for (int frame = 0; frame < 2; ++frame)
				REQUIRE(ui.Frame(draw));
			CHECK(script->LiveSessions == 0);
			context.Actions.Pump();
			REQUIRE(context.Editor.HasProject());
			CHECK(context.Editor.GetProject().GetProjectFile() == created->ProjectFile);
		}

		TEST_CASE("ProjectLauncher: the location's Browse asks for a folder from the typed location made absolute and fills the location")
		{
			// M18, E-DIALOGS (Docs/Decisions/0029-m18-contract.md decision 19): the location's browser is a PickFolder request
			// without extensions or a filter label, starting in the typed location, which a relative location reaches made
			// absolute (the FileDialog API refuses a relative initial folder); the chosen folder becomes the location that
			// project.create takes.
			Test::PanelInteractionFixture fixture("LauncherLocationDialog", false);
			auto& context = fixture.GetContext();
			REQUIRE(context.Editor.CloseProject());
			const auto directory = fixture.GetAutomation().GetEditorFixture().GetDirectory() / "Picked";
			REQUIRE(FileSystem::CreateDirectories(directory).has_value());
			const auto script = CreateRef<Test::ScriptedFileDialogScript>();
			script->Outcome = FileDialogOutcome{ .State = FileDialogState::Accepted, .Path = directory };
			context.FileDialogs = Test::MakeScriptedFileDialogBackend(script);
			ProjectLauncher launcher;
			Test::AssetDesignInteraction ui;
			const auto draw = [&launcher, &context]()
			{
				return launcher.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			auto* root = ui.Window("Panel interaction");
			auto* landing = ui.Child(root, root->GetID("ProjectLanding"));
			const ImGuiID tab = ImHashStr("New project", 0, landing->GetID("ProjectChoices"));
			ui.Click(draw, landing, tab);
			ui.Type(draw, landing, ImHashStr("##ProjectName", 0, tab), "FromDialog");
			// The working directory, typed as a relative location.
			ui.Type(draw, landing, ImHashStr("##ProjectLocation", 0, tab), ".");
			ui.Click(draw, landing, ImHashStr("Browse...", 0, tab));
			REQUIRE(script->Requests.size() == 1);
			const FileDialogRequest& request = script->Requests.front();
			CHECK(request.Kind == FileDialogKind::PickFolder);
			CHECK(request.InitialDirectory.is_absolute());
			std::error_code error;
			const std::filesystem::path workingDirectory = std::filesystem::absolute(".", error);
			REQUIRE_FALSE(error);
			CHECK(request.InitialDirectory == workingDirectory);
			CHECK(request.Extensions.empty());
			CHECK(request.FilterName.empty());
			for (int frame = 0; frame < 2; ++frame)
				REQUIRE(ui.Frame(draw));
			CHECK(script->LiveSessions == 0);
			// The chosen folder is the location: Create project creates the project inside it.
			const int emptyIndex = 1;
			const ImGuiID templateScope = ImHashData(&emptyIndex, sizeof(emptyIndex), ImHashStr("Templates", 0, tab));
			ui.Click(draw, landing, ImHashStr("Empty project", 0, templateScope));
			ui.Click(draw, landing, ImHashStr("Create project", 0, tab));
			context.Actions.Pump();
			REQUIRE(context.Editor.HasProject());
			CHECK(context.Editor.GetProject().GetProjectFile() == directory / "FromDialog" / "FromDialog.eproj");
		}

		TEST_CASE("ProjectLauncher: folder browsers start outside the engine's resources")
		{
			const std::filesystem::path location = GetDefaultProjectLocation();
			REQUIRE_FALSE(location.empty());
			CHECK(location.is_absolute());
			std::error_code error;
			CHECK(std::filesystem::is_directory(location, error));
			CHECK_FALSE(error);
			// Accepting the browser as shown must never create a project inside the engine's tracked resource tree.
			const auto resources = std::filesystem::weakly_canonical(Test::GetRepositoryRoot() / "Resources", error);
			REQUIRE_FALSE(error);
			const auto relative = std::filesystem::weakly_canonical(location, error).lexically_relative(resources);
			REQUIRE_FALSE(error);
			CHECK((relative.empty() || *relative.begin() == ".."));
		}

		TEST_CASE("ProjectLauncher: entering an Empty project queues creation until the safe point")
		{
			Test::PanelInteractionFixture fixture("LauncherCreate", false);
			auto& context = fixture.GetContext();
			REQUIRE(context.Editor.CloseProject());
			const auto directory = fixture.GetAutomation().GetEditorFixture().GetDirectory() / "UiProject";
			ProjectLauncher launcher;
			Test::AssetDesignInteraction ui;
			const auto draw = [&launcher, &context]()
			{
				return launcher.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			auto* root = ui.Window("Panel interaction");
			auto* landing = ui.Child(root, root->GetID("ProjectLanding"));
			const ImGuiID tab = ImHashStr("New project", 0, landing->GetID("ProjectChoices"));
			ui.Click(draw, landing, tab);
			ui.Type(draw, landing, ImHashStr("##ProjectName", 0, tab), "UiProject");
			ui.Type(draw, landing, ImHashStr("##ProjectLocation", 0, tab), FileSystem::PathToUtf8(directory.parent_path()).c_str());
			const int emptyIndex = 1;
			const ImGuiID templateScope = ImHashData(&emptyIndex, sizeof(emptyIndex), ImHashStr("Templates", 0, tab));
			ui.Click(draw, landing, ImHashStr("Empty project", 0, templateScope));
			ui.Click(draw, landing, ImHashStr("Create project", 0, tab));
			CHECK_FALSE(context.Editor.HasProject());
			CHECK_FALSE(FileSystem::Exists(directory / "UiProject.eproj"));
			context.Actions.Pump();
			REQUIRE(context.Editor.HasProject());
			CHECK(context.Editor.GetProject().GetProjectFile() == directory / "UiProject.eproj");
			CHECK(FileSystem::Exists(directory / "UiProject.eproj"));
			CHECK_FALSE(context.Editor.HasScene());
			const auto recent = ProjectManager::ReadRecentProjects(context.Editor.GetVfs());
			REQUIRE(recent);
			REQUIRE_FALSE(recent->empty());
			CHECK(recent->front() == directory / "UiProject.eproj");
		}

		TEST_CASE("ProjectLauncher: drawing recent projects preserves launcher and preferences")
		{
			Test::EditorTestFixture fixture("LauncherDraw");
			auto& editor = fixture.GetEditor();
			Test::AutomationTestClient client(editor);
			auto& server = client.GetServer();
			EditorActions actions(editor, server);
			EditorAutomationControls controls(editor, server);
			Test::PanelInteractionViewport viewports;
			ReflectedEditController edits(editor);
			GizmoController gizmos(editor);
			ThumbnailCache thumbnails(editor, {});
			EditorPanelContext context{ editor, server, actions, controls, viewports, edits, thumbnails, gizmos };
			REQUIRE(ProjectManager::AddRecentProject(editor.GetVfs(), fixture.GetProjectRoot() / "Recent.eproj"));
			const auto before = ProjectManager::ReadRecentProjects(editor.GetVfs());
			REQUIRE(before);
			// The host's snapshot, as EditorApp reads it at startup.
			context.GetRecentProjects = [&before]()
			{
				return *before;
			};
			ProjectLauncher launcher;
			Test::PanelInteractionUi ui;
			REQUIRE(ui.Frame([&launcher, &context]()
			{
				return launcher.Draw(context);
			}));
			actions.Pump();
			CHECK_FALSE(editor.HasProject());
			const auto after = ProjectManager::ReadRecentProjects(editor.GetVfs());
			REQUIRE(after);
			CHECK(*after == *before);
		}

		TEST_CASE("ProjectLauncher: drawing shows the host's recent-project snapshot and reads no user data")
		{
			Test::PanelInteractionFixture fixture("LauncherSnapshot", false);
			auto& context = fixture.GetContext();
			REQUIRE(context.Editor.CloseProject());
			const auto& directory = fixture.GetAutomation().GetEditorFixture().GetDirectory();
			const auto created = ProjectManager::CreateProject({ .Directory = directory / "Snapshot", .Name = "Snapshot", .Template = ProjectTemplate::Empty, .TemplatesDirectory = context.Editor.GetSpecification().TemplatesDirectory }, context.Editor.GetTypeRegistry());
			REQUIRE(created);
			uint32_t snapshots = 0;
			context.GetRecentProjects = [&snapshots, &created]() -> Result<std::vector<std::filesystem::path>>
			{
				++snapshots;
				return std::vector<std::filesystem::path>{ created->ProjectFile };
			};
			VirtualFileSystem& vfs = context.Editor.GetVfs();
			auto inner = vfs.Unmount("user");
			REQUIRE(inner);
			auto counter = CreateScope<LauncherReadCounter>(std::move(*inner));
			const LauncherReadCounter& reads = *counter;
			REQUIRE(vfs.Mount("user", std::move(counter)));
			ProjectLauncher launcher;
			Test::AssetDesignInteraction ui;
			uint32_t drawReads = 0;
			const auto draw = [&launcher, &context, &reads, &drawReads]()
			{
				const uint32_t before = reads.GetReads();
				const Status drawn = launcher.Draw(context);
				drawReads += reads.GetReads() - before;
				return drawn;
			};
			for (int frame = 0; frame < 3; ++frame)
				REQUIRE(ui.Frame(draw));
			CHECK(drawReads == 0);
			CHECK(snapshots == 1);
			auto* root = ui.Window("Panel interaction");
			auto* landing = ui.Child(root, root->GetID("ProjectLanding"));
			const ImGuiID tab = ImHashStr("Recent", 0, landing->GetID("ProjectChoices"));
			auto* entries = ui.Child(landing, ImHashStr("RecentProjects", 0, tab));
			const ImGuiID row = ImHashStr("##RecentProject", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, FileSystem::PathToUtf8(created->ProjectFile)));
			static_cast<void>(ui.Locate(draw, entries, row));
			// Hidden while a project is open, the launcher takes the host's snapshot again when it reappears.
			REQUIRE(fixture.GetAutomation().Call("project.open", Json{ { "path", FileSystem::PathToUtf8(created->ProjectFile) } }));
			REQUIRE(ui.Frame(draw));
			REQUIRE(context.Editor.CloseProject());
			REQUIRE(ui.Frame(draw));
			CHECK(snapshots == 2);
			CHECK(drawReads == 0);
		}
	}

}
