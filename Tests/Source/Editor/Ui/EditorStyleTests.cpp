#include "TestsPCH.h"
#include "Editor/Ui/EditorStyle.h"

#include "Editor/PanelInteractionFixture.h"
#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <filesystem>
#include <limits>

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorStyle: monitor DPI and framebuffer scale are applied exactly once")
		{
			CHECK(Utils::EditorDisplayScale(1.0f, 1.0f) == 1.0f);
			CHECK(Utils::EditorDisplayScale(1.5f, 1.0f) == 1.5f);
			CHECK(Utils::EditorDisplayScale(2.0f, 1.0f) == 2.0f);
			CHECK(Utils::EditorDisplayScale(2.0f, 2.0f) == 1.0f);
			CHECK(Utils::EditorDisplayScale(3.0f, 2.0f) == 1.5f);
			CHECK(Utils::EditorDisplayScale(2.0f, 0.0f) == 1.0f);
			CHECK(Utils::EditorDisplayScale(std::numeric_limits<float>::infinity(), 1.0f) == 1.0f);
			CHECK(Utils::EditorDisplayScale(2.0f, std::numeric_limits<float>::quiet_NaN()) == 1.0f);
		}

		TEST_CASE("EditorStyle: identifier labels preserve acronyms and authored strings are not required")
		{
			CHECK(Utils::EditorLabel("WorldPosition") == "World Position");
			CHECK(Utils::EditorLabel("SSAORadius") == "SSAO Radius");
			CHECK(Utils::EditorLabel("UseHDR") == "Use HDR");
			CHECK(Utils::EditorLabel("MeshRenderer") == "Mesh Renderer");
			CHECK(Utils::EditorLabel("Basic3D") == "Basic3D");
			CHECK(Utils::EditorLabel("").empty());
		}

		TEST_CASE("EditorStyle: monitor scaling restores absolute spacing without compounding")
		{
			Test::PanelInteractionUi ui;
			Utils::ApplyEditorStyle();
			const ImGuiStyle baseline = ImGui::GetStyle();
			Utils::ApplyEditorStyle(2.0f);
			CHECK(ImGui::GetStyle().FramePadding.x == baseline.FramePadding.x * 2.0f);
			CHECK(ImGui::GetStyle().FontSizeBase == baseline.FontSizeBase);
			CHECK(ImGui::GetStyle().FontScaleDpi == 2.0f);
			Utils::ApplyEditorStyle(2.0f);
			CHECK(ImGui::GetStyle().FramePadding.x == baseline.FramePadding.x * 2.0f);
			Utils::ApplyEditorStyle(1.0f);
			CHECK(ImGui::GetStyle().WindowPadding.x == baseline.WindowPadding.x);
			CHECK(ImGui::GetStyle().FontScaleDpi == 1.0f);
			Utils::ApplyEditorStyle(std::numeric_limits<float>::quiet_NaN());
			CHECK(ImGui::GetStyle().FramePadding.x == baseline.FramePadding.x);
		}

		TEST_CASE("EditorStyle: missing and truncated font resources report errors without modifying the atlas")
		{
			Test::PanelInteractionUi ui;
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("engine", CreateScope<MemoryMount>()));
			const int count = ImGui::GetIO().Fonts->Fonts.Size;
			const auto missing = Utils::LoadEditorFont(vfs);
			REQUIRE_FALSE(missing);
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			const auto directory = VfsPath::Create("engine", "Fonts");
			REQUIRE(directory);
			REQUIRE(vfs.CreateDirectories(*directory));
			const auto path = VfsPath::Create("engine", "Fonts/Inter-Regular.ttf");
			REQUIRE(path);
			REQUIRE(vfs.WriteFileAtomic(*path, AsBytes("truncated")));
			const auto truncated = Utils::LoadEditorFont(vfs);
			REQUIRE_FALSE(truncated);
			CHECK(truncated.error().GetCode() == ErrorCode::Validation);
			CHECK(ImGui::GetIO().Fonts->Fonts.Size == count);
			const Buffer invalid(256, std::byte{ 0 });
			REQUIRE(vfs.WriteFileAtomic(*path, invalid));
			const auto malformed = Utils::LoadEditorFont(vfs);
			REQUIRE_FALSE(malformed);
			CHECK(malformed.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(ImGui::GetIO().Fonts->Fonts.Size == count);
		}

		TEST_CASE("EditorStyle: the real font remains usable after its source mount is gone")
		{
			Test::PanelInteractionUi ui;
			{
				VirtualFileSystem vfs;
				REQUIRE(vfs.Mount("engine", CreateScope<NativeDirectoryMount>(std::filesystem::path(ENGINE_REPO_ROOT) / "Resources", MountAccess::ReadOnly)));
				REQUIRE(Utils::LoadEditorFont(vfs));
			}
			Utils::ApplyEditorStyle(2.0f);
			REQUIRE(ui.Frame([]() -> Status
			{
				CHECK(ImGui::GetFontSize() == doctest::Approx(30.0f));
				CHECK(std::string_view(ImGui::GetIO().FontDefault->GetDebugName()) == "Inter Regular");
				ImGui::TextUnformatted("Position / Rotation / Scale");
				CHECK(ImGui::CalcTextSize("WWW").x > ImGui::CalcTextSize("iii").x);
				return {};
			}));
		}
	}

}
