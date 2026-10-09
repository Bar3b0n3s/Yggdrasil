#include "EditorPCH.h"
#include "Editor/Panels/ProjectSettingsPanel.h"

#include "Editor/Drawers/ReflectedDrawers.h"
#include "Editor/EditorPanelContext.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/Log.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <imgui.h>

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
		}
		if (project.empty())
		{
			ImGui::TextUnformatted("Open a project to edit settings");
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
		if (context.Editor.IsReadOnly())
			ImGui::TextUnformatted("Project is read-only");
		const ReflectedDrawerContext drawer{
			.Types = registry,
			.Resolve = { .Registry = &registry, .Owner = &m_Draft, .OwnerType = type, .Key = {} },
			.Path = "ProjectSettings",
			.ReadOnly = context.Editor.IsReadOnly() || m_Ticket != 0
		};
		ImGui::PushID(std::to_string(m_DraftEpoch).c_str());
		const auto edit = DrawReflectedValue(type->GetSelfField(), m_Value, drawer);
		ImGui::PopID();
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
		if (!m_Error.empty())
			ImGui::TextWrapped("%s", m_Error.c_str());
		return {};
	}

}
