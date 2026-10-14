#include "EnginePCH.h"
#include "Engine/ImGui/StatsOverlay.h"

#include "Engine/Core/Assert.h"

#include <imgui.h>

#include <format>

namespace Engine {

	namespace Utils {

		// The overlay window's distance from the main viewport's top-left corner, in pixels.
		constexpr float StatsOverlayMargin = 8.0f;
		constexpr float StatsOverlayBackgroundAlpha = 0.6f;

	}

	std::vector<std::string> FormatStatsOverlay(const StatsOverlayData& data)
	{
		// std::format without the L specifier never uses a locale, so the text is the same on every machine.
		std::vector<std::string> lines;
		lines.reserve(8);
		lines.push_back(std::format("{:.1f} FPS ({:.1f} ms)", data.FramesPerSecond, data.FrameMilliseconds));
		lines.push_back(std::format("Tick {}", data.Tick));
		lines.push_back(std::format("Entities {}", data.Entities));
		lines.push_back(std::format("Bodies {}", data.Bodies));
		lines.push_back(std::format("Voices {}", data.Voices));
		lines.push_back(std::format("Script heap {} KiB", data.ScriptHeapBytes / 1024));
		lines.push_back(std::format("Draws {}", data.DrawCalls));
		lines.push_back(data.GpuMilliseconds < 0.0 ? std::string("GPU n/a") : std::format("GPU {:.1f} ms", data.GpuMilliseconds));
		return lines;
	}

	void ConfigureStatsOverlayContext()
	{
		ENGINE_CORE_ASSERT(ImGui::GetCurrentContext() != nullptr, "ConfigureStatsOverlayContext needs the ImGuiLayer's context");
		ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
	}

	void DrawStatsOverlay(const StatsOverlayData& data)
	{
		// Input-transparent: NoInputs lets every mouse event pass through to whatever lies below (ImGui never reports the
		// overlay as hovered, so it never sets WantCaptureMouse), and without focus or navigation it never takes the
		// keyboard. NoSavedSettings keeps it out of imgui.ini.
		constexpr ImGuiWindowFlags Flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove
			| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus
			| ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize;
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + Utils::StatsOverlayMargin, viewport->WorkPos.y + Utils::StatsOverlayMargin),
			ImGuiCond_Always);
		ImGui::SetNextWindowViewport(viewport->ID);
		ImGui::SetNextWindowBgAlpha(Utils::StatsOverlayBackgroundAlpha);
		if (ImGui::Begin("##StatsOverlay", nullptr, Flags))
		{
			for (const std::string& line : FormatStatsOverlay(data))
				ImGui::TextUnformatted(line.c_str(), line.c_str() + line.size());
		}
		ImGui::End();
	}

	void StatsOverlayToggle::OnKeyPressed(Key key, bool repeat)
	{
		if (key == Key::F1 && !repeat)
			m_Visible = !m_Visible;
	}

}
