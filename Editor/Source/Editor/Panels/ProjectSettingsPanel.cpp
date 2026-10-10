#include "EditorPCH.h"
#include "Editor/Panels/ProjectSettingsPanel.h"

#include "Editor/Drawers/ReflectedDrawers.h"
#include "Editor/EditorPanelContext.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/Log.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <imgui.h>

#include <vector>

namespace Engine {

	namespace Utils {

		// Reflection merge patches replace Variants whole. A plain JSON diff would lose unchanged members in an
		// Input.Actions value when only one of its bindings changes. Follow the schema to retain the complete leaf.
		static Json MakeUtilitySettingsPatch(const Json& before, const Json& after, const TypeInfo& type)
		{
			if (type.GetKind() != FieldType::Struct && type.GetKind() != FieldType::Map)
				return after;
			Json patch = Json::object();
			for (auto value = after.begin(); value != after.end(); ++value)
			{
				const auto old = before.find(value.key());
				if (old != before.end() && *old == *value)
					continue;
				const TypeInfo* child = type.GetElement();
				if (type.GetKind() == FieldType::Struct)
				{
					const FieldInfo* field = type.GetStruct()->FindField(value.key());
					child = field ? &field->GetType() : nullptr;
				}
				patch[value.key()] = old != before.end() && child ? MakeUtilitySettingsPatch(*old, *value, *child) : *value;
			}
			for (auto value = before.begin(); value != before.end(); ++value)
			{
				if (!after.contains(value.key()))
					patch[value.key()] = nullptr;
			}
			return patch;
		}

		static Result<ReflectedDrawerResult> DrawSettingsSection(const StructInfo& type, Value& value,
			const ReflectedDrawerContext& drawer, std::string_view section)
		{
			std::vector<std::string> keys(value.GetKeys().begin(), value.GetKeys().end());
			std::vector<Value> values(value.GetElements().begin(), value.GetElements().end());
			ReflectedDrawerResult result;
			for (size_t index = 0; index < keys.size(); ++index)
			{
				const FieldInfo* field = type.FindField(keys[index]);
				if (!field || (section.empty() ? field->GetKind() == FieldType::Struct : keys[index] != section))
					continue;
				ReflectedDrawerContext child = drawer;
				child.Path += "." + keys[index];
				ENGINE_TRY_ASSIGN(const auto edit, DrawReflectedValue(*field, values[index], child));
				result.Activated |= edit.Activated;
				result.Changed |= edit.Changed;
				result.Committed |= edit.Committed;
				result.Cancelled |= edit.Cancelled;
			}
			if (result.Changed)
				value = Value::FromStruct(std::move(keys), std::move(values));
			return result;
		}

	}

	Status ProjectSettingsPanel::Draw(EditorPanelContext& context)
	{
		const auto report = [this](const Error& error)
		{
			const std::string text = error.ToString();
			if (text != m_Error)
				ENGINE_ERROR("Project settings: {}", error);
			m_Error = text;
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
			m_Project = project;
			++m_DraftEpoch;
			m_Ticket = 0;
			m_Value = {};
			m_Editing = false;
			m_Section.clear();
			m_Error.clear();
		}
		if (project.empty())
		{
			Utils::EditorEmptyState("No project open", "Open a project to configure its window, simulation, input and export settings.");
			return {};
		}
		if (m_Ticket != 0)
		{
			auto result = context.Actions.TakeResult(m_Ticket);
			if (!result || result->has_value())
			{
				m_Ticket = 0;
				m_Value = {};
				if (!result)
					report(result.error());
				else if (!**result)
					report((**result).error());
				else
					m_Error.clear();
			}
		}
		const TypeRegistry& registry = context.Editor.GetTypeRegistry();
		const StructInfo* type = registry.FindStruct<ProjectSettings>();
		if (!type)
			return MakeError(ErrorCode::InvalidState, "project settings type is not registered");
		if (m_Ticket == 0 && (m_Value.IsNull() || context.Editor.GetRevision() != m_Revision))
		{
			if (m_Editing)
			{
				report(Error(ErrorCode::Conflict, "project changed while editing settings; draft discarded"));
				++m_DraftEpoch;
			}
			m_Draft = context.Editor.GetProject().GetSettings();
			ENGINE_TRY_ASSIGN(m_Baseline, type->ToJson(&m_Draft));
			ENGINE_TRY_ASSIGN(m_Value, ValueFromJson(JsonReader(m_Baseline), type->GetType()));
			m_Revision = context.Editor.GetRevision();
			m_Editing = false;
		}
		std::string nextSection = m_Section;
		const std::string sectionLabel = m_Section.empty() ? "General" : Utils::EditorLabel(m_Section);
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::BeginCombo("##SettingsCategory", sectionLabel.c_str()))
		{
			if (ImGui::Selectable("General", m_Section.empty()))
				nextSection.clear();
			for (const auto& field : type->GetFields())
			{
				if (field->GetKind() != FieldType::Struct)
					continue;
				if (ImGui::Selectable(Utils::EditorLabel(field->GetName()).c_str(), m_Section == field->GetName()))
					nextSection = field->GetName();
			}
			ImGui::EndCombo();
		}
		ImGui::SetItemTooltip("Choose a settings category. Each completed edit is saved and can be undone.");
		ImGui::TextWrapped("%s", context.Editor.IsReadOnly() ? "Read-only project. Settings cannot be changed." : m_Ticket != 0 ? "Saving settings..."
																																: "Changes save automatically and can be undone.");
		Utils::EditorSectionHeading(sectionLabel);
		const ReflectedDrawerContext drawer{
			.Types = registry,
			.Resolve = { .Registry = &registry, .Owner = &m_Draft, .OwnerType = type, .Key = {} },
			.Path = "ProjectSettings",
			.ReadOnly = context.Editor.IsReadOnly() || m_Ticket != 0
		};
		ImGui::PushID(std::to_string(m_DraftEpoch).c_str());
		auto edit = Utils::DrawSettingsSection(*type, m_Value, drawer, m_Section);
		ImGui::PopID();
		// Navigation deactivates the old category's controls before replacing them. Finish the same validated edit
		// path, including Map/Variant merge semantics, instead of losing a draft when its widgets disappear.
		if (edit && nextSection != m_Section && m_Editing)
			edit->Committed = true;
		if (!edit)
			report(edit.error());
		else if (edit->Cancelled)
		{
			++m_DraftEpoch;
			m_Value = {};
			m_Editing = false;
		}
		else
		{
			m_Editing = m_Editing || edit->Activated || edit->Changed;
			if (edit->Changed || edit->Committed)
			{
				const auto document = ValueToJson(m_Value, type->GetType());
				if (!document)
					report(document.error());
				else
				{
					const Status validated = type->FromJson(&m_Draft, JsonReader(*document), {});
					if (!validated)
						report(validated.error());
					else if (edit->Committed)
					{
						const Json patch = Utils::MakeUtilitySettingsPatch(m_Baseline, *document, type->GetType());
						if (!patch.empty())
						{
							auto ticket = context.Actions.Submit("project.setSettings", Json{ { "patch", patch }, { "ifRevision", m_Revision } });
							if (ticket)
								m_Ticket = *ticket;
							else
								report(ticket.error());
						}
						m_Editing = false;
						if (m_Ticket == 0)
							m_Value = {};
					}
				}
			}
		}
		if (!m_Editing)
			m_Section = std::move(nextSection);
		if (!m_Error.empty())
			ImGui::TextWrapped("%s", m_Error.c_str());
		return {};
	}

}
