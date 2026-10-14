#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Platform/Input/KeyCodes.h"

#include <cstdint>
#include <string>
#include <vector>

// The Runtime's F1 stats overlay (Architecture §8.3 pass 15, §14.4 "Release keeps an F1 stats overlay"; Docs/Decisions/
// 0012-m7-decisions.md decision 16 deferred it to M15; Docs/Decisions/0024-m15-contract.md decision 17). A windowed
// Debug or Release Runtime enables ImGui and toggles the overlay with F1 (hidden at start); a headless run, and every Dist
// build, has neither ImGui nor the overlay, so the test runs and the shipping binary are unaffected (the Dist map check
// names any ImGui symbol). The text is built by a pure function and the F1 toggle is a pure state, so both are unit-tested
// without a device. The overlay never consumes input: F1 and every other event reach the game in every configuration, so
// Release and Dist games see identical input (the overlay window takes no input, and the Runtime ignores ImGui's
// WantCapture flags). The lead adopts the call site in Runtime/RuntimeApp.cpp after streams B and D merge.

namespace Engine {

	// One frame's numbers, gathered by the Runtime from its frame loop, play session and renderer.
	struct StatsOverlayData
	{
		double FrameMilliseconds = 0.0; // the last frame's CPU time (FrameStatistics)
		double FramesPerSecond = 0.0;
		uint64_t Tick = 0; // the play session's tick
		uint32_t Entities = 0;
		uint32_t Bodies = 0;
		uint32_t Voices = 0;
		uint64_t ScriptHeapBytes = 0;
		uint32_t DrawCalls = 0;
		double GpuMilliseconds = 0.0; // the sum of the last frame's GPU pass timings; negative when the device has none
	};

	// The overlay's lines, in this order: "<fps> FPS (<ms> ms)" with one decimal, "Tick <n>", "Entities <n>", "Bodies <n>",
	// "Voices <n>", "Script heap <KiB> KiB", "Draws <n>", and "GPU <ms> ms" with one decimal (or "GPU n/a" without
	// timings). Pure and locale-independent.
	[[nodiscard]] std::vector<std::string> FormatStatsOverlay(const StatsOverlayData& data);

	// Draws the lines of FormatStatsOverlay as a fixed, input-transparent ImGui window in the top-left corner (no inputs, no
	// focus, no saved settings). Main thread, between ImGui::NewFrame and ImGui::Render (Application::OnImGuiRender).
	void DrawStatsOverlay(const StatsOverlayData& data);

	// Whether the overlay shows: hidden at start; each press of F1 that is not a key repeat toggles it, and nothing else
	// does. The Runtime hands it every KeyPressed event and consumes none.
	class StatsOverlayToggle
	{
	public:
		void OnKeyPressed(Key key, bool repeat);
		[[nodiscard]] bool IsVisible() const { return m_Visible; }
	private:
		bool m_Visible = false;
	};

}
