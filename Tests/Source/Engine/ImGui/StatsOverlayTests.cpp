#include "TestsPCH.h"

#include "Engine/ImGui/StatsOverlay.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <string>
#include <string_view>
#include <vector>

// The Runtime's F1 stats overlay text and toggle (Architecture §8.3 pass 15, §14.4; Docs/Decisions/0024-m15-contract.md
// decision 17, stream D). The overlay's window itself is drawn only by a windowed non-Dist Runtime; the drawing case runs
// it in an ImGui context of its own without a device.

namespace Engine {

	namespace {

		// A Dear ImGui context of its own (no platform or renderer backend: the display size, delta and input events are set
		// by hand). The atlas builds itself (RendererHasTextures) and nothing uploads it, which a frame without rendering
		// does not need. No ini file.
		class DevicelessImGuiContext
		{
		public:
			DevicelessImGuiContext()
				: m_Context(ImGui::CreateContext())
			{
				ImGuiIO& io = ImGui::GetIO();
				io.DisplaySize = ImVec2(640.0f, 360.0f);
				io.DeltaTime = 1.0f / 60.0f;
				io.IniFilename = nullptr;
				io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			}

			~DevicelessImGuiContext()
			{
				ImGui::DestroyContext(m_Context);
			}

			DevicelessImGuiContext(const DevicelessImGuiContext&) = delete;
			DevicelessImGuiContext& operator=(const DevicelessImGuiContext&) = delete;
		private:
			ImGuiContext* m_Context = nullptr;
		};

		constexpr StatsOverlayData OverlayData{
			.FrameMilliseconds = 16.66,
			.FramesPerSecond = 60.02,
			.Tick = 1234,
			.Entities = 52,
			.Bodies = 50,
			.Voices = 3,
			.ScriptHeapBytes = 2048 * 1024,
			.DrawCalls = 77,
			.GpuMilliseconds = 4.26,
		};

	}

	TEST_SUITE("ImGui")
	{
		TEST_CASE("StatsOverlay: the overlay lists frame time, tick, entities, bodies, voices, script memory, draws and GPU time")
		{
			CHECK(FormatStatsOverlay(OverlayData) == std::vector<std::string>{ "60.0 FPS (16.7 ms)", "Tick 1234", "Entities 52", "Bodies 50", "Voices 3", "Script heap 2048 KiB", "Draws 77", "GPU 4.3 ms" });
			StatsOverlayData untimed = OverlayData;
			untimed.GpuMilliseconds = -1.0;
			const std::vector<std::string> lines = FormatStatsOverlay(untimed);
			REQUIRE(lines.size() == 8);
			CHECK(lines.back() == "GPU n/a");
		}

		TEST_CASE("StatsOverlay: hidden at start, each F1 press toggles it, and repeats and other keys do not")
		{
			StatsOverlayToggle toggle;
			CHECK_FALSE(toggle.IsVisible());
			toggle.OnKeyPressed(Key::F1, false);
			CHECK(toggle.IsVisible());
			toggle.OnKeyPressed(Key::F1, true); // a key repeat
			CHECK(toggle.IsVisible());
			toggle.OnKeyPressed(Key::F2, false);
			toggle.OnKeyPressed(Key::Escape, false);
			CHECK(toggle.IsVisible());
			toggle.OnKeyPressed(Key::F1, false);
			CHECK_FALSE(toggle.IsVisible());
		}

		TEST_CASE("StatsOverlay: the overlay's context never changes the OS cursor")
		{
			// Dear ImGui's GLFW backend resets a hidden or captured cursor to the normal arrow in every NewFrame unless this
			// flag is set, which would undo the game's cursor mode in a Release Runtime but not in Dist.
			DevicelessImGuiContext context;
			CHECK((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) == 0);
			ConfigureStatsOverlayContext();
			CHECK((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) == ImGuiConfigFlags_NoMouseCursorChange);
		}

		TEST_CASE("StatsOverlay: the overlay draws its lines in a fixed, input-transparent window without a device")
		{
			DevicelessImGuiContext context;
			ImGuiIO& io = ImGui::GetIO();
			float expectedContentHeight = 0.0f;
			// Three frames with the mouse resting on the overlay, a click in the second and F1 held: an input-transparent
			// window is never hovered or focused, so ImGui never asks for the mouse or the keyboard.
			for (int frame = 0; frame < 3; ++frame)
			{
				io.AddMousePosEvent(24.0f, 24.0f);
				io.AddMouseButtonEvent(ImGuiMouseButton_Left, frame == 1);
				io.AddKeyEvent(ImGuiKey_F1, true);
				ImGui::NewFrame();
				DrawStatsOverlay(OverlayData);
				// Eight lines of text: eight line heights and the item spacing between them.
				expectedContentHeight = 8.0f * ImGui::GetTextLineHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y;
				ImGui::Render();
				CHECK_FALSE(io.WantCaptureMouse);
				CHECK_FALSE(io.WantCaptureKeyboard);
			}

			const ImDrawData* drawData = ImGui::GetDrawData();
			REQUIRE(drawData != nullptr);
			CHECK(drawData->Valid);
			CHECK(drawData->CmdListsCount == 1);
			CHECK(drawData->TotalVtxCount > 0);

			const ImGuiWindow* window = ImGui::FindWindowByName("##StatsOverlay");
			REQUIRE(window != nullptr);
			CHECK(window->Active);
			CHECK((window->Flags & ImGuiWindowFlags_NoInputs) == ImGuiWindowFlags_NoInputs);
			CHECK((window->Flags & ImGuiWindowFlags_NoSavedSettings) == ImGuiWindowFlags_NoSavedSettings);
			CHECK(window->Pos.x == doctest::Approx(8.0f));
			CHECK(window->Pos.y == doctest::Approx(8.0f));
			CHECK(window->ContentSize.y == doctest::Approx(expectedContentHeight));
			CHECK(ImGui::GetCurrentContext()->HoveredWindow == nullptr);
			CHECK(ImGui::GetCurrentContext()->NavWindow != window);
			CHECK_FALSE(std::string_view(ImGui::SaveIniSettingsToMemory()).contains("StatsOverlay"));
		}
	}

}
