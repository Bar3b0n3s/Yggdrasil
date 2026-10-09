#include "EditorPCH.h"
#include "Editor/Panels/StatsPanel.h"

#include "Editor/EditorPanelContext.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <imgui.h>

namespace Engine {

	Status StatsPanel::Draw(EditorPanelContext& context)
	{
		const auto report = [this](const Error& error)
		{
			m_Error = error.ToString();
			m_Refresh = false;
			m_HasSample = false;
			ENGINE_ERROR("Stats: {}", error);
		};
		const auto project = context.Editor.HasProject() ? context.Editor.GetProject().GetProjectFile() : std::filesystem::path{};
		if (project != m_Project)
		{
			if (m_Ticket != 0)
			{
				const Status cancelled = context.Actions.Cancel(m_Ticket);
				if (!cancelled)
					report(cancelled.error());
			}
			m_Ticket = 0;
			m_Project = project;
			m_HasSample = false;
			m_Refresh = true;
		}
		if (project.empty())
		{
			ImGui::TextUnformatted("Open a project to view statistics");
			return {};
		}
		if (m_Ticket != 0)
		{
			auto result = context.Actions.TakeResult(m_Ticket);
			if (!result || result->has_value())
			{
				m_Ticket = 0;
				if (!result)
					report(result.error());
				else if (!**result)
					report((**result).error());
				else
				{
					const StructInfo* type = context.Editor.GetTypeRegistry().FindStruct<StatsGetResult>();
					if (!type)
						return MakeError(ErrorCode::InvalidState, "stats result type is not registered");
					StatsGetResult sample;
					const Status decoded = type->FromJson(&sample, JsonReader(***result), {});
					if (!decoded)
						report(decoded.error());
					else
					{
						m_Stats = std::move(sample);
						m_HasSample = true;
						m_Error.clear();
					}
				}
			}
		}
		if (ImGui::Button("Refresh"))
			m_Refresh = true;
		if (m_Refresh && m_Ticket == 0)
		{
			auto ticket = context.Actions.Submit("stats.get", Json::object());
			if (ticket)
				m_Ticket = *ticket;
			else
				report(ticket.error());
		}
		if (!m_Error.empty())
			ImGui::TextWrapped("%s", m_Error.c_str());
		if (!m_HasSample)
		{
			ImGui::TextUnformatted("Statistics unavailable");
			return {};
		}
		ImGui::TextUnformatted(std::format("FPS {:.1f} | CPU {:.3f} ms | Dropped {:.3f} s", m_Stats.Fps, m_Stats.CpuMilliseconds, m_Stats.DroppedSeconds).c_str());
		ImGui::TextUnformatted(std::format("Entities {} | Bodies {} | Voices {}", m_Stats.Entities, m_Stats.Bodies, m_Stats.Voices).c_str());
		ImGui::TextUnformatted(std::format("Device memory allocations {} / {}", m_Stats.MemoryAllocationCount, m_Stats.MaxMemoryAllocationCount).c_str());
		if (m_Stats.ScriptAvailable)
			ImGui::TextUnformatted(std::format("Luau heap {:.0f} bytes | Soft limit {:.0f} | Hard limit {:.0f}", m_Stats.ScriptHeapBytes, m_Stats.ScriptSoftLimitBytes, m_Stats.ScriptHardLimitBytes).c_str());
		else
			ImGui::TextUnformatted("Luau heap unavailable");
		for (const StatsViewSummary& view : m_Stats.Views)
		{
			ImGui::SeparatorText(view.Name.c_str());
			if (!view.Available)
			{
				ImGui::TextUnformatted("Render statistics unavailable");
				continue;
			}
			ImGui::TextUnformatted(std::format("{} x {} | Frame {} | CPU {:.3f} ms", view.Width, view.Height, view.Frame, view.CpuMilliseconds).c_str());
			ImGui::TextUnformatted(view.GpuAvailable ? std::format("GPU frame {}", view.GpuFrame).c_str() : "GPU timings unavailable");
			ImGui::TextUnformatted(std::format("Shadow draws {} | Spots {} | Dropped {}", view.ShadowDraws, view.ShadowedSpotLights, view.DroppedSpotShadows).c_str());
			for (const StatsPassSummary& pass : view.Passes)
			{
				ImGui::TextUnformatted(std::format("{} | CPU {:.3f} ms | GPU {} | {} draws | {} dispatches | {} triangles", pass.Name, pass.CpuMilliseconds,
					pass.GpuAvailable ? std::format("{:.3f} ms", pass.GpuMilliseconds) : "unavailable", pass.DrawCalls, pass.Dispatches, pass.Triangles)
						.c_str());
			}
		}
		return {};
	}

}
