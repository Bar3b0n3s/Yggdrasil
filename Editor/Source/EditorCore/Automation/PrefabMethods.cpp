#include "EditorPCH.h"
#include "EditorCore/Automation/PrefabMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/AssetMethodSupport.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/Commands/AssetEditCommand.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Private/EditorFileError.h"
#include "EditorCore/Private/PrefabInstances.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/Importers/PrefabImporter.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/ComponentInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/Value.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/PrefabAsset.h"
#include "Engine/Scene/PrefabInstantiator.h"
#include "Engine/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>

namespace Engine {

	namespace {

		// The instance root an "instance" param names. Errors: those of ResolveEntity; InvalidArgument at "/instance" for an
		// entity that is not an instance root.
		Result<Entity> ResolveInstanceRoot(const EditorMethodContext& context, Scene& scene, std::string_view reference)
		{
			ENGINE_TRY_ASSIGN(Entity root, context.ResolveEntity(scene, reference, "/instance"));
			if (!root.HasComponent<PrefabInstanceComponent>())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/instance",
					std::format("'{}' is not the root of a prefab instance", scene.GetEntityPath(root)),
					"pass the root of an instance (the entity prefab.instantiate created)"));
			}
			return root;
		}

		// The prefab asset the instance root `root` names, at its current version. Errors: NotFound at "/instance" for a
		// missing prefab (PREFAB_MISSING_ASSET); those of LoadPrefabAsset.
		Result<Prefab> LoadInstancePrefab(EditorContext& editor, const Scene& scene, ConstEntity root)
		{
			const AssetHandle handle = root.GetComponent<PrefabInstanceComponent>().Prefab.GetHandle();
			if (!handle.IsValid())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/instance",
					std::format("the instance '{}' names no prefab asset", scene.GetEntityPath(root)), "unpack it with prefab.unpack"));
			}
			LoadReport report;
			Result<Prefab> prefab = LoadPrefabAsset(editor.GetAssets(), handle, editor.GetTypeRegistry(), report);
			if (!prefab && prefab.error().GetCode() == ErrorCode::NotFound)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/instance",
					std::format("the prefab asset {} of the instance '{}' is missing (PREFAB_MISSING_ASSET)", handle.ToString(), scene.GetEntityPath(root)),
					"restore the prefab (edit.undo after asset.delete), or unpack the instance with prefab.unpack"));
			}
			return prefab;
		}

		// Writes the (partial) Transform value `value` onto `entity`: stored fields in one write, then writable virtual fields
		// (EulerAngles, WorldPosition) through their setters, like entity.update. Errors located below `pointer`.
		Status WriteTransform(Entity entity, const Json& value, std::string_view pointer)
		{
			if (!value.is_object())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, "the transform must be an object of Transform fields",
					"for example {\"Translation\": [2, 0, 0]}"));
			}
			const ComponentInfo* info = entity.GetScene()->GetTypeRegistry().FindComponent("Transform");
			ENGINE_ASSERT(info != nullptr, "The Transform component is registered by every engine context");
			Json stored = Json::object();
			std::vector<std::pair<const FieldInfo*, const Json*>> virtuals;
			for (auto member = value.begin(); member != value.end(); ++member)
			{
				const FieldInfo* field = info->FindField(member.key());
				if (field != nullptr && field->IsVirtual())
					virtuals.emplace_back(field, &member.value());
				else
					stored[member.key()] = member.value();
			}
			if (!stored.empty())
			{
				if (Status patched = ComponentAccess::PatchComponentJson(entity, info->GetName(), stored); !patched)
					return std::unexpected(Utils::PrefixPointers(patched.error(), pointer));
			}
			for (const auto& [field, json] : virtuals)
			{
				const std::string fieldPointer = JsonReader::AppendPointer(pointer, field->GetName());
				Result<Value> fieldValue = ValueFromJson(JsonReader(*json), field->GetType());
				if (!fieldValue)
					return std::unexpected(Utils::PrefixPointers(fieldValue.error(), fieldPointer));
				if (Status set = ComponentAccess::SetFieldValue(entity, info->GetName(), field->GetName(), *fieldValue); !set)
					return std::unexpected(Utils::PrefixPointers(set.error(), pointer));
			}
			return {};
		}

		// The registry spelling of an override kind given in any ASCII case ("" for an empty one). Errors: InvalidArgument at
		// `pointer` for an unknown kind.
		Result<std::string> ParseOverrideKind(std::string_view kind, std::string_view pointer)
		{
			if (kind.empty())
				return std::string();
			constexpr std::array<std::string_view, 4> Kinds = { "Field", "AddComponent", "RemoveComponent", "EntityKey" };
			for (const std::string_view name : Kinds)
			{
				if (name.size() == kind.size() && std::equal(name.begin(), name.end(), kind.begin(), [](char left, char right)
				{
					return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
				}))
				{
					return std::string(name);
				}
			}
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("'{}' is not an override kind", kind),
				"use Field, AddComponent, RemoveComponent or EntityKey, or leave it empty"));
		}

		// The registry spelling of `kind`.
		std::string_view OverrideKindToString(PrefabOverrideKind kind)
		{
			switch (kind)
			{
				case PrefabOverrideKind::Field:           return "Field";
				case PrefabOverrideKind::AddComponent:    return "AddComponent";
				case PrefabOverrideKind::RemoveComponent: return "RemoveComponent";
				case PrefabOverrideKind::EntityKey:       return "EntityKey";
			}
			return "Field";
		}

		// One parsed PrefabOverrideKey.
		struct OverrideSelector
		{
			UUID PrefabEntityID{};
			std::string Kind{}; // empty: any
			std::string Component{};
			std::string Field{};

			[[nodiscard]] bool Matches(const PrefabOverride& entry) const
			{
				return entry.PrefabEntityID == PrefabEntityID && (Kind.empty() || Kind == OverrideKindToString(entry.Kind))
					&& (Component.empty() || Component == entry.Component) && (Field.empty() || Field == entry.Field);
			}
		};

		Result<std::vector<OverrideSelector>> ParseOverrideKeys(std::span<const PrefabOverrideKey> keys)
		{
			std::vector<OverrideSelector> selectors;
			selectors.reserve(keys.size());
			for (size_t index = 0; index < keys.size(); ++index)
			{
				const PrefabOverrideKey& key = keys[index];
				const std::string pointer = std::format("/overrides/{}", index);
				const std::optional<UUID> id = key.PrefabEntityId.size() == UUID::TextLength ? UUID::FromString(key.PrefabEntityId) : std::nullopt;
				if (!id.has_value() || !id->IsValid())
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer + "/prefabEntityId",
						std::format("'{}' is not a prefab-local entity id", key.PrefabEntityId), "give the 16 hex digits of the override's PrefabEntityID"));
				}
				ENGINE_TRY_ASSIGN(std::string kind, ParseOverrideKind(key.Kind, pointer + "/kind"));
				selectors.push_back(OverrideSelector{ .PrefabEntityID = *id, .Kind = std::move(kind), .Component = key.Component, .Field = key.Field });
			}
			return selectors;
		}

	}

	namespace Automation {

		Result<PrefabCreateResult> PrefabCreate(EditorMethodContext& context, const PrefabCreateParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(SceneTarget::Edit, false, true));
			ENGINE_TRY_ASSIGN(Entity entity, context.ResolveEntity(*scene, params.Entity, "/entity"));
			ENGINE_TRY_ASSIGN(const VfsPath path, Utils::ResolveAssetsPath(context, params.Path, "/path", ".prefab"));
			ENGINE_TRY(Utils::RefreshAssets(editor));
			VirtualFileSystem& vfs = editor.GetVfs();
			ENGINE_TRY(Utils::CheckAssetPathFree(vfs, path, "/path"));
			ENGINE_TRY_ASSIGN(const auto schemas, editor.GetScriptSchemaSnapshot());
			const PrefabOptions prefabOptions{ .Schemas = schemas.get() };

			// Nested instances are flattened (§5.5); the entities keep their ids as prefab-local ids.
			const std::string name(path.GetStem());
			ENGINE_TRY_ASSIGN(const Prefab prefab, Prefab::CreateFromEntity(entity, name));
			ENGINE_TRY_ASSIGN(const std::string text, prefab.SaveToString());
			EditorAssetManager& assets = editor.GetAssets();
			ENGINE_TRY_ASSIGN(const AssetMetadata metadata, assets.CreateMetadata(path));
			const std::string metaText = SerializeAssetMetadata(metadata);
			ENGINE_TRY_ASSIGN(const VfsPath metaPath, GetMetaPath(path));
			std::vector<AssetFileEdit> edits;
			Utils::AddMissingDirectoryEdits(vfs, path.GetParent(), edits);
			const std::span<const std::byte> textBytes = AsBytes(text);
			const std::span<const std::byte> metaBytes = AsBytes(metaText);
			edits.push_back(AssetFileEdit{ .Path = path, .Kind = AssetFileKind::File, .Before = std::nullopt, .After = Buffer(textBytes.begin(), textBytes.end()) });
			edits.push_back(AssetFileEdit{ .Path = metaPath, .Kind = AssetFileKind::File, .Before = std::nullopt, .After = Buffer(metaBytes.begin(), metaBytes.end()) });

			PrefabCreateResult result;
			result.Prefab = AssetSummary{ .Id = metadata.Handle.ToString(), .Path = Utils::ToProjectRelative(path), .Type = AssetType::Prefab };
			const std::string label = std::format("Create Prefab '{}'", name);
			if (!params.ReplaceWithInstance)
			{
				ENGINE_TRY_ASSIGN(const uint64_t undoIndex, editor.Execute(CreateScope<AssetEditCommand>(label, std::move(edits))));
				result.UndoIndex = ToAutomationCounter(undoIndex);
				return result;
			}

			// The entity becomes an instance of the new prefab in the same place, with the same root id, so references to it
			// survive; the file and the scene change are one undo step.
			EditorTransaction transaction(editor, label);
			ENGINE_TRY(editor.Execute(CreateScope<AssetEditCommand>(label, std::move(edits))));
			{
				const UUID rootID = entity.GetUUID();
				const Entity parent = entity.GetParent();
				const uint32_t siblingIndex = entity.GetSiblingIndex();
				const TransformComponent transform = entity.GetComponent<TransformComponent>();
				SceneEdit edit(editor, std::format("Replace '{}' With a Prefab Instance", entity.GetName()));
				scene->DestroyEntity(entity);
				LoadReport report;
				ENGINE_TRY_ASSIGN(const Entity instance, PrefabInstantiator::Instantiate(*scene, prefab, { .PrefabHandle = metadata.Handle, .RootID = rootID, .Parent = parent, .SiblingIndex = siblingIndex, .RootTransform = transform }, prefabOptions, report));
				Utils::LogLoadDiagnostics(result.Prefab.Path, report);
				result.Instance = context.MakeEntitySummary(instance);
				ENGINE_TRY_ASSIGN(const uint64_t editIndex, edit.Commit());
				static_cast<void>(editIndex);
			}
			result.UndoIndex = ToAutomationCounter(transaction.Commit());
			return result;
		}

		Result<PrefabInstantiateResult> PrefabInstantiate(EditorMethodContext& context, const PrefabInstantiateParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Prefab, "/prefab"));
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(SceneTarget::Edit, false, true));
			Entity parent;
			if (!params.Parent.empty())
			{
				ENGINE_TRY_ASSIGN(parent, context.ResolveEntity(*scene, params.Parent, "/parent"));
			}
			const bool hasTransform = context.HasParam("transform") && !params.Transform.Get().is_null();
			if (hasTransform && !params.Transform.Get().is_object())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/transform", "the transform must be an object of Transform fields",
					"for example {\"Translation\": [2, 0, 0]}"));
			}
			const bool hasName = context.HasParam("name");
			if (hasName && params.Name.empty())
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/name", "an entity name must not be empty"));

			EditorAssetManager& assets = editor.GetAssets();
			ENGINE_TRY_ASSIGN(const auto schemas, editor.GetScriptSchemaSnapshot());
			const PrefabOptions prefabOptions{ .Schemas = schemas.get() };
			SceneEdit edit(editor, std::format("Instantiate Prefab '{}'", assets.GetReferencePath(handle)));
			LoadReport report;
			const PrefabInstantiateOptions instance{ .PrefabHandle = handle,
				.RootID = editor.GetIdGenerator().Next(),
				.Parent = parent,
				.SiblingIndex = context.HasParam("index") ? std::optional<uint32_t>(params.Index) : std::nullopt,
				.RootTransform = std::nullopt };
			Result<Entity> created = InstantiatePrefabAsset(*scene, assets, instance, prefabOptions, report);
			if (!created)
			{
				if (created.error().GetCode() == ErrorCode::NotFound)
					return std::unexpected(Utils::LocateAtParam(created.error(), "/prefab"));
				return std::unexpected(std::move(created).error());
			}
			Utils::LogLoadDiagnostics(assets.GetReferencePath(handle), report);
			const Entity root = *created;
			if (hasName)
				root.SetName(params.Name);
			if (hasTransform)
				ENGINE_TRY(WriteTransform(root, params.Transform.Get(), "/transform"));
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, edit.Commit());

			PrefabInstantiateResult result;
			result.Entity = context.MakeEntitySummary(root);
			result.Prefab = Utils::MakeAssetSummary(assets, handle);
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
		}

		Result<PrefabApplyResult> PrefabApply(EditorMethodContext& context, const PrefabApplyParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(SceneTarget::Edit, false, true));
			ENGINE_TRY_ASSIGN(const Entity root, ResolveInstanceRoot(context, *scene, params.Instance));
			ENGINE_TRY(Utils::RefreshAssets(editor));
			ENGINE_TRY_ASSIGN(const Prefab current, LoadInstancePrefab(editor, *scene, root));
			const AssetHandle handle = root.GetComponent<PrefabInstanceComponent>().Prefab.GetHandle();
			EditorAssetManager& assets = editor.GetAssets();
			const AssetRecord* record = assets.GetRegistry().Find(handle);
			if (record == nullptr || record->Metadata.Importer != PrefabImporter::Id)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/instance",
					std::format("the prefab '{}' of '{}' is imported, not a .prefab file, and cannot be written", assets.GetReferencePath(handle),
						scene->GetEntityPath(root)),
					"create a prefab from the instance with prefab.create, or unpack it with prefab.unpack"));
			}
			const VfsPath source = record->SourcePath;
			ENGINE_TRY_ASSIGN(const auto schemas, editor.GetScriptSchemaSnapshot());
			const PrefabOptions prefabOptions{ .Schemas = schemas.get() };
			ENGINE_TRY_ASSIGN(const Prefab applied, PrefabInstantiator::ApplyOverrides(root, current, prefabOptions));
			ENGINE_TRY_ASSIGN(const std::string text, applied.SaveToString());
			Result<std::string> before = editor.GetVfs().ReadText(source);
			if (!before)
				return std::unexpected(Utils::ToEditorFileError(std::move(before).error()));

			// One undo step (§5.5 "Update"): the overrides of every instance recorded against the current version, the .prefab
			// written and imported, and every instance rebuilt from the new version.
			const std::string label = std::format("Apply Prefab '{}'", Utils::ToProjectRelative(source));
			EditorTransaction transaction(editor, label);
			ENGINE_TRY(Utils::RecordPrefabOverrides(editor, handle, current));
			if (text != *before)
			{
				ENGINE_TRY_ASSIGN(Scope<AssetEditCommand> command, AssetEditCommand::CreateForWrite(editor, source, AsBytes(text), label));
				ENGINE_TRY(editor.Execute(std::move(command)));
				ENGINE_TRY_ASSIGN(const AssetImportOutcome outcome, assets.Reimport(handle));
				static_cast<void>(outcome);
			}
			const size_t instances = Utils::FindPrefabInstances(editor.GetScene(), std::span<const AssetHandle>(&handle, 1)).size();
			const AssetHandle prefabs[] = { handle };
			ENGINE_TRY_ASSIGN(Scope<Command> update, editor.CreatePrefabUpdateCommand(prefabs));
			if (update != nullptr)
				ENGINE_TRY(editor.Execute(std::move(update)));

			PrefabApplyResult result;
			result.Prefab = Utils::MakeAssetSummary(assets, handle);
			result.UpdatedInstances = ToAutomationCounter(instances);
			result.UndoIndex = ToAutomationCounter(transaction.Commit());
			return result;
		}

		Result<PrefabRevertResult> PrefabRevert(EditorMethodContext& context, const PrefabRevertParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(SceneTarget::Edit, false, true));
			ENGINE_TRY_ASSIGN(Entity root, ResolveInstanceRoot(context, *scene, params.Instance));
			const bool selective = context.HasParam("overrides");
			std::vector<OverrideSelector> selectors;
			if (selective)
			{
				ENGINE_TRY_ASSIGN(selectors, ParseOverrideKeys(params.Overrides));
			}
			ENGINE_TRY(Utils::RefreshAssets(editor));
			ENGINE_TRY_ASSIGN(const Prefab prefab, LoadInstancePrefab(editor, *scene, root));
			ENGINE_TRY_ASSIGN(const auto schemas, editor.GetScriptSchemaSnapshot());
			const PrefabOptions prefabOptions{ .Schemas = schemas.get() };

			// The overrides the instance has now, derived by diffing (ADR 0006 decision 17), whatever was recorded before.
			ENGINE_TRY_ASSIGN(const std::vector<PrefabOverride> overrides, PrefabInstantiator::ComputeOverrides(root, prefab, prefabOptions));
			SceneEdit edit(editor, std::format("Revert Prefab Instance '{}'", root.GetName()));
			LoadReport report;
			size_t removed = overrides.size();
			if (!selective)
			{
				ENGINE_TRY(PrefabInstantiator::Revert(*scene, root, prefab, prefabOptions, report));
			}
			else
			{
				std::vector<PrefabOverride> kept;
				for (const PrefabOverride& entry : overrides)
				{
					const bool selected = std::ranges::any_of(selectors, [&entry](const OverrideSelector& selector)
					{
						return selector.Matches(entry);
					});
					if (!selected)
						kept.push_back(entry);
				}
				removed = overrides.size() - kept.size();
				root.Patch<PrefabInstanceComponent>([&kept](PrefabInstanceComponent& instance)
				{
					instance.Overrides = std::move(kept);
				});
				ENGINE_TRY(PrefabInstantiator::UpdateInstance(*scene, root, prefab, prefabOptions, report));
			}
			Utils::LogLoadDiagnostics(scene->GetEntityPath(root), report);
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, edit.Commit());

			PrefabRevertResult result;
			result.Instance = context.MakeEntitySummary(root);
			result.RemovedOverrides = ToAutomationCounter(removed);
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
		}

		Result<PrefabUnpackResult> PrefabUnpack(EditorMethodContext& context, const PrefabUnpackParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(SceneTarget::Edit, false, true));
			ENGINE_TRY_ASSIGN(const Entity root, ResolveInstanceRoot(context, *scene, params.Instance));
			SceneEdit edit(editor, std::format("Unpack Prefab Instance '{}'", root.GetName()));
			ENGINE_TRY(PrefabInstantiator::Unpack(root));
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, edit.Commit());
			PrefabUnpackResult result;
			result.Entity = context.MakeEntitySummary(root);
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
		}

	}

	void RegisterPrefabMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<PrefabOverrideKey>("PrefabOverrideKey", "Selects overrides of a prefab instance; an empty kind, component or field matches any.")
			.Field("prefabEntityId", &PrefabOverrideKey::PrefabEntityId, "The prefab-local id of the entity the override applies to: 16 hex digits.")
			.Field("kind", &PrefabOverrideKey::Kind, "Field, AddComponent, RemoveComponent or EntityKey (any case); empty matches every kind.")
			.Field("component", &PrefabOverrideKey::Component, "The component's registry name; empty matches every component.")
			.Field("field", &PrefabOverrideKey::Field, "The field (or entity key) name; empty matches every field.");

		registry.Struct<PrefabCreateParams>("PrefabCreateParams", "The params of prefab.create.")
			.Field("entity", &PrefabCreateParams::Entity, "The root of the subtree to save: 16 hex digits, a unique id prefix or a path.")
			.Field("path", &PrefabCreateParams::Path, "The new .prefab file, project-relative below Assets/.")
			.Field("replaceWithInstance", &PrefabCreateParams::ReplaceWithInstance,
				"Replace the entity with an instance of the new prefab, keeping its id, parent, place and transform.");

		registry.Struct<PrefabCreateResult>("PrefabCreateResult", "The created prefab.")
			.Field("prefab", &PrefabCreateResult::Prefab, "The new prefab asset.")
			.Field("instance", &PrefabCreateResult::Instance, "The instance root with replaceWithInstance; empty otherwise.")
			.Field("undoIndex", &PrefabCreateResult::UndoIndex, "The undo index; 0 in a dry run or a batch.");

		registry.Struct<PrefabInstantiateParams>("PrefabInstantiateParams", "The params of prefab.instantiate.")
			.Field("prefab", &PrefabInstantiateParams::Prefab, "The prefab asset: 16 hex digits, a path such as \"Assets/Prefabs/Cell.prefab\", or a glTF.")
			.Field("parent", &PrefabInstantiateParams::Parent, "The parent entity; empty for a root.")
			.Field("index", &PrefabInstantiateParams::Index, "The position among the parent's children; last when absent.")
			.Field("transform", &PrefabInstantiateParams::Transform, "Transform fields over the prefab root's, such as {\"Translation\": [2, 0, 0]}.")
			.Field("name", &PrefabInstantiateParams::Name, "The instance root's name; the prefab root's when absent.");

		registry.Struct<PrefabInstantiateResult>("PrefabInstantiateResult", "The new instance.")
			.Field("entity", &PrefabInstantiateResult::Entity, "The instance root.")
			.Field("prefab", &PrefabInstantiateResult::Prefab, "The prefab asset.")
			.Field("undoIndex", &PrefabInstantiateResult::UndoIndex, "The undo index; 0 in a dry run or a batch.");

		registry.Struct<PrefabApplyParams>("PrefabApplyParams", "The params of prefab.apply.")
			.Field("instance", &PrefabApplyParams::Instance, "The instance root whose overrides become the prefab's.");

		registry.Struct<PrefabApplyResult>("PrefabApplyResult", "The applied prefab.")
			.Field("prefab", &PrefabApplyResult::Prefab, "The prefab asset written.")
			.Field("updatedInstances", &PrefabApplyResult::UpdatedInstances, "The instances in the open scene rebuilt from it, this one included.")
			.Field("undoIndex", &PrefabApplyResult::UndoIndex, "The undo index; 0 in a batch.");

		registry.Struct<PrefabRevertParams>("PrefabRevertParams", "The params of prefab.revert.")
			.Field("instance", &PrefabRevertParams::Instance, "The instance root to revert.")
			.Field("overrides", &PrefabRevertParams::Overrides, "Only the overrides these keys select; every override when absent.");

		registry.Struct<PrefabRevertResult>("PrefabRevertResult", "The reverted instance.")
			.Field("instance", &PrefabRevertResult::Instance, "The instance root.")
			.Field("removedOverrides", &PrefabRevertResult::RemovedOverrides, "The overrides removed.")
			.Field("undoIndex", &PrefabRevertResult::UndoIndex, "The undo index; 0 when nothing changed, in a dry run or a batch.");

		registry.Struct<PrefabUnpackParams>("PrefabUnpackParams", "The params of prefab.unpack.")
			.Field("instance", &PrefabUnpackParams::Instance, "The instance root to unlink from its prefab.");

		registry.Struct<PrefabUnpackResult>("PrefabUnpackResult", "The unpacked entity.")
			.Field("entity", &PrefabUnpackResult::Entity, "The former instance root, now an ordinary entity.")
			.Field("undoIndex", &PrefabUnpackResult::UndoIndex, "The undo index; 0 in a dry run or a batch.");
	}

	void RegisterPrefabMethods(MethodRegistry& methods)
	{
		Json createExample = Json::object();
		createExample["entity"] = "/Cell";
		createExample["path"] = "Assets/Prefabs/Cell.prefab";
		createExample["replaceWithInstance"] = true;
		methods.Add(
			{
				.Name = "prefab.create",
				.Description = "Saves an entity's subtree as a new .prefab asset (nested instances flattened), optionally replacing the entity "
							   "with an instance of it, as one undoable step.",
				.RequiredParams = { "entity", "path" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Turn the cell into a prefab and an instance of it.", .Params = createExample } },
			},
			&Automation::PrefabCreate);

		Json instantiateExample = Json::object();
		instantiateExample["prefab"] = "Assets/Prefabs/Cell.prefab";
		instantiateExample["parent"] = "/Board";
		instantiateExample["transform"] = Json::object();
		instantiateExample["transform"]["Translation"] = Json::array({ 2.0, 0.0, 0.0 });
		methods.Add(
			{
				.Name = "prefab.instantiate",
				.Description = "Creates an instance of a prefab asset (a .prefab or a glTF) under a parent, with an optional transform and "
							   "name, as one undoable step.",
				.RequiredParams = { "prefab" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Place a cell on the board.", .Params = instantiateExample } },
			},
			&Automation::PrefabInstantiate);

		Json applyExample = Json::object();
		applyExample["instance"] = "/Board/Cell";
		methods.Add(
			{
				.Name = "prefab.apply",
				.Description = "Writes an instance's overrides into its .prefab asset and rebuilds every instance of it in the open scene, as "
							   "one undoable step.",
				.RequiredParams = { "instance" },
				.ExposeAsTool = true,
				.Mutates = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Make the cell's edits the prefab's.", .Params = applyExample } },
			},
			&Automation::PrefabApply);

		Json revertExample = Json::object();
		revertExample["instance"] = "/Board/Cell";
		methods.Add(
			{
				.Name = "prefab.revert",
				.Description = "Removes an instance's overrides (all, or those the keys select) and rebuilds it from its prefab; user children "
							   "stay.",
				.RequiredParams = { "instance" },
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Drop every edit of the cell.", .Params = revertExample } },
			},
			&Automation::PrefabRevert);

		Json unpackExample = Json::object();
		unpackExample["instance"] = "/Board/Cell";
		methods.Add(
			{
				.Name = "prefab.unpack",
				.Description = "Unlinks an instance from its prefab: its entities and their data stay as ordinary entities.",
				.RequiredParams = { "instance" },
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Make the cell an ordinary entity.", .Params = unpackExample } },
			},
			&Automation::PrefabUnpack);
	}

}
