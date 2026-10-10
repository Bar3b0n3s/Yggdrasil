#include "EditorPCH.h"
#include "Editor/Panels/InspectorPanel.h"

#include "Editor/Drawers/ReflectedDrawers.h"
#include "Editor/EditorPanelContext.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Scripting/EditorScriptService.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/AssetPipeline/Importers/SoundEffectImporter.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Session/PlaySession.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <tuple>

namespace Engine {

	namespace Utils {

		static bool InspectorReferenceMatches(std::string_view text, std::string_view query)
		{
			return query.empty() || std::search(text.begin(), text.end(), query.begin(), query.end(), [](char left, char right)
			{
				const auto fold = [](char character)
				{
					return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A')) : character;
				};
				return fold(left) == fold(right);
			}) != text.end();
		}

		static Result<std::vector<ReflectedReferenceCandidate>> FindInspectorReferenceCandidates(const EditorContext& editor,
			SceneTarget target, const FieldInfo& field, std::string_view query)
		{
			std::vector<ReflectedReferenceCandidate> candidates;
			auto append = [&candidates, query](UUID id, std::string label, std::string path)
			{
				if (InspectorReferenceMatches(label, query) || InspectorReferenceMatches(path, query) || InspectorReferenceMatches(id.ToString(), query))
					candidates.push_back({ .Id = id, .Label = std::move(label), .Path = std::move(path) });
			};
			if (field.GetKind() == FieldType::EntityRef)
			{
				const Scene* scene = nullptr;
				if (target == SceneTarget::Edit && editor.HasScene())
					scene = &editor.GetScene();
				else if (target == SceneTarget::Play && editor.GetPlay().GetSession() != nullptr)
					scene = &editor.GetPlay().GetSession()->GetScene();
				if (scene == nullptr)
					return MakeError(ErrorCode::InvalidState, "the addressed inspector scene is no longer available");
				scene->ForEachCanonical([scene, &append](ConstEntity entity)
				{
					append(entity.GetUUID(), entity.GetName(), scene->GetEntityPath(entity));
				});
			}
			else if (field.GetKind() == FieldType::AssetRef)
			{
				if (!editor.HasProject())
					return MakeError(ErrorCode::InvalidState, "no project is open for asset reference search");
				const auto filter = field.GetMeta().AssetFilter.empty() ? std::optional(AssetType::None) : AssetTypeFromString(field.GetMeta().AssetFilter);
				if (!filter)
					return MakeError(ErrorCode::Validation, "unknown asset filter '{}'", field.GetMeta().AssetFilter);
				const auto& assets = editor.GetAssets();
				const auto& registry = assets.GetRegistry();
				for (const AssetHandle handle : registry.GetHandles())
				{
					const auto location = registry.Locate(handle);
					if (!location || (*filter != AssetType::None && location->Type != *filter))
						continue;
					std::string label(location->Record->SourcePath.GetFileName());
					if (!location->SubAssetKey.empty())
						label += " / " + location->SubAssetKey;
					append(handle, std::move(label), registry.GetReferencePath(handle));
				}
				for (const auto& entry : assets.GetBuiltins().GetEntries())
				{
					if (*filter == AssetType::None || entry.Type == *filter)
					{
						const size_t slash = entry.Path.find_last_of('/');
						append(entry.Handle, entry.Path.substr(slash == std::string::npos ? 0 : slash + 1), entry.Path);
					}
				}
			}
			else
				return MakeError(ErrorCode::InvalidArgument, "reference search requires an entity or asset field");
			std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right)
			{
				return std::tie(left.Label, left.Path, left.Id) < std::tie(right.Label, right.Path, right.Id);
			});
			return candidates;
		}

	}

	void InspectorPanel::ReportFailure(const Error& error)
	{
		const std::string message = error.GetMessageText();
		if (m_Error != message)
			ENGINE_ERROR("InspectorPanel: {}", error.ToString());
		m_Error = message;
	}

	Status InspectorPanel::ApplyQueuedEdit(EditorPanelContext& context)
	{
		if (!m_CommitQueued)
			return {};
		m_CommitQueued = false;
		auto committed = context.InspectorEdits.Commit();
		if (!committed)
		{
			ReportFailure(committed.error());
			CancelEdit(context);
			return std::unexpected(std::move(committed).error());
		}
		m_ActiveTarget.reset();
		return {};
	}

	void InspectorPanel::CancelEdit(EditorPanelContext& context)
	{
		context.InspectorEdits.Cancel();
		m_CommitQueued = false;
		m_ActiveTarget.reset();
	}

	Status InspectorPanel::Draw(EditorPanelContext& context)
	{
		EditorContext& editor = context.Editor;
		for (auto ticket = m_Tickets.begin(); ticket != m_Tickets.end();)
		{
			auto result = context.Actions.TakeResult(*ticket);
			if (!result || result->has_value())
			{
				if (!result)
					ReportFailure(result.error());
				else if (!**result)
					ReportFailure((**result).error());
				ticket = m_Tickets.erase(ticket);
			}
			else
				++ticket;
		}
		const std::vector<UUID> selection(editor.GetSelection().begin(), editor.GetSelection().end());
		const AssetHandle asset = editor.GetUiState().GetSelectedAsset();
		const SceneTarget sceneTarget = editor.GetSelectionTarget();
		if (m_ActiveTarget && (ImGui::IsKeyPressed(ImGuiKey_Escape) || (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !m_CommitQueued) || m_ActiveTarget->Asset != asset || (m_ActiveTarget->Kind == InspectorTargetKind::Component && (m_ActiveTarget->Entities != selection || m_ActiveTarget->Target != sceneTarget))))
			CancelEdit(context);
		if (!m_Error.empty())
			ImGui::TextWrapped("%s", m_Error.c_str());
		if (sceneTarget == SceneTarget::Play && !selection.empty())
			ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Runtime values - changes end on Stop");
		auto queue = [this, &context](std::string_view method, const Json& params)
		{
			auto submitted = context.Actions.Submit(method, params);
			if (submitted)
				m_Tickets.push_back(*submitted);
			else
				ReportFailure(submitted.error());
		};
		auto drawField = [this, &context, &editor](const FieldInfo& field, Value value, InspectorEditTarget target,
							 const ResolveContext& resolve, bool mixed, const ScriptFieldSchema* scriptSchema = nullptr)
		{
			if (field.GetMeta().Hidden)
				return;
			const bool active = m_ActiveTarget && m_ActiveTarget->Kind == target.Kind && m_ActiveTarget->Component == target.Component
				&& m_ActiveTarget->FieldPath == target.FieldPath;
			if (active && context.InspectorEdits.IsEditing())
			{
				auto preview = context.InspectorEdits.GetPreview();
				if (preview)
					value = std::move(*preview);
			}
			// Keep the mixed-value row until commit so activation cannot move the widget away from a held pointer.
			auto result = DrawReflectedValue(field, value, { .Types = editor.GetTypeRegistry(), .Resolve = resolve, .Path = target.Component + "." + target.FieldPath, .ReadOnly = editor.IsReadOnly(), .Mixed = mixed, .FindReferences = [&editor, addressedTarget = target.Target](const FieldInfo& referenceField, std::string_view query)
			{
				return Utils::FindInspectorReferenceCandidates(editor, addressedTarget, referenceField, query);
			},
															   .ScriptSchema = scriptSchema });
			if (!result)
			{
				ReportFailure(result.error());
				return;
			}
			if (result->Cancelled)
			{
				CancelEdit(context);
				return;
			}
			if ((result->Activated || result->Changed) && !active)
			{
				CancelEdit(context);
				auto began = context.InspectorEdits.Begin(target);
				if (!began)
				{
					ReportFailure(began.error());
					return;
				}
				m_ActiveTarget = std::move(target);
			}
			if (result->Changed && context.InspectorEdits.IsEditing())
			{
				auto previewed = context.InspectorEdits.Preview(value);
				if (!previewed)
				{
					ReportFailure(previewed.error());
					return;
				}
			}
			if (result->Committed && context.InspectorEdits.IsEditing())
				m_CommitQueued = true;
		};
		if (asset.IsValid())
		{
			const AssetRecord* record = editor.GetAssets().GetRegistry().Find(asset);
			if (record == nullptr)
			{
				ImGui::TextDisabled("The selected asset is no longer available.");
				return {};
			}
			ImGui::TextWrapped("%s", record->SourcePath.ToString().c_str());
			const TypeRegistry& types = editor.GetTypeRegistry();
			auto drawObject = [&drawField, &types, asset](const StructInfo& type, const Json& json, InspectorTargetKind kind)
			{
				const JsonReader reader(json);
				for (const auto& field : type.GetFields())
				{
					auto found = json.find(field->GetName());
					if (found == json.end())
						continue;
					auto value = ValueFromJson(JsonReader(*found), field->GetType());
					if (value)
						drawField(*field, std::move(*value), { .Kind = kind, .Asset = asset, .FieldPath = field->GetName() },
							{ .Registry = &types, .OwnerType = &type, .OwnerJson = &reader, .Key = {} }, false);
				}
			};
			if (record->Metadata.Type == AssetType::Material || record->Metadata.Importer == SoundEffectImporter::Id)
			{
				ENGINE_TRY_ASSIGN(std::string text, editor.GetVfs().ReadText(record->SourcePath));
				if (record->Metadata.Type == AssetType::Material)
				{
					MaterialLoadReport report;
					ENGINE_TRY_ASSIGN(MaterialData material, MaterialFromText(text, types, report));
					const StructInfo* type = types.FindStruct<MaterialData>();
					ENGINE_TRY_ASSIGN(Json json, type->ToJson(&material));
					drawObject(*type, json, InspectorTargetKind::AssetProperties);
				}
				else
				{
					SoundEffectLoadReport report;
					ENGINE_TRY_ASSIGN(SoundEffectDescription sound, SoundEffectFromText(text, types, report));
					const StructInfo* type = types.FindStruct<SoundEffectDescription>();
					ENGINE_TRY_ASSIGN(Json json, type->ToJson(&sound));
					drawObject(*type, json, InspectorTargetKind::AssetProperties);
				}
			}
			ImporterRegistry importers;
			RegisterBuiltinImporters(importers);
			if (const IAssetImporter* importer = importers.FindById(record->Metadata.Importer))
			{
				const StructInfo* settings = types.FindStruct(importer->GetSettingsTypeName());
				if (settings != nullptr && ImGui::CollapsingHeader("Import settings"))
				{
					ImGui::PushID("ImportSettings");
					drawObject(*settings, record->Metadata.Settings.Get(), InspectorTargetKind::AssetImportSettings);
					ImGui::PopID();
				}
			}
			return {};
		}
		Scene* scene = sceneTarget == SceneTarget::Play ? (editor.GetPlay().GetSession() == nullptr ? nullptr : &editor.GetPlay().GetSession()->GetScene())
			: editor.HasScene()                         ? &editor.GetScene()
														: nullptr;
		if (scene == nullptr || selection.empty())
		{
			ImGui::TextDisabled("Select an entity or asset to inspect.");
			return {};
		}
		for (const UUID id : selection)
		{
			if (!scene->FindEntityByID(id).IsValid())
				return {};
		}
		Entity first = scene->FindEntityByID(selection.front());
		ImGui::Text("%zu selected", selection.size());
		const char* targetName = sceneTarget == SceneTarget::Play ? "Play" : "Edit";
		for (const ComponentInfo* component : editor.GetTypeRegistry().GetComponents())
		{
			const ComponentHostOps* ops = component->GetHostOps();
			if (!component->HasFlag(ComponentFlags::EditorVisible) || component->HasFlag(ComponentFlags::Hidden)
				|| component->HasFlag(ComponentFlags::EntityLevel) || ops == nullptr)
				continue;
			const bool shared = std::all_of(selection.begin(), selection.end(), [scene, ops](UUID id)
			{
				return ops->Has(scene->FindEntityByID(id));
			});
			if (!shared)
				continue;
			ImGui::PushID(component->GetName().c_str());
			if (ImGui::CollapsingHeader(component->GetName().c_str(), ImGuiTreeNodeFlags_DefaultOpen))
			{
				const void* object = ops->GetConst(first);
				ResolveContext resolve{ .Registry = &editor.GetTypeRegistry(), .Owner = object, .OwnerType = component, .Key = {} };
				for (const auto& field : component->GetFields())
				{
					if (component->GetName() == "Script" && field->GetName() == "Fields")
					{
						const auto& attached = first.GetComponent<ScriptComponent>();
						EditorScriptService* service = editor.GetScriptService();
						const auto script = service == nullptr ? Result<AssetRef<ScriptData>>(MakeError(ErrorCode::InvalidState, "script schemas are unavailable"))
															   : service->GetFields(attached.Script.GetHandle());
						if (!script)
						{
							ImGui::TextWrapped("Script fields: %s", script.error().GetMessageText().c_str());
							continue;
						}
						if ((*script)->Kind != ScriptKind::Behaviour)
						{
							ImGui::TextWrapped("SCRIPT_NOT_A_BEHAVIOUR: assign a Behaviour script.");
							continue;
						}
						auto schemas = ScriptFieldSchemaSource::Create({ { attached.Script.GetHandle(), *script } });
						if (!schemas)
						{
							ImGui::TextWrapped("Script fields: %s", schemas.error().GetMessageText().c_str());
							continue;
						}
						bool sameScript = true;
						for (const UUID id : selection)
							sameScript &= scene->FindEntityByID(id).GetComponent<ScriptComponent>().Script == attached.Script;
						if (!sameScript)
						{
							ImGui::TextDisabled("Select entities with the same Behaviour to edit script fields together.");
							continue;
						}
						ResolveContext scriptResolve = resolve;
						scriptResolve.Schemas = schemas->get();
						for (const auto& declaration : (*script)->Fields)
						{
							if (declaration.Meta.Hidden)
								continue;
							const auto descriptor = (*schemas)->FindField(attached.Script.GetHandle(), declaration.Name);
							if (!descriptor)
								continue;
							bool invalid = false;
							const auto effective = [&declaration, &descriptor](const ScriptComponent& instance, bool& mismatch)
							{
								const auto found = instance.Fields.find(declaration.Name);
								if (found == instance.Fields.end())
									return declaration.DefaultValue;
								ValidationContext validation;
								(*descriptor)->ValidateJson(JsonReader(found->second.Get()), {}, validation);
								mismatch |= validation.HasErrors();
								return validation.HasErrors() ? declaration.DefaultValue : found->second;
							};
							const VariantValue initial = effective(attached, invalid);
							bool mixed = false;
							for (const UUID id : selection)
								mixed |= effective(scene->FindEntityByID(id).GetComponent<ScriptComponent>(), invalid).Get() != initial.Get();
							if (invalid)
								ImGui::TextWrapped("SCRIPT_FIELD_TYPE_MISMATCH: %s uses its default until the override is corrected.", declaration.Name.c_str());
							FieldInfo variant({ .Name = declaration.Name, .Description = (*descriptor)->GetDescription(), .Type = field->GetType().GetElement(), .Meta = declaration.Meta, .Accessor = {}, .Resolver = field->GetResolver() });
							scriptResolve.Key = declaration.Name;
							drawField(variant, Value::FromVariant(initial), { .Entities = selection, .Component = "Script", .FieldPath = "Fields[" + declaration.Name + "]", .Target = sceneTarget },
								scriptResolve, mixed, &declaration);
						}
						for (const auto& [name, value] : attached.Fields)
						{
							if (!(*schemas)->FindField(attached.Script.GetHandle(), name))
								ImGui::TextWrapped("SCRIPT_UNKNOWN_FIELD_OVERRIDE: %s = %s (preserved)", name.c_str(), value.Get().dump().c_str());
						}
					}
					else
					{
						auto value = ComponentAccess::GetFieldValue(first, component->GetName(), field->GetName());
						if (!value)
							continue;
						bool mixed = false;
						for (const UUID id : selection)
						{
							auto other = ComponentAccess::GetFieldValue(scene->FindEntityByID(id), component->GetName(), field->GetName());
							mixed |= !other || *other != *value;
						}
						drawField(*field, std::move(*value), { .Entities = selection, .Component = component->GetName(), .FieldPath = field->GetName(), .Target = sceneTarget }, resolve, mixed);
					}
					if (selection.size() == 1 && first.HasComponent<PrefabLinkComponent>() && sceneTarget == SceneTarget::Edit)
					{
						const auto link = first.GetComponent<PrefabLinkComponent>();
						const ConstEntity root = scene->FindEntityByID(link.InstanceRoot);
						if (root.IsValid() && root.HasComponent<PrefabInstanceComponent>())
						{
							const auto& overrides = root.GetComponent<PrefabInstanceComponent>().Overrides;
							const bool overridden = std::any_of(overrides.begin(), overrides.end(), [&link, component, &field](const PrefabOverride& item)
							{
								return item.PrefabEntityID == link.PrefabEntityID && item.Component == component->GetName() && item.Field == field->GetName();
							});
							if (overridden)
							{
								ImGui::PushID(field->GetName().c_str());
								ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "Prefab override");
								ImGui::SameLine();
								ImGui::BeginDisabled(editor.IsReadOnly());
								if (ImGui::SmallButton("Revert"))
									queue("prefab.revert", Json{ { "instance", link.InstanceRoot.ToString() }, { "overrides", Json::array({ Json{ { "prefabEntityId", link.PrefabEntityID.ToString() }, { "kind", "Field" }, { "component", component->GetName() }, { "field", field->GetName() } } }) } });
								ImGui::EndDisabled();
								ImGui::PopID();
							}
						}
					}
				}
				ImGui::BeginDisabled(editor.IsReadOnly() || !component->HasFlag(ComponentFlags::Removable));
				if (ImGui::Button("Remove component"))
				{
					Json commands = Json::array();
					for (const UUID id : selection)
						commands.push_back(Json{ { "method", "entity.update" }, { "params", Json{ { "entity", id.ToString() }, { "removeComponents", Json::array({ component->GetName() }) }, { "target", targetName } } } });
					queue("edit.batch", Json{ { "label", "Remove component" }, { "ops", std::move(commands) } });
				}
				ImGui::EndDisabled();
			}
			ImGui::PopID();
		}
		ImGui::BeginDisabled(editor.IsReadOnly());
		if (ImGui::Button("Add Component"))
			ImGui::OpenPopup("AddComponent");
		if (ImGui::BeginPopup("AddComponent"))
		{
			std::string category;
			for (const ComponentInfo* component : editor.GetTypeRegistry().GetComponents())
			{
				if (!component->HasFlag(ComponentFlags::EditorVisible) || component->HasFlag(ComponentFlags::Hidden) || component->HasFlag(ComponentFlags::EntityLevel))
					continue;
				if (category != component->GetCategory())
				{
					category = component->GetCategory();
					ImGui::SeparatorText(category.c_str());
				}
				if (ImGui::Selectable(component->GetName().c_str()))
				{
					Json commands = Json::array();
					for (const UUID id : selection)
					{
						if (component->GetHostOps()->Has(scene->FindEntityByID(id)))
							continue;
						commands.push_back(Json{ { "method", "entity.update" }, { "params", Json{ { "entity", id.ToString() }, { "components", Json{ { component->GetName(), Json::object() } } }, { "target", targetName } } } });
					}
					if (!commands.empty())
						queue("edit.batch", Json{ { "label", "Add component" }, { "ops", std::move(commands) } });
				}
			}
			ImGui::EndPopup();
		}
		ImGui::EndDisabled();
		return {};
	}

}
