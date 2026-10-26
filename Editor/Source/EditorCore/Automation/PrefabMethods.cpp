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
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/AssetPipeline/AssetDependencyGraph.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/Importers/PrefabImporter.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/ComponentInfo.h"
#include "Engine/Reflection/EnumInfo.h"
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

#include <algorithm>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

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

		// The prefab asset the instance root `root` names, at its current version. Errors: NotFound at `pointer` (the param
		// that names the instance) for a missing prefab (PREFAB_MISSING_ASSET); those of LoadPrefabAsset.
		Result<Prefab> LoadInstancePrefab(EditorContext& editor, const Scene& scene, ConstEntity root, std::string_view pointer)
		{
			const AssetHandle handle = root.GetComponent<PrefabInstanceComponent>().Prefab.GetHandle();
			if (!handle.IsValid())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, pointer,
					std::format("the instance '{}' names no prefab asset", scene.GetEntityPath(root)), "unpack it with prefab.unpack"));
			}
			LoadReport report;
			ENGINE_TRY_ASSIGN(const auto schemas, editor.GetScriptSchemaSnapshot());
			Result<Prefab> prefab = LoadPrefabAsset(editor.GetAssets(), handle, editor.GetTypeRegistry(), schemas.get(), report);
			if (!prefab && prefab.error().GetCode() == ErrorCode::NotFound)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, pointer,
					std::format("the prefab asset {} of the instance '{}' is missing (PREFAB_MISSING_ASSET)", handle.ToString(), scene.GetEntityPath(root)),
					"restore the prefab (edit.undo after asset.delete), or unpack the instance with prefab.unpack"));
			}
			return prefab;
		}

		// The instance roots `root` belongs to, from the outermost down to `root` itself (PrefabInstantiator::
		// GetInstanceLevels; decision 8). An outermost instance root (no OuterPrefabEntityID: a top-level instance, or one a
		// user placed under a member of another) is its own only level, without walking the hierarchy above it. A nested
		// root's parent need not belong to the instance enclosing it (a user may move it under a user child of that
		// instance), so a nested root's levels are always looked up.
		Result<std::vector<Entity>> GetLevels(Entity root)
		{
			if (Utils::IsOutermostPrefabInstance(root))
				return std::vector<Entity>{ root };
			ENGINE_TRY_ASSIGN(std::vector<Entity> levels, PrefabInstantiator::GetInstanceLevels(root));
			ENGINE_ASSERT(!levels.empty() && levels.back() == root, "GetInstanceLevels of an instance root ends with that root");
			return levels;
		}

		// The paths of `levels`, outermost first, quoted and comma-separated, for a hint.
		std::string DescribeLevels(const Scene& scene, std::span<const Entity> levels)
		{
			std::string text;
			for (const Entity level : levels)
				text += std::format("{}'{}'", text.empty() ? "" : ", ", scene.GetEntityPath(level));
			return text;
		}

		// The instance whose prefab prefab.apply writes: `level` when given (one of `levels`), the outermost otherwise.
		// Errors: those of ResolveEntity at "/level"; InvalidArgument at "/level" for an entity that is not one of `levels`.
		Result<Entity> ResolveApplyLevel(const EditorMethodContext& context, Scene& scene, std::span<const Entity> levels, std::string_view level)
		{
			if (!context.HasParam("level"))
				return levels.front();
			ENGINE_TRY_ASSIGN(const Entity resolved, context.ResolveEntity(scene, level, "/level"));
			if (std::ranges::find(levels, resolved) != levels.end())
				return resolved;
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/level",
				std::format("'{}' is not a level of the prefab instance '{}'", scene.GetEntityPath(resolved), scene.GetEntityPath(levels.back())),
				std::format("pass one of its levels, outermost first: {}; or leave level out for the outermost", DescribeLevels(scene, levels))));
		}

		// The prefab of `level`, one of `levels` (the levels of the instance root `root`), with the overrides prefab.apply
		// writes back (decision 9): an outermost root applied to its own prefab is the two-argument ApplyOverrides, and every
		// other level re-bases the overrides' paths onto its prefab. Errors: those of PrefabInstantiator::ApplyOverrides.
		Result<Prefab> ApplyToLevel(Entity root, std::span<const Entity> levels, Entity level, const Prefab& levelPrefab, const PrefabOptions& options)
		{
			if (levels.size() == 1)
				return PrefabInstantiator::ApplyOverrides(root, levelPrefab, options);
			return PrefabInstantiator::ApplyOverrides(root, level, levelPrefab, options);
		}

		// `created`, a prefab Prefab::CreateFromEntity wrote, with the record of every instance it keeps live recomputed against
		// the current version of that instance's prefab (RefreshNestedPrefabOverrides; decision 8): CreateFromEntity moves onto
		// it only the overrides its enclosing instance recorded for it, while the document of the prefab that nested it may
		// record more. An instance of a prefab that is not registered keeps the record it was given (PREFAB_MISSING_ASSET).
		// Errors: those of LoadPrefabAsset but NotFound; those of RefreshNestedPrefabOverrides.
		Result<Prefab> RefreshCreatedPrefab(EditorContext& editor, const Prefab& created, const PrefabOptions& options)
		{
			const TypeRegistry& types = editor.GetTypeRegistry();
			const std::vector<AssetHandle> nested = GetNestedPrefabAssets(created, types);
			if (nested.empty())
				return created;
			std::map<AssetHandle, Prefab> versions;
			for (const AssetHandle handle : nested)
			{
				LoadReport report;
				Result<Prefab> version = LoadPrefabAsset(editor.GetAssets(), handle, types, options.Schemas, report);
				if (!version)
				{
					if (version.error().GetCode() == ErrorCode::NotFound)
						continue;
					return std::unexpected(std::move(version).error());
				}
				versions.emplace(handle, std::move(*version));
			}
			return RefreshNestedPrefabOverrides(created, types, versions, options);
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

		// True when `path` starts with `prefix` (equal included).
		bool StartsWith(std::span<const UUID> path, std::span<const UUID> prefix)
		{
			return path.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), path.begin());
		}

		// One parsed PrefabOverrideKey.
		struct OverrideSelector
		{
			std::vector<UUID> Path{};
			std::optional<PrefabOverrideKind> Kind{}; // nullopt: any
			std::string Component{};
			std::string Field{};

			[[nodiscard]] bool Matches(const PrefabOverride& entry) const
			{
				return entry.Path == Path && (!Kind.has_value() || *Kind == entry.Kind) && (Component.empty() || Component == entry.Component)
					&& (Field.empty() || Field == entry.Field);
			}
		};

		// Whether prefab.revert removes `entry`: it lies within `within` (the path of the nested instance root reverted; empty
		// for an outermost root) and, when the call gives keys (`selective`), one of `selectors` matches it.
		bool IsReverted(const PrefabOverride& entry, std::span<const UUID> within, bool selective, std::span<const OverrideSelector> selectors)
		{
			if (!StartsWith(entry.Path, within))
				return false;
			return !selective || std::ranges::any_of(selectors, [&entry](const OverrideSelector& selector)
			{
				return selector.Matches(entry);
			});
		}

		// The kind a PrefabOverrideKey names in any ASCII case, from the registry's PrefabOverrideKind (nullopt for an empty
		// one, which matches every kind). Errors: InvalidArgument at `pointer` for a name the enum does not have.
		Result<std::optional<PrefabOverrideKind>> ParseOverrideKind(const EnumInfo& kinds, std::string_view kind, std::string_view pointer)
		{
			if (kind.empty())
				return std::optional<PrefabOverrideKind>();
			if (const EnumEntry* entry = kinds.FindByNameIgnoreCase(kind))
				return std::optional<PrefabOverrideKind>(static_cast<PrefabOverrideKind>(entry->Value));
			std::string names;
			for (const EnumEntry& entry : kinds.GetEntries())
				names += std::format("{}{}", names.empty() ? "" : ", ", entry.Name);
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("'{}' is not a prefab override kind", kind),
				std::format("use one of {} (any case), or leave it empty to match every kind", names)));
		}

		Result<std::vector<OverrideSelector>> ParseOverrideKeys(const TypeRegistry& registry, std::span<const PrefabOverrideKey> keys)
		{
			const EnumInfo* kinds = registry.FindEnum<PrefabOverrideKind>();
			ENGINE_ASSERT(kinds != nullptr, "PrefabOverrideKind is registered by every engine context");
			constexpr std::string_view PathHint = "give the override's Path as the instance's Prefab component records it (entity.get with components [\"Prefab\"])";
			std::vector<OverrideSelector> selectors;
			selectors.reserve(keys.size());
			for (size_t index = 0; index < keys.size(); ++index)
			{
				const PrefabOverrideKey& key = keys[index];
				const std::string pointer = std::format("/overrides/{}", index);
				if (key.Path.empty())
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer + "/path", "an override path names at least one entity id",
						std::string(PathHint)));
				}
				OverrideSelector selector{ .Path = {}, .Kind = std::nullopt, .Component = key.Component, .Field = key.Field };
				selector.Path.reserve(key.Path.size());
				for (size_t element = 0; element < key.Path.size(); ++element)
				{
					const std::string& text = key.Path[element];
					const std::optional<UUID> id = text.size() == UUID::TextLength ? UUID::FromString(text) : std::nullopt;
					if (!id.has_value() || !id->IsValid())
					{
						return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("{}/path/{}", pointer, element),
							std::format("'{}' is not an entity id of an override path: 16 hex digits", text), std::string(PathHint)));
					}
					selector.Path.push_back(*id);
				}
				ENGINE_TRY_ASSIGN(selector.Kind, ParseOverrideKind(*kinds, key.Kind, pointer + "/kind"));
				selectors.push_back(std::move(selector));
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

			// The entities keep their ids as prefab-local ids, and every instance wholly inside the subtree stays live, outermost
			// in the new prefab, its record recomputed against its prefab (decision 8).
			const std::string name(path.GetStem());
			ENGINE_TRY_ASSIGN(const Prefab created, Prefab::CreateFromEntity(entity, name));
			ENGINE_TRY_ASSIGN(const Prefab prefab, RefreshCreatedPrefab(editor, created, prefabOptions));
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
			// survive; the file and the scene change are one undo step. A member of another instance is replaced like any
			// entity: the commit's override refresh records its deletion as a RemoveEntity on that instance's root, and the new
			// instance stays there as a user child of it (§5.5).
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
			// Per-instance values are ordinary writes to the new root, which the commit records as its overrides (§5.5).
			ENGINE_TRY(Utils::WriteComponents(editor, root, params.Components));
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
			ENGINE_TRY_ASSIGN(const std::vector<Entity> levels, GetLevels(root));
			ENGINE_TRY_ASSIGN(const Entity level, ResolveApplyLevel(context, *scene, levels, params.Level));
			const std::string_view levelPointer = context.HasParam("level") ? "/level" : "/instance";
			ENGINE_TRY(Utils::RefreshAssets(editor));
			ENGINE_TRY_ASSIGN(const Prefab current, LoadInstancePrefab(editor, *scene, level, levelPointer));
			const AssetHandle handle = level.GetComponent<PrefabInstanceComponent>().Prefab.GetHandle();
			EditorAssetManager& assets = editor.GetAssets();
			const AssetRecord* record = assets.GetRegistry().Find(handle);
			if (record == nullptr || record->Metadata.Importer != PrefabImporter::Id)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, levelPointer,
					std::format("the prefab '{}' of '{}' is imported, not a .prefab file, and cannot be written", assets.GetReferencePath(handle),
						scene->GetEntityPath(level)),
					"create a prefab from the instance with prefab.create, or unpack it with prefab.unpack"));
			}
			const VfsPath source = record->SourcePath;
			ENGINE_TRY_ASSIGN(const auto schemas, editor.GetScriptSchemaSnapshot());
			const PrefabOptions prefabOptions{ .Schemas = schemas.get() };
			ENGINE_TRY_ASSIGN(const Prefab applied, ApplyToLevel(root, levels, level, current, prefabOptions));
			ENGINE_TRY_ASSIGN(const std::string text, applied.SaveToString());
			Result<std::string> before = editor.GetVfs().ReadText(source);
			if (!before)
				return std::unexpected(Utils::ToEditorFileError(std::move(before).error()));
			// Before anything is written, so that a nesting cycle refuses the whole update.
			ENGINE_TRY_ASSIGN(const std::vector<AssetHandle> nesting, Utils::FindNestingPrefabs(editor, handle, current));

			// One undo step (§5.5 "Update"): the overrides of every instance recorded against the current versions, the .prefab
			// written and imported, the prefabs nesting it rewritten, and every instance of them rebuilt from the new versions,
			// the other open scenes' in their own histories (decision 15).
			const std::string label = std::format("Apply Prefab '{}'", Utils::ToProjectRelative(source));
			Scope<Command> write;
			if (text != *before)
			{
				ENGINE_TRY_ASSIGN(Scope<AssetEditCommand> command, AssetEditCommand::CreateForWrite(editor, source, AsBytes(text), label));
				write = std::move(command);
			}
			ENGINE_TRY_ASSIGN(const Utils::PrefabVersionUpdate update, Utils::UpdatePrefabVersion(editor, handle, current, std::move(write), nesting, label));

			PrefabApplyResult result;
			result.Prefab = Utils::MakeAssetSummary(assets, handle);
			result.UpdatedInstances = ToAutomationCounter(update.UpdatedInstances);
			result.UpdatedPrefabs.reserve(update.UpdatedPrefabs.size());
			for (const AssetHandle nestingPrefab : update.UpdatedPrefabs)
				result.UpdatedPrefabs.push_back(Utils::MakeAssetSummary(assets, nestingPrefab));
			result.UndoIndex = ToAutomationCounter(update.UndoIndex);
			return result;
		}

		Result<PrefabRevertResult> PrefabRevert(EditorMethodContext& context, const PrefabRevertParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(SceneTarget::Edit, false, true));
			ENGINE_TRY_ASSIGN(const Entity root, ResolveInstanceRoot(context, *scene, params.Instance));
			const bool selective = context.HasParam("overrides");
			std::vector<OverrideSelector> selectors;
			if (selective)
			{
				ENGINE_TRY_ASSIGN(selectors, ParseOverrideKeys(editor.GetTypeRegistry(), params.Overrides));
			}
			// The outermost instance records the overrides and is the one rebuilt; a nested root's overrides are those whose
			// paths start with its own.
			ENGINE_TRY_ASSIGN(const std::vector<Entity> levels, GetLevels(root));
			const Entity outermost = levels.front();
			std::vector<UUID> within;
			if (levels.size() > 1)
			{
				ENGINE_TRY_ASSIGN(within, PrefabInstantiator::GetOverridePath(root));
			}
			ENGINE_TRY(Utils::RefreshAssets(editor));
			ENGINE_TRY_ASSIGN(const Prefab prefab, LoadInstancePrefab(editor, *scene, outermost, "/instance"));
			ENGINE_TRY_ASSIGN(const auto schemas, editor.GetScriptSchemaSnapshot());
			const PrefabOptions prefabOptions{ .Schemas = schemas.get() };

			// The overrides the instance has now, derived by diffing (ADR 0006 decision 17), whatever was recorded before.
			ENGINE_TRY_ASSIGN(const std::vector<PrefabOverride> overrides, PrefabInstantiator::ComputeOverrides(outermost, prefab, prefabOptions));
			SceneEdit edit(editor, std::format("Revert Prefab Instance '{}'", root.GetName()));
			LoadReport report;
			size_t removed = overrides.size();
			if (!selective && within.empty())
			{
				ENGINE_TRY(PrefabInstantiator::Revert(*scene, outermost, prefab, prefabOptions, report));
			}
			else
			{
				std::vector<PrefabOverride> kept;
				for (const PrefabOverride& entry : overrides)
				{
					if (!IsReverted(entry, within, selective, selectors))
						kept.push_back(entry);
				}
				removed = overrides.size() - kept.size();
				outermost.Patch<PrefabInstanceComponent>([&kept](PrefabInstanceComponent& instance)
				{
					instance.Overrides = std::move(kept);
				});
				ENGINE_TRY(PrefabInstantiator::UpdateInstance(*scene, outermost, prefab, prefabOptions, report));
			}
			Utils::LogLoadDiagnostics(scene->GetEntityPath(outermost), report);
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
			// One level at a time (decision 8): a nested instance is unpacked after the instance that encloses it.
			ENGINE_TRY_ASSIGN(const std::vector<Entity> levels, GetLevels(root));
			if (levels.size() > 1)
			{
				const std::string outermost = scene->GetEntityPath(levels.front());
				// The way out as a call to copy, its params built as JSON so that any entity name is escaped.
				const Json unpack{ { "instance", outermost } };
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidState, "/instance",
					std::format("'{}' is nested in the prefab instance '{}', and prefab.unpack unpacks one level at a time from the outermost",
						scene->GetEntityPath(root), outermost),
					std::format("unpack its outermost instance first (prefab.unpack {}), after which this one is an instance of its own",
						unpack.dump(-1, ' ', false, Json::error_handler_t::replace))));
			}
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
		registry.Struct<PrefabOverrideKey>("PrefabOverrideKey", "Selects overrides of a prefab instance by path; an empty kind, component or field matches any.")
			.Field("path", &PrefabOverrideKey::Path, "The override's path, as the instance's Prefab component records it.")
			.Field("kind", &PrefabOverrideKey::Kind, "A PrefabOverrideKind name (any case); empty matches every kind.")
			.Field("component", &PrefabOverrideKey::Component, "The component's registry name; empty matches every component.")
			.Field("field", &PrefabOverrideKey::Field, "The field (or entity key) name; empty matches every field.");

		registry.Struct<PrefabCreateParams>("PrefabCreateParams", "The params of prefab.create.")
			.Field("entity", &PrefabCreateParams::Entity, "The root of the subtree to save: 16 hex digits, a unique id prefix or a path.")
			.Field("path", &PrefabCreateParams::Path, "The new .prefab file, project-relative below Assets/.")
			.Field("replaceWithInstance", &PrefabCreateParams::ReplaceWithInstance,
				"Replace the entity with an instance of the new prefab, keeping its id, parent, place and transform.");

		registry.Struct<PrefabCreateResult>("PrefabCreateResult", "The created prefab.")
			.Field("prefab", &PrefabCreateResult::Prefab, "The new prefab asset.")
			.Field("instance", &PrefabCreateResult::Instance,
				"The instance root with replaceWithInstance; empty otherwise (prefab.instantiate's result calls its root entity).")
			.Field("undoIndex", &PrefabCreateResult::UndoIndex, "The undo index; 0 in a dry run or a batch.");

		registry.Struct<PrefabInstantiateParams>("PrefabInstantiateParams", "The params of prefab.instantiate.")
			.Field("prefab", &PrefabInstantiateParams::Prefab, "The prefab asset: 16 hex digits, a path such as \"Assets/Prefabs/Cell.prefab\", or a glTF.")
			.Field("parent", &PrefabInstantiateParams::Parent, "The parent entity; empty for a root.")
			.Field("index", &PrefabInstantiateParams::Index, "The position among the parent's children; last when absent.")
			.Field("transform", &PrefabInstantiateParams::Transform, "Transform fields over the prefab root's, such as {\"Translation\": [2, 0, 0]}.")
			.Field("name", &PrefabInstantiateParams::Name, "The instance root's name; the prefab root's when absent.")
			.VariantField("components", &PrefabInstantiateParams::Components,
				"Per-instance component values for the instance root by registry name, merged into the prefab's as entity.update merges them "
				"(missing components are added) and recorded as the instance's overrides; script fields go in {\"Script\": {\"Scripts\": "
				"[{\"Fields\": {...}}]}}: an entry that gives Fields without Script takes the Script handle of the prefab root's slot at its "
				"index, every other member an entry omits takes its default (repeat a non-zero ExecutionOrder), and the list replaces the root's. "
				"An Entity value takes 16 hex digits, a unique id prefix of at least 6 or a path such as \"/Game/ScoreText\", resolved in the "
				"edit scene the request addresses.",
				&ResolveComponentValue);

		registry.Struct<PrefabInstantiateResult>("PrefabInstantiateResult", "The new instance.")
			.Field("entity", &PrefabInstantiateResult::Entity, "The instance root (prefab.create's result calls it instance).")
			.Field("prefab", &PrefabInstantiateResult::Prefab, "The prefab asset.")
			.Field("undoIndex", &PrefabInstantiateResult::UndoIndex, "The undo index; 0 in a dry run or a batch.");

		registry.Struct<PrefabApplyParams>("PrefabApplyParams", "The params of prefab.apply.")
			.Field("instance", &PrefabApplyParams::Instance,
				"The instance root, outermost or nested, whose overrides are applied: those its outermost instance records for it and the "
				"entities below it.")
			.Field("level", &PrefabApplyParams::Level,
				"The instance whose prefab receives the overrides: the outermost instance root (the default) or a nested instance root down "
				"to instance.");

		registry.Struct<PrefabApplyResult>("PrefabApplyResult", "The applied prefab.")
			.Field("prefab", &PrefabApplyResult::Prefab, "The prefab asset written: the level's.")
			.Field("updatedInstances", &PrefabApplyResult::UpdatedInstances,
				"The outermost instances rebuilt in the scene the request addresses from it or from a prefab nesting it, this one's included.")
			.Field("updatedPrefabs", &PrefabApplyResult::UpdatedPrefabs, "The prefab assets nesting the applied prefab that were rewritten, in nesting order.")
			.Field("undoIndex", &PrefabApplyResult::UndoIndex, "The undo index; 0 in a batch.");

		registry.Struct<PrefabRevertParams>("PrefabRevertParams", "The params of prefab.revert.")
			.Field("instance", &PrefabRevertParams::Instance,
				"The instance root to revert; a nested one reverts the overrides its outermost instance records within it.")
			.Field("overrides", &PrefabRevertParams::Overrides, "Only the overrides these keys select; every override when absent.");

		registry.Struct<PrefabRevertResult>("PrefabRevertResult", "The reverted instance.")
			.Field("instance", &PrefabRevertResult::Instance, "The instance root.")
			.Field("removedOverrides", &PrefabRevertResult::RemovedOverrides, "The overrides removed.")
			.Field("undoIndex", &PrefabRevertResult::UndoIndex, "The undo index; 0 when nothing changed, in a dry run or a batch.");

		registry.Struct<PrefabUnpackParams>("PrefabUnpackParams", "The params of prefab.unpack.")
			.Field("instance", &PrefabUnpackParams::Instance,
				"The outermost instance root to unlink from its prefab; the instances nested in it become outermost.");

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
				.Description = "Saves an entity's subtree as a new .prefab asset (the instances inside it stay live, nested in it), optionally "
							   "replacing the entity with an instance of it, as one undoable step. The result's instance is that instance's root "
							   "(empty without replaceWithInstance). Replacing a member of another prefab instance records its deletion there "
							   "(RemoveEntity), and the new instance becomes a user child of that instance.",
				.RequiredParams = { "entity", "path" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.SupportsSceneTarget = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Turn the cell into a prefab and an instance of it.", .Params = createExample } },
			},
			&Automation::PrefabCreate);

		Json instantiateExample = Json::object();
		instantiateExample["prefab"] = "Assets/Prefabs/Cell.prefab";
		instantiateExample["parent"] = "/Board";
		instantiateExample["transform"] = Json::object();
		instantiateExample["transform"]["Translation"] = Json::array({ 2.0, 0.0, 0.0 });
		Json overridesExample = instantiateExample;
		overridesExample["name"] = "Cell 3";
		overridesExample["components"] = Json::object();
		overridesExample["components"]["MeshRenderer"] = Json::object();
		overridesExample["components"]["MeshRenderer"]["CastShadows"] = false;
		methods.Add(
			{
				.Name = "prefab.instantiate",
				.Description = "Creates an instance of a prefab asset (a .prefab or a glTF) under a parent, with an optional transform, name and "
							   "per-instance component values (recorded as the instance's overrides), as one undoable step. The result's "
							   "entity is the new instance root.",
				.RequiredParams = { "prefab" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.SupportsSceneTarget = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Place a cell on the board.", .Params = instantiateExample },
					{ .Description = "Place a named cell that casts no shadow (an override of the instance).", .Params = overridesExample } },
			},
			&Automation::PrefabInstantiate);

		Json applyExample = Json::object();
		applyExample["instance"] = "/Board/Cell";
		Json levelExample = Json::object();
		levelExample["instance"] = "/Room/Lamp";
		levelExample["level"] = "/Room/Lamp";
		methods.Add(
			{
				.Name = "prefab.apply",
				.Description = "Writes an instance's overrides into a .prefab asset, by default its outermost instance's prefab, or with level the "
							   "prefab of a nested instance on the way down; then rewrites the prefabs nesting it (updatedPrefabs) and rebuilds "
							   "their instances in the scene it addresses, as one undoable step, and in every other open scene, as a step of "
							   "that scene's own undo history.",
				.RequiredParams = { "instance" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsSceneTarget = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Make the cell's edits the prefab's.", .Params = applyExample },
					{ .Description = "Make the edits of a lamp inside a room the Lamp prefab's, so every room's lamps follow.", .Params = levelExample } },
			},
			&Automation::PrefabApply);

		Json revertExample = Json::object();
		revertExample["instance"] = "/Board/Cell";
		methods.Add(
			{
				.Name = "prefab.revert",
				.Description = "Removes an instance's overrides (all, or those the keys select) and rebuilds it from its prefab; user children "
							   "stay. On a nested instance, the overrides its outermost instance records within it.",
				.RequiredParams = { "instance" },
				.Mutates = true,
				.SupportsDryRun = true,
				.SupportsSceneTarget = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Drop every edit of the cell.", .Params = revertExample } },
			},
			&Automation::PrefabRevert);

		Json unpackExample = Json::object();
		unpackExample["instance"] = "/Board/Cell";
		methods.Add(
			{
				.Name = "prefab.unpack",
				.Description = "Unlinks an outermost instance from its prefab, one level at a time: its entities and their data stay as ordinary "
							   "entities, and the instances nested in it become outermost.",
				.RequiredParams = { "instance" },
				.Mutates = true,
				.SupportsDryRun = true,
				.SupportsSceneTarget = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Make the cell an ordinary entity.", .Params = unpackExample } },
			},
			&Automation::PrefabUnpack);
	}

}
