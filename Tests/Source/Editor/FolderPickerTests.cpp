#include "TestsPCH.h"
#include "Editor/FolderPicker.h"

#include "Engine/Core/FileSystem.h"
#include "Support/TempDirectory.h"

#include <imgui.h>

namespace Engine {

	TEST_SUITE("Editor")
	{
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
			ImGuiContext* context = ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(800.0f, 600.0f);
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			const auto frame = [&picker]()
			{
				ImGui::NewFrame();
				ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
				auto result = picker.Draw();
				ImGui::Render();
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
			// Navigate into the first folder in the 660x440 picker, then accept; public input events only.
			io.AddMousePosEvent(50.0f, 112.0f);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			REQUIRE(frame());
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			REQUIRE(frame());
			io.AddMousePosEvent(50.0f, 375.0f);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			REQUIRE(frame());
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			const auto accepted = frame();
			ImGui::DestroyContext(context);
			REQUIRE(accepted);
			REQUIRE(accepted->has_value());
			CHECK(**accepted == std::filesystem::canonical(directory / "Child", error));
			CHECK_FALSE(error);
			CHECK_FALSE(picker.IsOpen());
		}
	}

}
