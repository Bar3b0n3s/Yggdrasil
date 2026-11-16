#include "EditorPCH.h"
#include "Editor/Panels/SaveDataPanel.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Session/SaveDataStore.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <format>
#include <optional>
#include <string_view>
#include <utility>

namespace Engine {

	namespace Utils {

		// The longest value text a row shows; longer values end with an ellipsis.
		constexpr size_t SaveDataRowTextBytes = 160;

		[[nodiscard]] static std::string ShortenSaveDataText(std::string text)
		{
			if (text.size() <= SaveDataRowTextBytes)
				return text;
			size_t cut = SaveDataRowTextBytes;
			// Never inside a UTF-8 sequence: back up over continuation bytes.
			while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U)
				--cut;
			text.resize(cut);
			text += "...";
			return text;
		}

	}

	void SaveDataPanel::Refresh(EditorPanelContext& context)
	{
		EditorContext& editor = context.Editor;
		m_Project = editor.GetProject().GetProjectFile();
		const Result<SaveDataFileLoad> load = editor.GetPlay().ReadPlaySaveData();
		if (!load)
		{
			m_Values = VariantValue(Json::object());
			m_LoadError.clear();
			m_Problem = load.error().ToString();
			return;
		}
		m_Values = load->Values;
		m_LoadError = load->Error.value_or(std::string());
		m_Problem.clear();
	}

	void SaveDataPanel::Write(EditorPanelContext& context, const Json& values)
	{
		const Status written = context.Editor.GetPlay().WritePlaySaveData(values);
		Refresh(context);
		if (!written)
			m_Problem = written.error().ToString();
	}

	void SaveDataPanel::ApplyChange(EditorPanelContext& context, std::string_view key, std::optional<std::string_view> valueText)
	{
		Json value(nullptr);
		if (valueText.has_value())
		{
			Result<Json> parsed = JsonReader::Parse(*valueText);
			if (!parsed)
			{
				m_Problem = std::format("the value is not JSON: {}", parsed.error().ToString());
				return;
			}
			value = std::move(*parsed);
		}
		const Result<SaveDataFileLoad> load = context.Editor.GetPlay().ReadPlaySaveData();
		if (!load)
		{
			m_Problem = load.error().ToString();
			return;
		}
		SaveDataStore store;
		Status changed = store.Replace(load->Values.Get());
		if (changed)
			changed = store.Set(key, value);
		if (!changed)
		{
			m_Problem = changed.error().ToString();
			return;
		}
		Write(context, store.ToJson());
		// The read above moved a store it could not read aside, so Write's refresh finds a readable one: keep why.
		if (load->Error.has_value())
			m_LoadError = *load->Error;
	}

	Status SaveDataPanel::Draw(EditorPanelContext& context)
	{
		EditorContext& editor = context.Editor;
		if (!editor.HasProject())
		{
			m_Values.reset();
			Utils::EditorEmptyState("No project open", "Open a project to see the save data its play sessions start with.");
			return {};
		}
		if (!m_Values.has_value() || m_Project != editor.GetProject().GetProjectFile())
			Refresh(context);
		ImGui::TextWrapped("%s", editor.IsReadOnly() ? "This read-only editor's private store: play sessions start with it, SaveData.Commit writes it." : "Library/SaveData/Store.json: play sessions start with it, SaveData.Commit writes it.");
		if (ImGui::Button("Refresh"))
			Refresh(context);
		ImGui::SameLine();
		if (ImGui::Button("Clear"))
			Write(context, Json::object());
		if (!m_LoadError.empty())
			ImGui::TextWrapped("%s", m_LoadError.c_str());
		if (!m_Problem.empty())
			ImGui::TextWrapped("%s", m_Problem.c_str());

		ImGui::SeparatorText("Set a value");
		ImGui::InputText("Key", m_Key.data(), m_Key.size());
		ImGui::InputText("JSON value", m_ValueText.data(), m_ValueText.size());
		if (ImGui::Button("Set"))
			ApplyChange(context, std::string_view(m_Key.data()), std::string_view(m_ValueText.data()));

		ImGui::SeparatorText(std::format("{} values", m_Values->Get().size()).c_str());
		std::optional<std::string> deleted;
		if (ImGui::BeginTable("SaveData", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			for (const auto& [key, value] : m_Values->Get().items())
			{
				ImGui::PushID(key.c_str());
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(key.c_str());
				ImGui::TableNextColumn();
				const std::string text = Utils::ShortenSaveDataText(value.dump());
				ImGui::TextUnformatted(text.c_str());
				ImGui::TableNextColumn();
				if (ImGui::SmallButton("Delete"))
					deleted = key;
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		if (deleted.has_value())
			ApplyChange(context, *deleted, std::nullopt);
		return {};
	}

}
