#include "TestsPCH.h"
#include "Editor/FileDialog.h"

#include "Editor/AssetDesignInteraction.h"
#include "Editor/ScriptedFileDialog.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/FileSystem.h"
#include "Support/TempDirectory.h"

#include <imgui_internal.h>

#include <system_error>

// The editor's FileDialog API (M18, C18.1, E-DIALOGS; Docs/Decisions/0029-m18-contract.md decisions 19 and 20) through a
// scripted backend and the ImGui picker. Roadmap M18: "The FileDialog API returns a path through the injected backend, and
// the ImGui picker stays the fallback headless, without a native backend and on Linux and macOS". Skipped until stream G
// implements the API.

namespace Engine {

	namespace {

		// Whether an ImGui window whose name contains `name` was drawn in the last frame.
		bool HasActiveWindow(std::string_view name)
		{
			for (const ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
				if (window->Active && std::string_view(window->Name).contains(name))
					return true;
			return false;
		}

		FileDialogRequest MakeOpenProjectRequest(const std::filesystem::path& directory)
		{
			return { .Kind = FileDialogKind::OpenFile, .Title = "Open a project", .InitialDirectory = directory, .Extensions = { ".eproj" }, .FilterName = "Projects" };
		}

		std::filesystem::path Canonical(const std::filesystem::path& path)
		{
			std::error_code error;
			std::filesystem::path canonical = std::filesystem::canonical(path, error);
			REQUIRE_FALSE(error);
			return canonical;
		}

	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("FileDialog: an injected backend's chosen path is returned once, revalidated" * doctest::skip(true))
		{
			Test::TempDirectory directory("FileDialogNative");
			REQUIRE(FileSystem::CreateDirectories(directory / "Projects"));
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Projects/Demo.eproj", AsBytes("{}")));
			const auto script = CreateRef<Test::ScriptedFileDialogScript>();
			script->PendingPolls = 2;
			// As a native dialog may return it: not canonical, the extension in another case.
			script->Outcome = FileDialogOutcome{ .State = FileDialogState::Accepted, .Path = directory / "Projects" / "." / "Demo.EPROJ" };
			Test::AssetDesignInteraction ui(false);
			FileDialog dialog;
			const FileDialogRequest request = MakeOpenProjectRequest(directory.GetPath());
			REQUIRE(dialog.Open(request, Test::MakeScriptedFileDialogBackend(script)));
			CHECK(dialog.IsOpen());
			CHECK(dialog.IsNative());
			REQUIRE(script->Requests.size() == 1);
			CHECK(script->Requests.front() == request);
			std::vector<std::optional<std::filesystem::path>> answers;
			const auto draw = [&dialog, &answers]() -> Status
			{
				ENGINE_TRY_ASSIGN(auto answer, dialog.Draw());
				answers.push_back(std::move(answer));
				return {};
			};
			for (int frame = 0; frame < 3; ++frame)
				REQUIRE(ui.Frame(draw));
			// Pending twice, drawing nothing, then the path once.
			REQUIRE(answers.size() == 3);
			CHECK_FALSE(answers[0].has_value());
			CHECK_FALSE(answers[1].has_value());
			REQUIRE(answers[2].has_value());
			CHECK(*answers[2] == Canonical(directory / "Projects/Demo.eproj"));
			CHECK_FALSE(HasActiveWindow("###FolderPicker"));
			CHECK_FALSE(dialog.IsOpen());
			CHECK_FALSE(dialog.IsNative());
			CHECK(script->LiveSessions == 0);
		}

		TEST_CASE("FileDialog: without a backend, and when the backend cannot show the request, the ImGui picker serves it" * doctest::skip(true))
		{
			Test::TempDirectory directory("FileDialogFallback");
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Demo.eproj", AsBytes("{}")));
			const auto script = CreateRef<Test::ScriptedFileDialogScript>();
			FileDialogBackend backend{};
			SUBCASE("no backend: headless editors and tests")
			{
			}
			SUBCASE("an Unsupported backend: Linux and macOS")
			{
				script->StartError = Error(ErrorCode::Unsupported, "Linux has no native file dialog: only Windows has a backend");
				backend = Test::MakeScriptedFileDialogBackend(script);
			}
			SUBCASE("a backend whose dialog cannot start")
			{
				script->StartError = Error(ErrorCode::Io, "CoInitializeEx failed");
				backend = Test::MakeScriptedFileDialogBackend(script);
			}
			Test::AssetDesignInteraction ui(false);
			FileDialog dialog;
			REQUIRE(dialog.Open(MakeOpenProjectRequest(directory.GetPath()), backend));
			CHECK(dialog.IsOpen());
			CHECK_FALSE(dialog.IsNative());
			CHECK(script->LiveSessions == 0);
			std::optional<std::filesystem::path> accepted;
			const auto draw = [&dialog, &accepted]() -> Status
			{
				ENGINE_TRY_ASSIGN(const auto answer, dialog.Draw());
				if (answer)
					accepted = *answer;
				return {};
			};
			REQUIRE(ui.Frame(draw));
			auto* window = ui.Window("###FolderPicker");
			auto* entries = ui.Child(window, window->GetID("Entries"));
			ui.Click(draw, entries, ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "Demo.eproj")));
			ui.Click(draw, window, "Open file");
			REQUIRE(accepted.has_value());
			CHECK(*accepted == Canonical(directory / "Demo.eproj"));
			CHECK_FALSE(dialog.IsOpen());
		}

		TEST_CASE("FileDialog: a cancelled native dialog closes without a path, and Cancel closes an open one" * doctest::skip(true))
		{
			Test::TempDirectory directory("FileDialogCancel");
			const auto script = CreateRef<Test::ScriptedFileDialogScript>();
			script->PendingPolls = 0;
			script->Outcome = FileDialogOutcome{ .State = FileDialogState::Cancelled, .Path = {} };
			Test::AssetDesignInteraction ui(false);
			FileDialog dialog;
			const FileDialogRequest request = { .Kind = FileDialogKind::PickFolder, .Title = "Choose a location", .InitialDirectory = directory.GetPath(), .Extensions = {}, .FilterName = {} };
			REQUIRE(dialog.Open(request, Test::MakeScriptedFileDialogBackend(script)));
			std::optional<std::filesystem::path> answer;
			const auto draw = [&dialog, &answer]() -> Status
			{
				ENGINE_TRY_ASSIGN(answer, dialog.Draw());
				return {};
			};
			REQUIRE(ui.Frame(draw));
			CHECK_FALSE(answer.has_value());
			CHECK_FALSE(dialog.IsOpen());
			CHECK(script->LiveSessions == 0);

			// A dialog still shown is closed by Cancel (its site's UI went away), through its session; then nothing is open.
			script->PendingPolls = 100;
			REQUIRE(dialog.Open(request, Test::MakeScriptedFileDialogBackend(script)));
			REQUIRE(ui.Frame(draw));
			CHECK(dialog.IsOpen());
			dialog.Cancel();
			dialog.Cancel();
			CHECK(script->Cancels >= 1);
			CHECK_FALSE(dialog.IsOpen());
			CHECK(script->LiveSessions == 0);
			// Opening again while one is open closes the first.
			REQUIRE(dialog.Open(request, Test::MakeScriptedFileDialogBackend(script)));
			const uint32_t cancels = script->Cancels;
			REQUIRE(dialog.Open(request, Test::MakeScriptedFileDialogBackend(script)));
			CHECK(script->Cancels == cancels + 1);
			CHECK(script->LiveSessions == 1);
		}

		TEST_CASE("FileDialog: a chosen path of the wrong kind or extension, or a failed dialog, is an error and closes it" * doctest::skip(true))
		{
			Test::TempDirectory directory("FileDialogRefused");
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Notes.txt", AsBytes("text")));
			REQUIRE(FileSystem::CreateDirectories(directory / "Folder.eproj"));
			const auto script = CreateRef<Test::ScriptedFileDialogScript>();
			script->PendingPolls = 0;
			struct Case
			{
				const char* Label = "";
				Result<FileDialogOutcome> Outcome = FileDialogOutcome{};
				ErrorCode Code = ErrorCode::InvalidArgument;
			};
			const std::vector<Case> cases = {
				{ "another extension", FileDialogOutcome{ .State = FileDialogState::Accepted, .Path = directory / "Notes.txt" }, ErrorCode::InvalidArgument },
				{ "a folder for a file", FileDialogOutcome{ .State = FileDialogState::Accepted, .Path = directory / "Folder.eproj" }, ErrorCode::InvalidArgument },
				{ "a file removed meanwhile", FileDialogOutcome{ .State = FileDialogState::Accepted, .Path = directory / "Gone.eproj" }, ErrorCode::NotFound },
				{ "the dialog's own error", std::unexpected(Error(ErrorCode::Io, "IFileDialog::Show failed")), ErrorCode::Io },
			};
			Test::AssetDesignInteraction ui(false);
			for (const Case& entry : cases)
			{
				CAPTURE(entry.Label);
				script->Outcome = entry.Outcome;
				FileDialog dialog;
				REQUIRE(dialog.Open(MakeOpenProjectRequest(directory.GetPath()), Test::MakeScriptedFileDialogBackend(script)));
				Result<std::optional<std::filesystem::path>> answer = std::optional<std::filesystem::path>();
				const auto draw = [&dialog, &answer]() -> Status
				{
					answer = dialog.Draw();
					return {};
				};
				REQUIRE(ui.Frame(draw));
				REQUIRE_FALSE(answer.has_value());
				CHECK(answer.error().GetCode() == entry.Code);
				CHECK_FALSE(dialog.IsOpen());
			}
		}

		TEST_CASE("FileDialog: a malformed request or an unreadable initial folder is refused and keeps the open dialog" * doctest::skip(true))
		{
			Test::TempDirectory directory("FileDialogRefusedOpen");
			const auto script = CreateRef<Test::ScriptedFileDialogScript>();
			script->PendingPolls = 100;
			FileDialog dialog;
			const FileDialogRequest request = MakeOpenProjectRequest(directory.GetPath());
			REQUIRE(dialog.Open(request, Test::MakeScriptedFileDialogBackend(script)));
			FileDialogRequest untitled = request;
			untitled.Title.clear();
			const Status refused = dialog.Open(untitled, Test::MakeScriptedFileDialogBackend(script));
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			FileDialogRequest absent = request;
			absent.InitialDirectory = directory / "Absent";
			const Status missing = dialog.Open(absent, Test::MakeScriptedFileDialogBackend(script));
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			// Neither reached the backend, and the first dialog is still open.
			CHECK(script->Requests.size() == 1);
			CHECK(dialog.IsOpen());
			CHECK(dialog.IsNative());
			CHECK(script->Cancels == 0);
		}
	}

}
