#include "TestsPCH.h"
#include "Editor/FolderPicker.h"

#include "Editor/AssetDesignInteraction.h"
#include "Editor/Ui/EditorStyle.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Platform/Environment.h"
#include "Engine/Platform/Process.h"
#include "Support/TempDirectory.h"

#include <imgui.h>

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("FolderPicker: transient instances do not enter saved workspace settings")
		{
			Test::TempDirectory directory("FolderPickerSettings");
			FolderPicker first;
			FolderPicker second;
			REQUIRE(first.Open("First picker", directory.GetPath()));
			REQUIRE(second.Open("Second picker", directory.GetPath()));
			Test::AssetDesignInteraction ui(false);
			const auto draw = [&first, &second]() -> Status
			{
				ENGINE_TRY(first.Draw());
				ENGINE_TRY(second.Draw());
				return {};
			};
			REQUIRE(ui.Frame(draw));
			CHECK(ui.Window("First picker")->ID != ui.Window("Second picker")->ID);
			const std::string_view settings(ImGui::SaveIniSettingsToMemory());
			CHECK_FALSE(settings.contains("FolderPicker"));
		}

		TEST_CASE("FolderPicker: a failed open preserves the active dialog and cancel is idempotent")
		{
			Test::TempDirectory directory("FolderPicker");
			FolderPicker picker;
			CHECK_FALSE(picker.IsOpen());
			CHECK(picker.Open("", directory.GetPath()).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK_FALSE(picker.IsOpen());
			REQUIRE(picker.Open("Choose folder", directory.GetPath()));
			CHECK(picker.IsOpen());
			CHECK(picker.Open("Other", directory / "Absent").error().GetCode() == ErrorCode::NotFound);
			CHECK(picker.IsOpen());
			picker.Cancel();
			picker.Cancel();
			CHECK_FALSE(picker.IsOpen());
			const auto result = picker.Draw();
			REQUIRE(result);
			CHECK_FALSE(result->has_value());
		}

		TEST_CASE("FolderPicker: a dangling link does not make its folder unbrowsable")
		{
			Test::TempDirectory directory("FolderDangling");
			REQUIRE(FileSystem::CreateDirectories(directory / "Child"));
			REQUIRE(FileSystem::CreateDirectories(directory / "Target"));
			REQUIRE(FileSystem::WriteFileAtomic(directory / "file.txt", AsBytes("file")));
			const auto link = directory / "Broken";
#if defined(ENGINE_PLATFORM_WINDOWS)
			// Junction creation needs no developer-mode privilege; both endpoints remain in this test's directory.
			const std::optional<std::string> shell = ReadEnvironmentVariable("ComSpec");
			REQUIRE(shell.has_value());
			std::string linkPath = FileSystem::PathToUtf8(link);
			std::string targetPath = FileSystem::PathToUtf8(directory / "Target");
			std::replace(linkPath.begin(), linkPath.end(), '/', '\\');
			std::replace(targetPath.begin(), targetPath.end(), '/', '\\');
			const auto linked = Process::Run({ .Executable = FileSystem::PathFromUtf8(*shell), .Arguments = { "/c", "mklink", "/J", linkPath, targetPath } }, std::chrono::seconds(30));
			REQUIRE_MESSAGE(linked.has_value(), linked.error().ToString());
			REQUIRE_MESSAGE(linked->ExitCode == 0, linked->StandardError);
#else
			std::error_code linkError;
			std::filesystem::create_directory_symlink(directory / "Target", link, linkError);
			REQUIRE_MESSAGE(!linkError, linkError.message());
#endif
			// The link now points nowhere: querying it fails like a dangling link or an entry removed meanwhile.
			REQUIRE(FileSystem::Remove(directory / "Target"));
			REQUIRE_FALSE(FileSystem::GetInfo(link));
			FolderPicker picker;
			const Status opened = picker.OpenFile("Choose a file", directory.GetPath());
			REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
			Test::AssetDesignInteraction ui(false);
			ImGui::GetIO().DisplaySize = ImVec2(800.0f, 600.0f);
			Result<std::optional<std::filesystem::path>> result;
			const auto draw = [&picker, &result]() -> Status
			{
				result = picker.Draw();
				return result ? Status{} : std::unexpected(result.error());
			};
			REQUIRE(ui.Frame(draw));
			auto* window = ui.Window("###FolderPicker");
			auto* entries = ui.Child(window, window->GetID("Entries"));
			// Every readable entry is still listed and usable.
			ui.Click(draw, entries, ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "file.txt")));
			ui.Click(draw, window, "Open file");
			REQUIRE(result);
			REQUIRE(result->has_value());
			std::error_code error;
			CHECK(**result == std::filesystem::canonical(directory / "file.txt", error));
			CHECK_FALSE(error);
		}

		TEST_CASE("FolderPicker: drawing native folders never changes the process directory")
		{
			Test::TempDirectory directory("FolderDraw");
			REQUIRE(FileSystem::CreateDirectories(directory / "Child"));
			REQUIRE(FileSystem::WriteFileAtomic(directory / "file.txt", AsBytes("file")));
			std::error_code error;
			const auto cwd = std::filesystem::current_path(error);
			REQUIRE_FALSE(error);
			FolderPicker picker;
			REQUIRE(picker.Open("Choose folder", directory.GetPath()));
			Test::AssetDesignInteraction ui(false);
			ImGui::GetIO().DisplaySize = ImVec2(800.0f, 600.0f);
			Result<std::optional<std::filesystem::path>> result;
			const auto draw = [&picker, &result]() -> Status
			{
				result = picker.Draw();
				return result ? Status{} : std::unexpected(result.error());
			};
			const auto frame = [&ui, &draw, &result]()
			{
				REQUIRE(ui.Frame(draw));
				return result;
			};
			const auto pending = frame();
			REQUIRE(pending);
			CHECK_FALSE(pending->has_value());
			CHECK(picker.IsOpen());
			CHECK(std::filesystem::current_path(error) == cwd);
			CHECK_FALSE(error);
			CHECK(picker.Open("Not a directory", directory / "file.txt").error().GetCode() == ErrorCode::NotFound);
			CHECK(picker.IsOpen());
			auto* window = ui.Window("###FolderPicker");
			auto* folders = ui.Child(window, window->GetID("Entries"));
			ui.Click(draw, folders, ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(folders->ID, "Child")));
			ui.Click(draw, window, "Select folder");
			const auto accepted = result;
			REQUIRE(accepted);
			REQUIRE(accepted->has_value());
			CHECK(**accepted == std::filesystem::canonical(directory / "Child", error));
			CHECK_FALSE(error);
			CHECK_FALSE(picker.IsOpen());
		}

		TEST_CASE("FolderPicker: file browsing filters extensions and returns a canonical file without typing")
		{
			Test::TempDirectory directory("AssetFilePicker");
			REQUIRE(FileSystem::CreateDirectories(directory / "Models"));
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Models/Ship.GLB", AsBytes("model")));
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Models/Other.png", AsBytes("image")));
			Test::AssetDesignInteraction ui(false);
			Utils::ApplyEditorStyle();
			ImGui::GetIO().DisplaySize = ImVec2(1280.0f, 720.0f);
			SUBCASE("standard scale")
			{
			}
			SUBCASE("large scale")
			{
				Utils::ApplyEditorStyle(1.5f);
				ImGui::GetIO().DisplaySize = ImVec2(1600.0f, 900.0f);
			}
			FolderPicker picker;
			REQUIRE(picker.OpenFile("Choose a model", directory.GetPath(), ".glb"));
			Result<std::optional<std::filesystem::path>> result;
			const auto draw = [&picker, &result]() -> Status
			{
				result = picker.Draw();
				return result ? Status{} : std::unexpected(result.error());
			};
			REQUIRE(ui.Frame(draw));
			auto* window = ui.Window("###FolderPicker");
			auto* entries = ui.Child(window, window->GetID("Entries"));
			ui.Click(draw, entries, ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "Models")));
			ui.Click(draw, entries, ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "Ship.GLB")));
			// These rows share a position and the clicks fall inside the double-click interval.
			// Entering a folder then selecting its first file must leave confirmation available.
			REQUIRE(picker.IsOpen());
			REQUIRE(result);
			CHECK_FALSE(result->has_value());
			// Neither an invalid mode request nor a failed navigation may lose the selection.
			CHECK_FALSE(picker.OpenFile("Wrong filter", directory.GetPath(), "glb"));
			CHECK_FALSE(picker.Open("Missing folder", directory / "Absent"));
			ui.Click(draw, window, "Open file");
			REQUIRE(result);
			REQUIRE(result->has_value());
			std::error_code error;
			CHECK(**result == std::filesystem::canonical(directory / "Models/Ship.GLB", error));
			CHECK_FALSE(error);
			CHECK_FALSE(picker.IsOpen());
			CHECK(window->Size.x <= ImGui::GetIO().DisplaySize.x);
			CHECK(window->Size.y <= ImGui::GetIO().DisplaySize.y);
		}

		TEST_CASE("FolderPicker: file browsing accepts any of several extensions")
		{
			// M18 (Docs/Decisions/0029-m18-contract.md decision 19): the picker serves the FileDialog API's requests, which
			// may list several extensions.
			Test::TempDirectory directory("AssetPickerExtensions");
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Ship.glb", AsBytes("model")));
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Tree.GLTF", AsBytes("model")));
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Notes.txt", AsBytes("text")));
			Test::AssetDesignInteraction ui(false);
			ImGui::GetIO().DisplaySize = ImVec2(800.0f, 600.0f);
			FolderPicker picker;
			const std::vector<std::string> extensions = { ".glb", ".gltf" };
			REQUIRE(picker.OpenFile("Choose a model", directory.GetPath(), std::span<const std::string>(extensions)));
			// One malformed extension refuses the whole list and keeps the open picker.
			const std::vector<std::string> malformed = { ".glb", "gltf" };
			const Status refused = picker.OpenFile("Choose a model", directory.GetPath(), std::span<const std::string>(malformed));
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(picker.IsOpen());
			Result<std::optional<std::filesystem::path>> result;
			const auto draw = [&picker, &result]() -> Status
			{
				result = picker.Draw();
				return result ? Status{} : std::unexpected(result.error());
			};
			REQUIRE(ui.Frame(draw));
			auto* window = ui.Window("###FolderPicker");
			auto* entries = ui.Child(window, window->GetID("Entries"));
			// Both extensions are listed, the second one in another case.
			const ImRect first = ui.Locate(draw, entries, ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "Ship.glb")));
			CHECK(first.GetWidth() > 0.0f);
			ui.Click(draw, entries, ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "Tree.GLTF")));
			ui.Click(draw, window, "Open file");
			REQUIRE(result);
			REQUIRE(result->has_value());
			std::error_code error;
			CHECK(**result == std::filesystem::canonical(directory / "Tree.GLTF", error));
			CHECK_FALSE(error);
			CHECK_FALSE(picker.IsOpen());
		}

		TEST_CASE("FolderPicker: double clicking the same file accepts its canonical path")
		{
			Test::TempDirectory directory("AssetPickerDoubleClick");
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Source.png", AsBytes("image")));
			Test::AssetDesignInteraction ui(false);
			FolderPicker picker;
			REQUIRE(picker.OpenFile("Choose an image", directory.GetPath()));
			std::optional<std::filesystem::path> accepted;
			const auto draw = [&picker, &accepted]() -> Status
			{
				ENGINE_TRY_ASSIGN(const auto result, picker.Draw());
				if (result)
					accepted = *result;
				return {};
			};
			REQUIRE(ui.Frame(draw));
			auto* window = ui.Window("###FolderPicker");
			auto* entries = ui.Child(window, window->GetID("Entries"));
			const ImRect row = ui.Locate(draw, entries, ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "Source.png")));
			REQUIRE(ui.Click(draw, row.GetCenter()));
			REQUIRE(picker.IsOpen());
			CHECK_FALSE(accepted.has_value());
			REQUIRE(ui.Click(draw, row.GetCenter()));
			CHECK_FALSE(picker.IsOpen());
			REQUIRE(accepted.has_value());
			std::error_code error;
			CHECK(*accepted == std::filesystem::canonical(directory / "Source.png", error));
			CHECK_FALSE(error);
		}

		TEST_CASE("FolderPicker: a file removed after selection remains a recoverable picker error")
		{
			Test::TempDirectory directory("AssetPickerRemoved");
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Source.png", AsBytes("image")));
			Test::AssetDesignInteraction ui(false);
			FolderPicker picker;
			REQUIRE(picker.OpenFile("Choose an image", directory.GetPath()));
			Result<std::optional<std::filesystem::path>> result;
			const auto draw = [&picker, &result]() -> Status
			{
				result = picker.Draw();
				return {};
			};
			REQUIRE(ui.Frame(draw));
			auto* window = ui.Window("###FolderPicker");
			auto* entries = ui.Child(window, window->GetID("Entries"));
			ui.Click(draw, entries, ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "Source.png")));
			REQUIRE(FileSystem::Remove(directory / "Source.png"));
			ui.Click(draw, window, "Open file");
			REQUIRE_FALSE(result);
			CHECK(result.error().GetCode() == ErrorCode::NotFound);
			CHECK(picker.IsOpen());
			ui.Click(draw, window, "Cancel");
			CHECK_FALSE(picker.IsOpen());
		}
	}

}
