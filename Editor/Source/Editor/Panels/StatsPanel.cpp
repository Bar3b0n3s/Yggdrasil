#include "EditorPCH.h"
#include "Editor/Panels/StatsPanel.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <imgui.h>

namespace Engine {

	namespace Utils {

		static void DrawSupportingMetric(std::string_view label, std::string_view value)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextWrapped("%.*s", static_cast<int>(label.size()), label.data());
			ImGui::TableNextColumn();
			ImGui::TextWrapped("%.*s", static_cast<int>(value.size()), value.data());
		}

		static std::string SupportingMemoryLabel(float bytes)
		{
			if (bytes >= 1024.0f * 1024.0f)
				return std::format("{:.1f} MiB", bytes / (1024.0f * 1024.0f));
			if (bytes >= 1024.0f)
				return std::format("{:.1f} KiB", bytes / 1024.0f);
			return std::format("{:.0f} B", bytes);
		}

	}

	Status StatsPanel::Draw(EditorPanelContext& context)
	{
		const auto report = [this](const Error& error)
		{
			m_Error = error.ToString();
			m_Refresh = false;
			m_Live = false;
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
			Utils::EditorEmptyState("No project open", "Open a project to inspect frame timings, rendering and memory use.");
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
		ImGui::BeginDisabled(m_Ticket != 0);
		if (Utils::EditorToolbarButton("Refresh", "Request a fresh statistics sample without advancing the simulation."))
			m_Refresh = true;
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::Checkbox("Live", &m_Live);
		ImGui::SetItemTooltip("Update as samples arrive. Turn off to inspect the most recent sample.");
		if ((m_Refresh || m_Live) && m_Ticket == 0)
		{
			m_Refresh = false;
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
			Utils::EditorEmptyState("Statistics unavailable", m_Ticket != 0 ? "Waiting for the editor's first sample." : "Use Refresh to request a new sample.");
			return {};
		}
		ImGui::TextDisabled("%s", m_Live ? "Live sample" : "Updates paused");
		Utils::EditorSectionHeading("Frame & simulation");
		if (ImGui::BeginTable("FrameMetrics", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			Utils::DrawSupportingMetric("Frame rate", std::format("{:.1f} fps", m_Stats.Fps));
			Utils::DrawSupportingMetric("CPU frame", std::format("{:.2f} ms", m_Stats.CpuMilliseconds));
			Utils::DrawSupportingMetric("Dropped simulation time", std::format("{:.3f} s", m_Stats.DroppedSeconds));
			Utils::DrawSupportingMetric("Entities", std::to_string(m_Stats.Entities));
			Utils::DrawSupportingMetric("Physics bodies", std::to_string(m_Stats.Bodies));
			Utils::DrawSupportingMetric("Audio voices", std::to_string(m_Stats.Voices));
			ImGui::EndTable();
		}
		Utils::EditorSectionHeading("Memory");
		if (ImGui::BeginTable("MemoryMetrics", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			Utils::DrawSupportingMetric("Device allocations", m_Stats.MaxMemoryAllocationCount == 0 ? "Unavailable" : std::format("{} / {}", m_Stats.MemoryAllocationCount, m_Stats.MaxMemoryAllocationCount));
			if (m_Stats.ScriptAvailable)
			{
				Utils::DrawSupportingMetric("Script heap", Utils::SupportingMemoryLabel(m_Stats.ScriptHeapBytes));
				Utils::DrawSupportingMetric("Script soft limit", Utils::SupportingMemoryLabel(m_Stats.ScriptSoftLimitBytes));
				Utils::DrawSupportingMetric("Script hard limit", Utils::SupportingMemoryLabel(m_Stats.ScriptHardLimitBytes));
			}
			else
				Utils::DrawSupportingMetric("Script heap", "Unavailable (no active script VM)");
			ImGui::EndTable();
		}
		Utils::EditorSectionHeading("Rendering");
		if (m_Stats.Views.empty())
			ImGui::TextWrapped("Render statistics unavailable. No renderer samples have been reported.");
		for (const StatsViewSummary& view : m_Stats.Views)
		{
			ImGui::PushID(view.Name.c_str());
			ImGui::TextUnformatted(view.Name == "scene" ? "Scene view" : view.Name == "game" ? "Game view"
																							 : Utils::EditorLabel(view.Name).c_str());
			if (!view.Available)
			{
				ImGui::TextWrapped("Render statistics unavailable. Display this view with a renderer to collect samples.");
				ImGui::PopID();
				continue;
			}
			if (ImGui::BeginTable("ViewMetrics", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
			{
				Utils::DrawSupportingMetric("Resolution", std::format("{} x {} px", view.Width, view.Height));
				Utils::DrawSupportingMetric("CPU preparation", std::format("{:.2f} ms", view.CpuMilliseconds));
				Utils::DrawSupportingMetric("CPU frame", view.Frame);
				Utils::DrawSupportingMetric("GPU frame", view.GpuAvailable ? view.GpuFrame : "Unavailable");
				Utils::DrawSupportingMetric("Shadow draws", std::to_string(view.ShadowDraws));
				Utils::DrawSupportingMetric("Shadowed spot lights", std::to_string(view.ShadowedSpotLights));
				Utils::DrawSupportingMetric("Dropped spot shadows", std::to_string(view.DroppedSpotShadows));
				ImGui::EndTable();
			}
			for (const StatsPassSummary& pass : view.Passes)
			{
				ImGui::PushID(pass.Name.c_str());
				const std::string label = Utils::EditorLabel(pass.Name);
				if (ImGui::TreeNodeEx("Pass", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", label.c_str()))
				{
					if (ImGui::BeginTable("PassMetrics", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
					{
						Utils::DrawSupportingMetric("CPU", std::format("{:.3f} ms", pass.CpuMilliseconds));
						Utils::DrawSupportingMetric("GPU", pass.GpuAvailable ? std::format("{:.3f} ms", pass.GpuMilliseconds) : "Unavailable");
						Utils::DrawSupportingMetric("Draw calls", std::to_string(pass.DrawCalls));
						Utils::DrawSupportingMetric("Dispatches", std::to_string(pass.Dispatches));
						Utils::DrawSupportingMetric("Triangles", std::to_string(pass.Triangles));
						ImGui::EndTable();
					}
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
			ImGui::PopID();
		}
		return {};
	}

}
