#include "EditorPCH.h"
#include "Editor/Panels/InspectorPanel.h"

#include "Editor/Drawers/ReflectedDrawers.h"
#include "Editor/EditorPanelContext.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Scripting/EditorScriptService.h"
#include "EditorCore/Thumbnails/ThumbnailCache.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/AssetPipeline/Importers/SoundEffectImporter.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
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
#include <utility>

namespace Engine {

	namespace Utils {

		static std::string InspectorOwnerIdentity(const EditorPanelContext& context, bool asset, SceneTarget target)
		{
			const EditorContext& editor = context.Editor;
			std::string identity = editor.HasProject() ? FileSystem::PathToUtf8(editor.GetProject().GetProjectFile()) : std::string{};
			// The host's project binding also distinguishes closing and reopening the same project path.
			identity += ":" + std::to_string(context.Thumbnails.GetProjectGeneration());
			if (asset)
				return identity + ":Asset";
			if (target == SceneTarget::Play)
			{
				const PlaySession* session = editor.GetPlay().GetSession();
				return identity + ":Play:" + std::to_string(session == nullptr ? 0 : session->GetSerial())
					+ ":" + std::to_string(session == nullptr ? 0 : session->GetSceneGeneration());
			}
			// Replacing or closing the scene document advances its epoch; edits, undo/redo and project-settings installations,
			// which all advance GetRevision, keep it, so they never discard a pending gesture or name edit.
			return identity + ":Edit:" + std::to_string(editor.GetSceneDocumentEpoch());
		}

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

		static std::optional<ReflectedReferenceCandidate> DescribeInspectorReference(const EditorContext& editor,
			SceneTarget target, const FieldInfo& field, UUID id)
		{
			if (field.GetKind() == FieldType::EntityRef)
			{
				const Scene* scene = target == SceneTarget::Edit ? (editor.HasScene() ? &editor.GetScene() : nullptr)
					: editor.GetPlay().GetSession() != nullptr   ? &editor.GetPlay().GetSession()->GetScene()
																 : nullptr;
				if (scene != nullptr)
				{
					const ConstEntity entity = scene->FindEntityByID(id);
					if (entity.IsValid())
						return ReflectedReferenceCandidate{ id, entity.GetName(), scene->GetEntityPath(entity) };
				}
			}
			else if (editor.HasProject())
			{
				const auto& assets = editor.GetAssets();
				const auto& registry = assets.GetRegistry();
				if (const auto location = registry.Locate(id))
				{
					std::string label(location->Record->SourcePath.GetFileName());
					if (!location->SubAssetKey.empty())
						label += " / " + location->SubAssetKey;
					return ReflectedReferenceCandidate{ id, std::move(label), registry.GetReferencePath(id) };
				}
				if (const auto* entry = assets.GetBuiltins().Find(id))
				{
					const size_t slash = entry->Path.find_last_of('/');
					return ReflectedReferenceCandidate{ id, entry->Path.substr(slash == std::string::npos ? 0 : slash + 1), entry->Path };
				}
			}
			return std::nullopt;
		}

		static std::string InspectorFieldLabel(std::string_view component, std::string_view field)
		{
			if (component == "Transform")
			{
				if (field == "Translation")
					return "Position";
				if (field == "EulerAngles")
					return "Rotation";
				if (field == "Rotation")
					return "Quaternion";
			}
			return EditorLabel(field);
		}

		static bool IsSameInspectorField(const InspectorEditTarget& left, const InspectorEditTarget& right)
		{
			return left.Kind == right.Kind && left.Component == right.Component && left.FieldPath == right.FieldPath;
		}

		// Every component of a value: a scalar, struct, container or rotation differs as a whole.
		constexpr uint32_t AllInspectorComponents = ~0u;

		// The components in which two values of one vector kind differ (bit i is component i); script vectors arrive as
		// Variant JSON arrays. AllInspectorComponents for differing values of every other kind.
		static uint32_t InspectorComponentDifferences(const Value& first, const Value& other)
		{
			if (first.IsNull() || other.IsNull() || first.GetKind() != other.GetKind())
				return AllInspectorComponents;
			const auto differences = [](const auto& left, const auto& right)
			{
				uint32_t mask = 0;
				for (glm::length_t index = 0; index < left.length(); ++index)
				{
					if (left[index] != right[index])
						mask |= 1u << static_cast<uint32_t>(index);
				}
				return mask;
			};
			switch (first.GetKind())
			{
				case FieldType::Vec2:  return differences(first.AsVec2(), other.AsVec2());
				case FieldType::Vec3:  return differences(first.AsVec3(), other.AsVec3());
				case FieldType::Vec4:  return differences(first.AsVec4(), other.AsVec4());
				case FieldType::Bool3: return differences(first.AsBool3(), other.AsBool3());
				case FieldType::Variant:
				{
					const Json& left = first.AsVariant().Get();
					const Json& right = other.AsVariant().Get();
					if (!left.is_array() || !right.is_array() || left.size() != right.size() || left.size() > 4)
						return left == right ? 0u : AllInspectorComponents;
					uint32_t mask = 0;
					for (size_t index = 0; index < left.size(); ++index)
					{
						if (left[index] != right[index])
							mask |= 1u << static_cast<uint32_t>(index);
					}
					return mask;
				}
				default: return first == other ? 0u : AllInspectorComponents;
			}
		}

		// Asset files are re-read at least this often while selected, so an external edit shows without a re-selection.
		constexpr double AssetPropertiesRefreshSeconds = 0.5;

	}

	void InspectorPanel::ReportFailure(const Error& error)
	{
		const std::string message = error.GetMessageText();
		if (m_Error != message)
			ENGINE_ERROR("InspectorPanel: {}", error.ToString());
		m_Error = message;
	}

	Status InspectorPanel::CommitActiveEdit(EditorPanelContext& context)
	{
		m_CommitQueued = false;
		auto committed = context.InspectorEdits.Commit();
		if (!committed)
		{
			ReportFailure(committed.error());
			CancelEdit(context);
			return std::unexpected(std::move(committed).error());
		}
		m_ActiveTarget.reset();
		m_ActiveComponents = 0;
		m_ActiveMixed = false;
		m_Error.clear();
		return {};
	}

	Status InspectorPanel::ApplyQueuedEdit(EditorPanelContext& context)
	{
		const auto replaced = [this, &context](const InspectorEditTarget& target)
		{
			return m_OwnerIdentity != Utils::InspectorOwnerIdentity(context, target.Kind != InspectorTargetKind::Component, target.Target);
		};
		if ((m_ActiveTarget && replaced(*m_ActiveTarget)) || (m_Pending && replaced(m_Pending->Target)))
		{
			CancelEdit(context);
			return {};
		}
		if (m_CommitQueued)
			ENGINE_TRY(CommitActiveEdit(context));
		// A gesture that started while the previous one was releasing begins now, against the committed state.
		if (!m_ActiveTarget && m_Pending)
		{
			BeginPendingEdit(context, *std::exchange(m_Pending, std::nullopt));
			if (m_CommitQueued)
				ENGINE_TRY(CommitActiveEdit(context));
		}
		return {};
	}

	void InspectorPanel::EndActiveEdit(EditorPanelContext& context)
	{
		context.InspectorEdits.Cancel();
		m_CommitQueued = false;
		m_ActiveTarget.reset();
		m_ActiveComponents = 0;
		m_ActiveMixed = false;
	}

	void InspectorPanel::CancelEdit(EditorPanelContext& context)
	{
		EndActiveEdit(context);
		m_Pending.reset();
	}

	void InspectorPanel::BeginPendingEdit(EditorPanelContext& context, PendingEdit pending)
	{
		auto began = context.InspectorEdits.Begin(pending.Target);
		if (!began)
		{
			ReportFailure(began.error());
			return;
		}
		m_ActiveTarget = std::move(pending.Target);
		m_ActiveComponents = pending.Components;
		m_ActiveMixed = pending.Mixed;
		if (pending.Changed)
		{
			auto previewed = context.InspectorEdits.Preview(pending.Candidate, pending.Mixed ? pending.Components : 0);
			if (!previewed)
			{
				ReportFailure(previewed.error());
				if (pending.Committed)
					EndActiveEdit(context);
				return;
			}
		}
		m_CommitQueued = pending.Committed;
	}

	const InspectorPanel::AssetProperties& InspectorPanel::ReadAssetProperties(EditorPanelContext& context, AssetHandle asset)
	{
		EditorContext& editor = context.Editor;
		const double now = ImGui::GetTime();
		AssetProperties properties;
		properties.Asset = asset;
		properties.ProjectGeneration = context.Thumbnails.GetProjectGeneration();
		properties.Version = editor.GetAssets().GetVersion(asset);
		properties.HistorySequence = editor.GetHistory().GetCurrentSequence();
		properties.HistoryUndoCount = editor.GetHistory().GetUndoCount();
		properties.Revision = editor.GetRevision();
		if (m_AssetProperties && m_AssetProperties->Asset == properties.Asset && m_AssetProperties->ProjectGeneration == properties.ProjectGeneration
			&& m_AssetProperties->Version == properties.Version && m_AssetProperties->HistorySequence == properties.HistorySequence
			&& m_AssetProperties->HistoryUndoCount == properties.HistoryUndoCount && m_AssetProperties->Revision == properties.Revision
			&& now < m_AssetProperties->RefreshAt)
			return *m_AssetProperties;
		properties.RefreshAt = now + Utils::AssetPropertiesRefreshSeconds;
		const AssetRecord* record = editor.GetAssets().GetRegistry().Find(asset);
		if (record != nullptr)
		{
			const TypeRegistry& types = editor.GetTypeRegistry();
			ImporterRegistry importers;
			RegisterBuiltinImporters(importers);
			if (const IAssetImporter* importer = importers.FindById(record->Metadata.Importer))
				properties.Settings = types.FindStruct(importer->GetSettingsTypeName());
			const bool material = record->Metadata.Type == AssetType::Material;
			if (material || record->Metadata.Importer == SoundEffectImporter::Id)
			{
				const Status read = [&]() -> Status
				{
					ENGINE_TRY_ASSIGN(const std::string text, editor.GetVfs().ReadText(record->SourcePath));
					if (material)
					{
						MaterialLoadReport report;
						ENGINE_TRY_ASSIGN(const MaterialData data, MaterialFromText(text, types, report));
						properties.Type = types.FindStruct<MaterialData>();
						ENGINE_TRY_ASSIGN(properties.Values, properties.Type->ToJson(&data));
					}
					else
					{
						SoundEffectLoadReport report;
						ENGINE_TRY_ASSIGN(const SoundEffectDescription data, SoundEffectFromText(text, types, report));
						properties.Type = types.FindStruct<SoundEffectDescription>();
						ENGINE_TRY_ASSIGN(properties.Values, properties.Type->ToJson(&data));
					}
					return {};
				}();
				if (!read)
				{
					properties.Type = nullptr;
					properties.Error = read.error().GetMessageText();
					// Logged once per failure, not once per re-read; the inspector shows it inline meanwhile.
					if (!m_AssetProperties || m_AssetProperties->Asset != asset || m_AssetProperties->Error != properties.Error)
						ENGINE_WARN("InspectorPanel: cannot read the properties of '{}': {}", record->SourcePath.ToString(), read.error().ToString());
				}
			}
		}
		m_AssetProperties = std::move(properties);
		return *m_AssetProperties;
	}

	Status InspectorPanel::Draw(EditorPanelContext& context)
	{
		EditorContext& editor = context.Editor;
		const AssetHandle asset = editor.GetUiState().GetSelectedAsset();
		const SceneTarget sceneTarget = editor.GetSelectionTarget();
		Scene* scene = sceneTarget == SceneTarget::Play ? (editor.GetPlay().GetSession() == nullptr ? nullptr : &editor.GetPlay().GetSession()->GetScene())
			: editor.HasScene()                         ? &editor.GetScene()
														: nullptr;
		// Ids missing from the target scene (destroyed since the last safe point) are skipped; the others stay inspectable.
		std::vector<UUID> selection;
		if (scene != nullptr)
		{
			for (const UUID id : editor.GetSelection())
			{
				if (scene->FindEntityByID(id).IsValid())
					selection.push_back(id);
			}
		}
		const std::string ownerIdentity = Utils::InspectorOwnerIdentity(context, asset.IsValid(), sceneTarget);
		if (m_OwnerIdentity != ownerIdentity)
		{
			CancelEdit(context);
			m_OwnerIdentity = ownerIdentity;
			m_Error.clear();
		}
		if (selection != m_ErrorSelection || asset != m_ErrorAsset)
		{
			// An error describes the inspected objects; it does not follow the user to another selection.
			m_Error.clear();
			m_ErrorSelection = selection;
			m_ErrorAsset = asset;
		}
		// Results arrive after the selection check, so a failure stays visible even when the selection changed meanwhile.
		for (auto ticket = m_Tickets.begin(); ticket != m_Tickets.end();)
		{
			auto result = context.Actions.TakeResult(*ticket);
			if (!result || result->has_value())
			{
				if (!result)
					ReportFailure(result.error());
				else if (!**result)
					ReportFailure((**result).error());
				else
					m_Error.clear();
				ticket = m_Tickets.erase(ticket);
			}
			else
				++ticket;
		}
		const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape);
		const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		const auto retargeted = [&selection, asset, sceneTarget](const InspectorEditTarget& target)
		{
			return target.Asset != asset || (target.Kind == InspectorTargetKind::Component && (target.Entities != selection || target.Target != sceneTarget));
		};
		if ((m_ActiveTarget || m_Pending)
			&& (editor.IsReadOnly() || escape || (!focused && !m_CommitQueued) || (m_ActiveTarget && retargeted(*m_ActiveTarget)) || (m_Pending && retargeted(m_Pending->Target))))
			CancelEdit(context);
		if (!m_Error.empty() && Utils::EditorErrorNotice(m_Error))
			m_Error.clear();
		const bool runtime = sceneTarget == SceneTarget::Play && !selection.empty();
		if (runtime)
			Utils::EditorPlayNotice("Runtime values - Changes end when you stop play.");
		if (editor.IsReadOnly())
			ImGui::TextDisabled("Read only");
		bool activeFieldDrawn = false;
		bool pendingFieldDrawn = false;
		auto queue = [this, &context](std::string_view method, const Json& params)
		{
			auto submitted = context.Actions.Submit(method, params);
			if (submitted)
				m_Tickets.push_back(*submitted);
			else
				ReportFailure(submitted.error());
		};
		auto drawField = [this, &context, &editor, &activeFieldDrawn, &pendingFieldDrawn, escape](const FieldInfo& field, Value value,
							 InspectorEditTarget target, const ResolveContext& resolve, bool mixed, uint32_t mixedComponents,
							 const ScriptFieldSchema* scriptSchema = nullptr)
		{
			if (field.GetMeta().Hidden)
				return;
			const bool active = m_ActiveTarget && Utils::IsSameInspectorField(*m_ActiveTarget, target);
			const bool pending = !active && m_Pending && Utils::IsSameInspectorField(m_Pending->Target, target);
			activeFieldDrawn |= active;
			pendingFieldDrawn |= pending;
			if (active && context.InspectorEdits.IsEditing())
			{
				auto preview = context.InspectorEdits.GetPreview();
				if (preview)
					value = std::move(*preview);
			}
			else if (pending && m_Pending->Changed)
				value = m_Pending->Candidate;
			// Selection and lifecycle identity prevent ImGui buffers from carrying over to replacement owners with the same UUID.
			std::string path = std::to_string(static_cast<int>(target.Kind)) + ":" + target.Asset.ToString() + ":" + target.Component + "." + target.FieldPath;
			for (const UUID id : target.Entities)
				path += ":" + id.ToString();
			path += target.Target == SceneTarget::Play ? ":Play" : ":Edit";
			path += ":" + m_OwnerIdentity;
			auto result = DrawReflectedValue(field, value, { .Types = editor.GetTypeRegistry(), .Resolve = resolve, .Path = std::move(path), .ReadOnly = editor.IsReadOnly(), .Mixed = mixed, .MixedComponents = mixedComponents == Utils::AllInspectorComponents ? 0u : mixedComponents, .FindReferences = [&editor, addressedTarget = target.Target](const FieldInfo& referenceField, std::string_view query)
			{
				return Utils::FindInspectorReferenceCandidates(editor, addressedTarget, referenceField, query);
			},
															   .ScriptSchema = scriptSchema,
															   .DisplayLabel = Utils::InspectorFieldLabel(target.Component, field.GetName()),
															   .DescribeReference = [&editor, addressedTarget = target.Target](const FieldInfo& referenceField, UUID id)
			{
				return Utils::DescribeInspectorReference(editor, addressedTarget, referenceField, id);
			} });
			if (!result)
			{
				ReportFailure(result.error());
				if (active)
					EndActiveEdit(context);
				if (pending)
					m_Pending.reset();
				return;
			}
			if (escape || result->Cancelled)
			{
				CancelEdit(context);
				return;
			}
			// A text entry that another item's activation released earlier in the frame applies its text after it stopped
			// being active, without ImGui's deactivated-after-edit flag. That is a release all the same.
			const bool released = result->Committed || (result->Changed && !result->Active);
			if (active)
			{
				if (!m_CommitQueued && !result->Active && !result->Activated && !result->Changed && !result->Committed)
				{
					EndActiveEdit(context);
					return;
				}
				if (result->Changed && context.InspectorEdits.IsEditing())
				{
					m_ActiveComponents |= result->ChangedComponents;
					auto previewed = context.InspectorEdits.Preview(value, m_ActiveMixed ? m_ActiveComponents : 0);
					if (!previewed)
					{
						ReportFailure(previewed.error());
						return;
					}
				}
				// Only the active field's release commits its gesture.
				if (released && context.InspectorEdits.IsEditing())
					m_CommitQueued = true;
				return;
			}
			if (pending)
			{
				if (result->Changed)
				{
					m_Pending->Candidate = value;
					m_Pending->Changed = true;
					m_Pending->Components |= result->ChangedComponents;
				}
				m_Pending->Committed |= released;
				if (!result->Active && !result->Activated && !result->Changed && !result->Committed && !m_Pending->Changed)
					m_Pending.reset();
				return;
			}
			if (!result->Activated && !result->Changed)
				return;
			PendingEdit edit{ .Target = std::move(target), .Candidate = value, .Components = result->ChangedComponents, .Mixed = mixed, .Changed = result->Changed, .Committed = released };
			if (m_ActiveTarget)
			{
				// The previous gesture still owns the controller: it was released this frame, or its field releases later in
				// this frame. This one begins once that edit is committed or ended, so neither is lost.
				m_Pending = std::move(edit);
				pendingFieldDrawn = true;
				return;
			}
			BeginPendingEdit(context, std::move(edit));
			activeFieldDrawn |= m_ActiveTarget.has_value();
		};
		const auto finishFrame = [this, &context, &activeFieldDrawn, &pendingFieldDrawn]()
		{
			// A released edit waits for the safe point even when its field is not drawn (a collapsed header).
			if (m_ActiveTarget && !activeFieldDrawn && !m_CommitQueued)
				EndActiveEdit(context);
			if (m_Pending && !pendingFieldDrawn)
				m_Pending.reset();
			if (!m_ActiveTarget && m_Pending)
				BeginPendingEdit(context, *std::exchange(m_Pending, std::nullopt));
		};
		if (asset.IsValid())
		{
			const AssetRecord* record = editor.GetAssets().GetRegistry().Find(asset);
			if (record == nullptr)
			{
				CancelEdit(context);
				Utils::EditorEmptyState("No editable source", "This resource is built in, imported as part of another asset, or no longer available. Select its source asset to inspect import settings.");
				return {};
			}
			Utils::EditorSectionHeading(record->SourcePath.GetFileName());
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("%s", record->SourcePath.ToString().c_str());
			ImGui::PopStyleColor();
			ImGui::Spacing();
			const TypeRegistry& types = editor.GetTypeRegistry();
			const AssetProperties& properties = ReadAssetProperties(context, asset);
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
							{ .Registry = &types, .OwnerType = &type, .OwnerJson = &reader, .Key = {} }, false, 0u);
				}
			};
			if (record->Metadata.Type == AssetType::Material || record->Metadata.Importer == SoundEffectImporter::Id)
			{
				Utils::EditorSectionHeading("Properties");
				if (!properties.Error.empty())
				{
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.48f, 0.48f, 1.0f));
					ImGui::TextWrapped("These properties cannot be read: %s", properties.Error.c_str());
					ImGui::PopStyleColor();
					ImGui::TextDisabled("Fix the file, or reimport it from Assets.");
				}
				else if (properties.Type != nullptr)
					drawObject(*properties.Type, properties.Values, InspectorTargetKind::AssetProperties);
			}
			if (properties.Settings != nullptr && ImGui::CollapsingHeader("Import settings"))
			{
				ImGui::PushID("ImportSettings");
				drawObject(*properties.Settings, record->Metadata.Settings.Get(), InspectorTargetKind::AssetImportSettings);
				ImGui::PopID();
			}
			finishFrame();
			return {};
		}
		if (scene == nullptr || selection.empty())
		{
			CancelEdit(context);
			Utils::EditorEmptyState("Nothing selected", "Select an object in the Hierarchy or a resource in Assets to view and edit its properties.");
			return {};
		}
		Entity first = scene->FindEntityByID(selection.front());
		const char* targetName = sceneTarget == SceneTarget::Play ? "Play" : "Edit";
		// Runtime content carries the play accent, so transient edits never look like edit-scene ones (§5.6).
		if (runtime)
			Utils::BeginEditorPlayTint();
		ImGui::BeginDisabled(editor.IsReadOnly());
		if (selection.size() == 1)
		{
			ImGui::PushID(static_cast<int>(FNV1a32(first.GetUUID().ToString() + ":" + m_OwnerIdentity)));
			bool enabled = first.IsActiveSelf();
			if (ImGui::Checkbox("##EntityActive", &enabled))
				queue("entity.update", Json{ { "entity", first.GetUUID().ToString() }, { "active", enabled }, { "target", targetName } });
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("Active in the scene");
			ImGui::SameLine();
			std::string name = first.GetName();
			ImGui::SetNextItemWidth(-1.0f);
			const bool entered = ImGui::InputText("##EntityName", &name, ImGuiInputTextFlags_EnterReturnsTrue);
			if (entered && name != first.GetName())
				queue("entity.update", Json{ { "entity", first.GetUUID().ToString() }, { "name", name }, { "target", targetName } });
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("Object name - Enter to rename");
			ImGui::PopID();
			ImGui::TextDisabled("%s", first.HasComponent<PrefabLinkComponent>() ? "Prefab instance" : "Scene object");
		}
		else
		{
			ImGui::Text("%zu objects selected", selection.size());
			ImGui::TextWrapped("Edit shared components together. Mixed properties show Multiple values.");
		}
		ImGui::EndDisabled();
		ImGui::Spacing();
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
			ImGui::PushID(static_cast<int>(FNV1a32(component->GetName())));
			bool expanded = false;
			bool openOptions = false;
			if (ImGui::BeginTable("ComponentHeader", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoPadOuterX))
			{
				ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("Options", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				expanded = ImGui::TreeNodeEx("##Component", ImGuiTreeNodeFlags_CollapsingHeader | ImGuiTreeNodeFlags_DefaultOpen,
					"%s", Utils::EditorLabel(component->GetName()).c_str());
				if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
					openOptions = true;
				ImGui::TableNextColumn();
				if (ImGui::Button("...##Options", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())))
					openOptions = true;
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Component options");
				ImGui::EndTable();
			}
			if (openOptions)
				ImGui::OpenPopup("ComponentOptions");
			if (ImGui::BeginPopup("ComponentOptions"))
			{
				ImGui::TextUnformatted(Utils::EditorLabel(component->GetName()).c_str());
				ImGui::Separator();
				if (ImGui::MenuItem("Remove component", nullptr, false, !editor.IsReadOnly() && component->HasFlag(ComponentFlags::Removable)))
				{
					CancelEdit(context);
					Json commands = Json::array();
					for (const UUID id : selection)
						commands.push_back(Json{ { "method", "entity.update" }, { "params", Json{ { "entity", id.ToString() }, { "removeComponents", Json::array({ component->GetName() }) }, { "target", targetName } } } });
					queue("edit.batch", Json{ { "label", "Remove component" }, { "ops", std::move(commands) } });
				}
				ImGui::EndPopup();
			}
			if (expanded)
			{
				const void* object = ops->GetConst(first);
				ResolveContext resolve{ .Registry = &editor.GetTypeRegistry(), .Owner = object, .OwnerType = component, .Key = {} };
				std::vector<const FieldInfo*> fields;
				const bool transform = component->GetName() == "Transform";
				if (transform)
				{
					for (const char* name : { "Translation", "EulerAngles", "Scale" })
					{
						if (const FieldInfo* field = component->FindField(name))
							fields.push_back(field);
					}
				}
				const size_t primaryFields = fields.size();
				for (const auto& field : component->GetFields())
				{
					if (std::find(fields.begin(), fields.end(), field.get()) == fields.end())
						fields.push_back(field.get());
				}
				for (size_t fieldIndex = 0; fieldIndex < fields.size(); ++fieldIndex)
				{
					if (transform && fieldIndex == primaryFields && !ImGui::CollapsingHeader("Advanced transform"))
						break;
					const FieldInfo* field = fields[fieldIndex];
					if (field->GetMeta().Hidden)
						continue;
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
							ImGui::TextWrapped("Assign a Behaviour script to edit its exposed properties.");
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
							ImGui::TextWrapped("Select objects with the same Behaviour to edit script fields together.");
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
							uint32_t mixedComponents = 0;
							for (const UUID id : selection)
							{
								const VariantValue other = effective(scene->FindEntityByID(id).GetComponent<ScriptComponent>(), invalid);
								if (other.Get() != initial.Get())
								{
									mixed = true;
									mixedComponents |= Utils::InspectorComponentDifferences(Value::FromVariant(initial), Value::FromVariant(other));
								}
							}
							if (invalid)
								ImGui::TextWrapped("%s uses its default until its saved value is corrected.", Utils::EditorLabel(declaration.Name).c_str());
							FieldInfo variant({ .Name = declaration.Name, .Description = (*descriptor)->GetDescription(), .Type = field->GetType().GetElement(), .Meta = declaration.Meta, .Accessor = {}, .Resolver = field->GetResolver() });
							scriptResolve.Key = declaration.Name;
							drawField(variant, Value::FromVariant(initial), { .Entities = selection, .Component = "Script", .FieldPath = "Fields[" + declaration.Name + "]", .Target = sceneTarget },
								scriptResolve, mixed, mixedComponents, &declaration);
						}
						for (const auto& [name, value] : attached.Fields)
						{
							if (!(*schemas)->FindField(attached.Script.GetHandle(), name))
								ImGui::TextWrapped("Saved field not in this script: %s = %s (preserved)", name.c_str(), value.Get().dump().c_str());
						}
					}
					else
					{
						auto value = ComponentAccess::GetFieldValue(first, component->GetName(), field->GetName());
						if (!value)
							continue;
						bool mixed = false;
						uint32_t mixedComponents = 0;
						for (const UUID id : selection)
						{
							auto other = ComponentAccess::GetFieldValue(scene->FindEntityByID(id), component->GetName(), field->GetName());
							if (!other || *other != *value)
							{
								mixed = true;
								mixedComponents |= other ? Utils::InspectorComponentDifferences(*value, *other) : Utils::AllInspectorComponents;
							}
						}
						drawField(*field, std::move(*value), { .Entities = selection, .Component = component->GetName(), .FieldPath = field->GetName(), .Target = sceneTarget }, resolve, mixed, mixedComponents);
					}
					if (selection.size() == 1 && first.HasComponent<PrefabLinkComponent>() && sceneTarget == SceneTarget::Edit)
					{
						const auto link = first.GetComponent<PrefabLinkComponent>();
						const ConstEntity root = scene->FindEntityByID(link.InstanceRoot);
						if (root.IsValid() && root.HasComponent<PrefabInstanceComponent>())
						{
							const auto& overrides = root.GetComponent<PrefabInstanceComponent>().Overrides;
							const std::string prefabField = transform && field->GetName() == "EulerAngles" ? "Rotation" : field->GetName();
							const bool overridden = std::any_of(overrides.begin(), overrides.end(), [&link, component, &prefabField](const PrefabOverride& item)
							{
								return item.Path == std::vector<UUID>{ link.PrefabEntityID } && item.Component == component->GetName() && item.Field == prefabField;
							});
							if (overridden)
							{
								ImGui::PushID(field->GetName().c_str());
								ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "Prefab override");
								ImGui::SameLine();
								ImGui::BeginDisabled(editor.IsReadOnly());
								if (ImGui::SmallButton("Revert"))
								{
									CancelEdit(context);
									queue("prefab.revert", Json{ { "instance", link.InstanceRoot.ToString() }, { "overrides", Json::array({ Json{ { "path", Json::array({ link.PrefabEntityID.ToString() }) }, { "kind", "Field" }, { "component", component->GetName() }, { "field", prefabField } } }) } });
								}
								ImGui::EndDisabled();
								ImGui::PopID();
							}
						}
					}
				}
			}
			ImGui::PopID();
		}
		ImGui::Spacing();
		ImGui::BeginDisabled(editor.IsReadOnly());
		if (ImGui::Button("Add component", ImVec2(-1.0f, 0.0f)))
		{
			m_ComponentSearch.clear();
			ImGui::OpenPopup("AddComponent");
		}
		ImGui::SetNextWindowSize(ImVec2(std::min(ImGui::GetFontSize() * 24.0f, ImGui::GetIO().DisplaySize.x - 32.0f), 0.0f), ImGuiCond_Appearing);
		if (ImGui::BeginPopup("AddComponent"))
		{
			if (ImGui::IsWindowAppearing())
				ImGui::SetKeyboardFocusHere();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputTextWithHint("##ComponentSearch", "Search components", &m_ComponentSearch);
			std::vector<const ComponentInfo*> available;
			for (const ComponentInfo* component : editor.GetTypeRegistry().GetComponents())
			{
				const ComponentHostOps* ops = component->GetHostOps();
				if (!component->HasFlag(ComponentFlags::EditorVisible) || component->HasFlag(ComponentFlags::Hidden)
					|| component->HasFlag(ComponentFlags::EntityLevel) || ops == nullptr)
					continue;
				if (!Utils::InspectorReferenceMatches(Utils::EditorLabel(component->GetName()), m_ComponentSearch)
					&& !Utils::InspectorReferenceMatches(component->GetName(), m_ComponentSearch)
					&& !Utils::InspectorReferenceMatches(component->GetCategory(), m_ComponentSearch))
					continue;
				if (std::all_of(selection.begin(), selection.end(), [scene, ops](UUID id)
				{
					return ops->Has(scene->FindEntityByID(id));
				}))
					continue;
				available.push_back(component);
			}
			std::sort(available.begin(), available.end(), [](const auto* left, const auto* right)
			{
				return std::tie(left->GetCategory(), left->GetName()) < std::tie(right->GetCategory(), right->GetName());
			});
			if (available.empty())
				ImGui::TextWrapped("No matching components to add.");
			if (ImGui::BeginChild("Components", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 14.0f)))
			{
				std::string category;
				for (const ComponentInfo* component : available)
				{
					if (category != component->GetCategory())
					{
						category = component->GetCategory();
						Utils::EditorSectionHeading(Utils::EditorLabel(category));
					}
					ImGui::PushID(static_cast<int>(FNV1a32(component->GetName())));
					if (ImGui::Selectable(Utils::EditorLabel(component->GetName()).c_str()))
					{
						CancelEdit(context);
						Json commands = Json::array();
						for (const UUID id : selection)
						{
							if (!component->GetHostOps()->Has(scene->FindEntityByID(id)))
								commands.push_back(Json{ { "method", "entity.update" }, { "params", Json{ { "entity", id.ToString() }, { "components", Json{ { component->GetName(), Json::object() } } }, { "target", targetName } } } });
						}
						queue("edit.batch", Json{ { "label", "Add component" }, { "ops", std::move(commands) } });
						ImGui::CloseCurrentPopup();
					}
					ImGui::PopID();
				}
			}
			ImGui::EndChild();
			ImGui::EndPopup();
		}
		ImGui::EndDisabled();
		if (runtime)
			Utils::EndEditorPlayTint();
		finishFrame();
		return {};
	}

}
