#include "EditorPCH.h"
#include "EditorCore/Inspector/ReflectedEditController.h"

#include "EditorCore/Automation/Private/AssetMethodSupport.h"
#include "EditorCore/Commands/AssetEditCommand.h"
#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Private/PrefabInstances.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/AssetPipeline/Importers/SoundEffectImporter.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/JsonSchema.h"
#include "Engine/Reflection/MergePatch.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/PrefabAsset.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Session/PlaySession.h"

#include <algorithm>
#include <charconv>
#include <utility>

namespace Engine {

	namespace Utils {

		class InspectorUserAttribution
		{
		public:
			explicit InspectorUserAttribution(EditorContext& editor)
				: m_Editor(editor)
			{
				if (editor.GetCommandOrigin() == CommandOrigin::Agent)
					m_Previous = editor.GetWriteAttribution();
				editor.SetWriteAttribution(std::nullopt);
			}
			~InspectorUserAttribution() { m_Editor.SetWriteAttribution(std::move(m_Previous)); }
		private:
			EditorContext& m_Editor; // borrowed until this commit finishes
			std::optional<WriteAttribution> m_Previous{};
		};

		struct InspectorValueSnapshot
		{
			const StructInfo* Type = nullptr; // immutable registry schema; registry outlives the controller
			Json Object{};
			VfsPath Path{};
			std::string Bytes{};
			AssetMetadata Metadata{};
		};

		static Result<std::vector<std::string>> SplitInspectorPath(std::string_view path)
		{
			std::vector<std::string> parts;
			while (!path.empty())
			{
				if (path.front() == '.' || path.front() == '/')
				{
					path.remove_prefix(1);
					if (path.empty())
						return MakeError(ErrorCode::InvalidArgument, "empty field path segment");
				}
				if (path.front() == '[')
				{
					const size_t end = path.find(']');
					if (end == std::string_view::npos || end == 1)
						return MakeError(ErrorCode::InvalidArgument, "invalid field path subscript");
					parts.emplace_back(path.substr(1, end - 1));
					path.remove_prefix(end + 1);
				}
				else
				{
					const size_t count = path.find_first_of("./[");
					parts.emplace_back(path.substr(0, count));
					path.remove_prefix(count == std::string_view::npos ? path.size() : count);
				}
				if (parts.back().empty())
					return MakeError(ErrorCode::InvalidArgument, "empty field path segment");
			}
			if (parts.empty())
				return MakeError(ErrorCode::InvalidArgument, "a field path is required");
			return parts;
		}

		// Works on owned JSON only. ResolveContext's owner reader remains alive through each recursive call.
		static Status WalkInspectorValue(const FieldInfo& field, Json& node, std::span<const std::string> path,
			ResolveContext resolve, const Value* replacement, Value& selected, Json& schema)
		{
			if (field.IsReadOnly())
				return MakeError(ErrorCode::Unsupported, "field '{}' is read-only", field.GetName());
			if (field.GetKind() == FieldType::Variant)
			{
				auto resolved = field.ResolveVariant(resolve);
				if (!resolved)
					return MakeError(ErrorCode::Unsupported, "unresolved variant '{}': {}", field.GetName(), resolved.error().GetMessageText());
				if (replacement != nullptr && path.empty() && !replacement->IsNull() && replacement->GetKind() == FieldType::Variant)
				{
					ENGINE_TRY_ASSIGN(Value typed, ValueFromJson(JsonReader(replacement->AsVariant().Get()), (*resolved)->GetType()));
					ENGINE_TRY(WalkInspectorValue(**resolved, node, path, resolve, &typed, selected, schema));
				}
				else
					ENGINE_TRY(WalkInspectorValue(**resolved, node, path, resolve, replacement, selected, schema));
				if (path.empty())
					selected = Value::FromVariant(VariantValue(node));
				return {};
			}
			if (path.empty())
			{
				if (replacement != nullptr)
				{
					ValidationContext validation;
					field.ValidateValue(*replacement, resolve, validation);
					ENGINE_TRY(validation.ToStatus(field.GetName()));
					for (const auto& issue : validation.GetIssues())
					{
						if (issue.Code == VariantUnresolvedCode || issue.Code == VariantSchemaMismatchCode)
							return MakeError(ErrorCode::Unsupported, "{}", issue.Message);
					}
					ENGINE_TRY_ASSIGN(node, ValueToJson(*replacement, field.GetType()));
				}
				ENGINE_TRY_ASSIGN(selected, ValueFromJson(JsonReader(node), field.GetType()));
				schema = JsonSchema::ForField(field);
				return {};
			}
			const std::string& key = path.front();
			const TypeInfo& type = field.GetType();
			if (type.GetKind() == FieldType::Struct)
			{
				const FieldInfo* child = type.GetStruct()->FindField(key);
				auto found = node.find(key);
				if (child == nullptr || found == node.end())
					return MakeError(ErrorCode::NotFound, "no field '{}'", key);
				const Json owner = node;
				const JsonReader reader(owner);
				resolve.Owner = nullptr;
				resolve.OwnerType = type.GetStruct();
				resolve.OwnerJson = &reader;
				resolve.Key = {};
				return WalkInspectorValue(*child, *found, path.subspan(1), resolve, replacement, selected, schema);
			}
			if (type.GetKind() == FieldType::Map || type.GetKind() == FieldType::Array)
			{
				FieldInfo element({ .Name = key, .Description = field.GetDescription(), .Type = type.GetElement(), .Meta = field.GetMeta(), .Accessor = {}, .Resolver = field.GetResolver() });
				if (type.GetKind() == FieldType::Map)
				{
					auto found = node.find(key);
					if (found == node.end())
						return MakeError(ErrorCode::NotFound, "no map key '{}'", key);
					resolve.Key = key;
					return WalkInspectorValue(element, *found, path.subspan(1), resolve, replacement, selected, schema);
				}
				size_t index = 0;
				const auto parsed = std::from_chars(key.data(), key.data() + key.size(), index);
				if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() || index >= node.size())
					return MakeError(ErrorCode::NotFound, "no array element '{}'", key);
				return WalkInspectorValue(element, node[index], path.subspan(1), resolve, replacement, selected, schema);
			}
			return MakeError(ErrorCode::NotFound, "field '{}' has no child '{}'", field.GetName(), key);
		}

		static Result<Scene*> InspectorScene(EditorContext& editor, SceneTarget target)
		{
			if (target == SceneTarget::Edit && editor.HasScene())
				return &editor.GetScene();
			if (target == SceneTarget::Play && editor.GetPlay().GetSession() != nullptr)
				return &editor.GetPlay().GetSession()->GetScene();
			return MakeError(ErrorCode::InvalidState, "inspector target has no scene");
		}

		static Result<InspectorValueSnapshot> ReadInspectorSnapshot(EditorContext& editor, const InspectorEditTarget& target, UUID id)
		{
			InspectorValueSnapshot result;
			const TypeRegistry& types = editor.GetTypeRegistry();
			if (target.Kind == InspectorTargetKind::Component)
			{
				ENGINE_TRY_ASSIGN(Scene * scene, InspectorScene(editor, target.Target));
				Entity entity = scene->FindEntityByID(id);
				if (!entity.IsValid())
					return MakeError(ErrorCode::NotFound, "inspector entity no longer exists");
				const ComponentInfo* component = types.FindComponent(target.Component);
				if (component == nullptr)
					return MakeError(ErrorCode::NotFound, "unknown component '{}'", target.Component);
				if (component->HasFlag(ComponentFlags::Hidden) || component->HasFlag(ComponentFlags::EntityLevel))
					return MakeError(ErrorCode::Unsupported, "component is maintained by the engine");
				result.Type = component;
				ENGINE_TRY_ASSIGN(result.Object, ComponentAccess::GetComponentJson(entity, target.Component));
				for (const auto& field : component->GetFields())
				{
					if (!field->IsVirtual())
						continue;
					ENGINE_TRY_ASSIGN(Value value, ComponentAccess::GetFieldValue(entity, target.Component, field->GetName()));
					ENGINE_TRY_ASSIGN(result.Object[field->GetName()], ValueToJson(value, field->GetType()));
				}
			}
			else if (target.Kind == InspectorTargetKind::ProjectSettings)
			{
				result.Type = types.FindStruct<ProjectSettings>();
				ENGINE_TRY_ASSIGN(result.Object, result.Type->ToJson(&editor.GetProject().GetSettings()));
			}
			else
			{
				const AssetRecord* record = editor.GetAssets().GetRegistry().Find(target.Asset);
				if (record == nullptr)
					return MakeError(ErrorCode::NotFound, "no editable source asset for this handle");
				result.Metadata = record->Metadata;
				result.Path = target.Kind == InspectorTargetKind::AssetImportSettings ? record->MetaPath : record->SourcePath;
				ENGINE_TRY_ASSIGN(result.Bytes, editor.GetVfs().ReadText(result.Path));
				if (target.Kind == InspectorTargetKind::AssetImportSettings)
				{
					ImporterRegistry importers;
					RegisterBuiltinImporters(importers);
					const IAssetImporter* importer = importers.FindById(record->Metadata.Importer);
					if (importer != nullptr)
						result.Type = types.FindStruct(importer->GetSettingsTypeName());
					result.Object = record->Metadata.Settings.Get();
				}
				else if (target.Kind == InspectorTargetKind::AssetProperties)
				{
					if (record->Metadata.Type == AssetType::Material)
					{
						MaterialLoadReport report;
						ENGINE_TRY_ASSIGN(MaterialData data, MaterialFromText(result.Bytes, types, report));
						result.Type = types.FindStruct<MaterialData>();
						ENGINE_TRY_ASSIGN(result.Object, result.Type->ToJson(&data));
					}
					else if (record->Metadata.Importer == SoundEffectImporter::Id)
					{
						SoundEffectLoadReport report;
						ENGINE_TRY_ASSIGN(SoundEffectDescription data, SoundEffectFromText(result.Bytes, types, report));
						result.Type = types.FindStruct<SoundEffectDescription>();
						ENGINE_TRY_ASSIGN(result.Object, result.Type->ToJson(&data));
					}
				}
				else
					return MakeError(ErrorCode::InvalidArgument, "unknown inspector target kind");
			}
			if (result.Type == nullptr)
				return MakeError(ErrorCode::Unsupported, "asset has no editable reflected schema");
			return result;
		}

		static Status ChangeInspectorSnapshot(InspectorValueSnapshot& snapshot, std::span<const std::string> path,
			const Value* replacement, Value& selected, Json& schema)
		{
			const FieldInfo* field = snapshot.Type->FindField(path.front());
			auto found = snapshot.Object.find(path.front());
			if (field == nullptr || found == snapshot.Object.end())
				return MakeError(ErrorCode::NotFound, "no inspector field '{}'", path.front());
			const Json owner = snapshot.Object;
			const JsonReader ownerReader(owner);
			ResolveContext resolve{ .Registry = &snapshot.Type->GetRegistry(), .OwnerType = snapshot.Type, .OwnerJson = &ownerReader, .Key = {} };
			ENGINE_TRY(WalkInspectorValue(*field, *found, path.subspan(1), resolve, replacement, selected, schema));
			if (replacement != nullptr)
			{
				ValidationContext validation;
				field->ValidateJson(JsonReader(*found), resolve, validation);
				ENGINE_TRY(validation.ToStatus(field->GetName()));
				if (!field->IsVirtual())
				{
					Json stored = snapshot.Object;
					for (const auto& member : snapshot.Type->GetFields())
					{
						if (member->IsVirtual())
							stored.erase(member->GetName());
					}
					ObjectPtr object = snapshot.Type->CreateDefault();
					ENGINE_TRY(snapshot.Type->FromJson(object.get(), JsonReader(stored), {}));
				}
			}
			return {};
		}

		// Run the real setters against an isolated scene first, including virtual fields whose validation depends on
		// their owning entity. A failing later target must never trigger a live patch, event or revision increment.
		static Status ValidateInspectorComponentBatch(EditorContext& editor, const InspectorEditTarget& target,
			std::string_view fieldName, std::span<const InspectorValueSnapshot> snapshots, std::span<const InspectorValueSnapshot> before)
		{
			ENGINE_TRY_ASSIGN(Scene * live, InspectorScene(editor, target.Target));
			ENGINE_TRY_ASSIGN(Json serialized, SceneSerializer::ToJson(*live));
			UUIDGenerator ids = UUIDGenerator::CreateDeterministic(0);
			auto copy = Scene::Create({ .Registry = &editor.GetTypeRegistry(), .IdGenerator = &ids });
			LoadReport report;
			ENGINE_TRY(SceneSerializer::FromJson(*copy, serialized, {}, report));
			for (size_t i = 0; i < target.Entities.size(); ++i)
			{
				const auto& snapshot = snapshots[i];
				if (snapshot.Object == before[i].Object)
					continue;
				const FieldInfo* field = snapshot.Type->FindField(fieldName);
				ENGINE_TRY_ASSIGN(Value value, ValueFromJson(JsonReader(snapshot.Object.find(field->GetName()).value()), field->GetType()));
				ENGINE_TRY(ComponentAccess::SetFieldValue(copy->FindEntityByID(target.Entities[i]), target.Component, field->GetName(), value));
			}
			return {};
		}

	}

	struct ReflectedEditController::State
	{
		EditorContext& Editor; // back-reference; editor outlives this controller
		InspectorEditTarget Target{};
		std::vector<std::string> Path{};
		std::vector<Utils::InspectorValueSnapshot> Before{};
		std::vector<Utils::InspectorValueSnapshot> After{};
		Value Preview{};
		uint64_t Revision = 0;
		uint64_t PlaySerial = 0;
		uint64_t PlayRevision = 0;
		uint64_t AssetVersion = 0;
		bool Editing = false;
	};

	ReflectedEditController::ReflectedEditController(EditorContext& context)
		: m_State(CreateScope<State>(context))
	{
	}

	ReflectedEditController::~ReflectedEditController() = default;

	Status ReflectedEditController::Begin(const InspectorEditTarget& target)
	{
		if (IsEditing())
			return MakeError(ErrorCode::InvalidState, "an inspector edit is already active");
		EditorContext& editor = m_State->Editor;
		if (!editor.HasProject())
			return MakeError(ErrorCode::InvalidState, "no project open");
		if (editor.IsReadOnly())
			return MakeError(ErrorCode::PermissionDenied, "the editor is read-only");
		if (target.Kind == InspectorTargetKind::Component && target.Entities.empty())
			return MakeError(ErrorCode::InvalidState, "no entities selected");
		ENGINE_TRY_ASSIGN(auto path, Utils::SplitInspectorPath(target.FieldPath));
		std::vector<Utils::InspectorValueSnapshot> snapshots;
		const std::vector<UUID> ids = target.Kind == InspectorTargetKind::Component ? target.Entities : std::vector<UUID>{ UUID{} };
		Value initial;
		Json firstSchema;
		for (const UUID id : ids)
		{
			ENGINE_TRY_ASSIGN(auto snapshot, Utils::ReadInspectorSnapshot(editor, target, id));
			Value value;
			Json schema;
			ENGINE_TRY(Utils::ChangeInspectorSnapshot(snapshot, path, nullptr, value, schema));
			if (snapshots.empty())
			{
				initial = value;
				firstSchema = schema;
			}
			else if (firstSchema != schema)
				return MakeError(ErrorCode::Unsupported, "selected fields have different schemas");
			snapshots.push_back(std::move(snapshot));
		}
		m_State->Target = target;
		m_State->Path = std::move(path);
		m_State->Before = std::move(snapshots);
		m_State->After = m_State->Before;
		m_State->Preview = std::move(initial);
		m_State->Revision = editor.GetRevision();
		const PlaySession* session = editor.GetPlay().GetSession();
		m_State->PlaySerial = session == nullptr ? 0 : session->GetSerial();
		m_State->PlayRevision = session == nullptr ? 0 : session->GetScene().GetRevision();
		m_State->AssetVersion = editor.GetAssets().GetVersion(target.Asset);
		m_State->Editing = true;
		return {};
	}

	Status ReflectedEditController::Preview(const Value& value)
	{
		if (!IsEditing())
			return MakeError(ErrorCode::InvalidState, "no inspector edit is active");
		auto proposed = m_State->Before;
		for (auto& snapshot : proposed)
		{
			Value selected;
			Json schema;
			ENGINE_TRY(Utils::ChangeInspectorSnapshot(snapshot, m_State->Path, &value, selected, schema));
		}
		if (m_State->Target.Kind == InspectorTargetKind::Component)
			ENGINE_TRY(Utils::ValidateInspectorComponentBatch(m_State->Editor, m_State->Target, m_State->Path.front(), proposed, m_State->Before));
		m_State->After = std::move(proposed);
		m_State->Preview = value;
		return {};
	}

	Result<Value> ReflectedEditController::GetPreview() const
	{
		if (!IsEditing())
			return MakeError(ErrorCode::InvalidState, "no inspector edit is active");
		return m_State->Preview;
	}

	Result<uint64_t> ReflectedEditController::Commit()
	{
		if (!IsEditing())
			return MakeError(ErrorCode::InvalidState, "no inspector edit is active");
		EditorContext& editor = m_State->Editor;
		const auto& target = m_State->Target;
		const PlaySession* session = editor.GetPlay().GetSession();
		if (!editor.HasProject() || editor.GetRevision() != m_State->Revision
			|| (session == nullptr ? 0 : session->GetSerial()) != m_State->PlaySerial
			|| (target.Target == SceneTarget::Play && session != nullptr && session->GetScene().GetRevision() != m_State->PlayRevision)
			|| editor.GetAssets().GetVersion(target.Asset) != m_State->AssetVersion)
			return MakeError(ErrorCode::Conflict, "the inspector target changed during this edit");
		if (editor.IsReadOnly())
			return MakeError(ErrorCode::PermissionDenied, "the editor is read-only");
		for (size_t i = 0; i < m_State->Before.size(); ++i)
		{
			ENGINE_TRY_ASSIGN(auto current, Utils::ReadInspectorSnapshot(editor, target, target.Kind == InspectorTargetKind::Component ? target.Entities[i] : UUID{}));
			const auto& before = m_State->Before[i];
			if (current.Type != before.Type || current.Object != before.Object || current.Bytes != before.Bytes || current.Path != before.Path)
				return MakeError(ErrorCode::Conflict, "the inspector target changed during this edit");
		}
		const std::string label = std::format("Set {}", target.FieldPath);
		Utils::InspectorUserAttribution attribution(editor);
		uint64_t index = 0;
		if (target.Kind == InspectorTargetKind::Component)
		{
			ENGINE_TRY(Utils::ValidateInspectorComponentBatch(editor, target, m_State->Path.front(), m_State->After, m_State->Before));
			ENGINE_TRY_ASSIGN(Scene * scene, Utils::InspectorScene(editor, target.Target));
			SceneEdit edit(editor, label);
			for (size_t i = 0; i < target.Entities.size(); ++i)
			{
				if (m_State->Before[i].Object == m_State->After[i].Object)
					continue;
				const auto& after = m_State->After[i];
				const FieldInfo* field = after.Type->FindField(m_State->Path.front());
				ENGINE_TRY_ASSIGN(Value value, ValueFromJson(JsonReader(after.Object.find(field->GetName()).value()), field->GetType()));
				ENGINE_TRY(ComponentAccess::SetFieldValue(scene->FindEntityByID(target.Entities[i]), target.Component, field->GetName(), value));
			}
			ENGINE_TRY_ASSIGN(index, edit.Commit());
		}
		else if (m_State->Before.front().Object != m_State->After.front().Object)
		{
			const auto& after = m_State->After.front();
			if (target.Kind == InspectorTargetKind::ProjectSettings)
			{
				ENGINE_TRY_ASSIGN(auto command, ProjectSettingsCommand::CreateFromPatch(editor, CreateMergePatch(m_State->Before.front().Object, after.Object), label));
				ENGINE_TRY_ASSIGN(index, editor.Execute(std::move(command)));
			}
			else
			{
				std::string text;
				if (target.Kind == InspectorTargetKind::AssetImportSettings)
				{
					AssetMetadata metadata = after.Metadata;
					metadata.Settings = VariantValue(after.Object);
					text = SerializeAssetMetadata(metadata);
				}
				else if (after.Type == editor.GetTypeRegistry().FindStruct<MaterialData>())
				{
					MaterialData material;
					ENGINE_TRY(after.Type->FromJson(&material, JsonReader(after.Object), { .Strict = true }));
					ENGINE_TRY_ASSIGN(text, MaterialToText(material, editor.GetTypeRegistry()));
				}
				else
				{
					SoundEffectDescription sound;
					ENGINE_TRY(after.Type->FromJson(&sound, JsonReader(after.Object), { .Strict = true }));
					ENGINE_TRY_ASSIGN(text, SoundEffectToText(sound, editor.GetTypeRegistry()));
				}
				ENGINE_TRY_ASSIGN(auto command, AssetEditCommand::CreateForWrite(editor, after.Path, AsBytes(text), label));
				const AssetHandle prefabs[] = { target.Asset };
				if (target.Kind == InspectorTargetKind::AssetImportSettings && editor.HasScene()
					&& !Utils::FindPrefabInstances(editor.GetScene(), prefabs).empty())
				{
					LoadReport report;
					ENGINE_TRY_ASSIGN(const Prefab current, LoadPrefabAsset(editor.GetAssets(), target.Asset, editor.GetTypeRegistry(), report));
					EditorTransaction transaction(editor, label);
					ENGINE_TRY(Utils::RecordPrefabOverrides(editor, target.Asset, current));
					ENGINE_TRY(editor.Execute(std::move(command)));
					ENGINE_TRY(editor.GetAssets().Reimport(target.Asset));
					ENGINE_TRY_ASSIGN(Scope<Command> update, editor.CreatePrefabUpdateCommand(prefabs));
					if (update != nullptr)
						ENGINE_TRY(editor.Execute(std::move(update)));
					index = transaction.Commit();
				}
				else
				{
					ENGINE_TRY_ASSIGN(index, editor.Execute(std::move(command)));
				}
			}
		}
		Cancel();
		return index;
	}

	void ReflectedEditController::Cancel()
	{
		m_State->Editing = false;
		m_State->Before.clear();
		m_State->After.clear();
		m_State->Preview = {};
	}

	bool ReflectedEditController::IsEditing() const
	{
		return m_State->Editing;
	}

}
