#include "TestsPCH.h"

#include "Engine/ImGui/StatsOverlay.h"

#include <string>
#include <vector>

// The Runtime's F1 stats overlay text and toggle (Architecture §8.3 pass 15, §14.4; Docs/Decisions/0024-m15-contract.md
// decision 17, stream D). The overlay's window itself is drawn only by a windowed non-Dist Runtime; stream D adds the case
// that draws it in an ImGui context without a device.

namespace Engine {

	TEST_SUITE("ImGui")
	{
		TEST_CASE("StatsOverlay: the overlay lists frame time, tick, entities, bodies, voices, script memory, draws and GPU time" * doctest::skip(true))
		{
			const StatsOverlayData data{
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
			CHECK(FormatStatsOverlay(data) == std::vector<std::string>{ "60.0 FPS (16.7 ms)", "Tick 1234", "Entities 52", "Bodies 50", "Voices 3", "Script heap 2048 KiB", "Draws 77", "GPU 4.3 ms" });
			StatsOverlayData untimed = data;
			untimed.GpuMilliseconds = -1.0;
			const std::vector<std::string> lines = FormatStatsOverlay(untimed);
			REQUIRE(lines.size() == 8);
			CHECK(lines.back() == "GPU n/a");
		}

		TEST_CASE("StatsOverlay: hidden at start, each F1 press toggles it, and repeats and other keys do not" * doctest::skip(true))
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
	}

}
