#include "TestsPCH.h"
#include "Editor/FolderPicker.h"

#include "Editor/AssetDesignInteraction.h"
#include "Engine/Core/FileSystem.h"
#include "Support/TempDirectory.h"

#include <imgui.h>

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
	}

}
