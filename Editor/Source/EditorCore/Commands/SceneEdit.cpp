#include "EditorPCH.h"
#include "EditorCore/Commands/SceneEdit.h"

#include "EditorCore/Commands/SceneEditCommand.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Private/SceneEditRollback.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/ChangeTracker.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/PrefabAsset.h"
#include "Engine/Scene/PrefabInstantiator.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

		// The parent's UUID, or the invalid UUID for a root.
		static UUID GetParentID(ConstEntity entity)
		{
			const ConstEntity parent = entity.GetParent();
			return parent.IsValid() ? parent.GetUUID() : UUID();
		}

		// True when `entity` (the instance root itself when `isRoot`) or a prefab member below it was touched by the active edit.
		// A nested instance root answers for its own members, and user children (no PrefabLinkComponent) never count: they
		// are stored in the scene as they are, not as overrides (§5.5).
		static bool HasTouchedMember(const Scene& scene, const ChangeTracker& tracker, ConstEntity entity, bool isRoot)
		{
			if (!isRoot && entity.TryGetComponent<PrefabInstanceComponent>() != nullptr)
				return false;
			if ((isRoot || entity.TryGetComponent<PrefabLinkComponent>() != nullptr) && !tracker.NeedsSnapshot(entity.GetUUID()))
				return true;
			for (const UUID child : entity.GetChildren())
			{
				const ConstEntity childEntity = scene.FindEntityByID(child);
				if (childEntity.IsValid() && HasTouchedMember(scene, tracker, childEntity, false))
					return true;
			}
			return false;
		}

		// §5.5 "When an edit commits on an instance member, the change tracker records field-level overrides": refreshes the
		// override records of every prefab instance whose root or members the active edit touched, against the prefab's
		// current version (PrefabInstantiator::RefreshOverrides; overrides are derived by diffing, ADR 0006 decision 17), while
		// the tracker still runs, so the new records are part of the same undo step and scene.open's rebuild from prefab +
		// overrides (ADR 0010 decision 11) keeps the edit. An instance whose prefab is not registered keeps its records
		// (PREFAB_MISSING_ASSET); one whose prefab cannot be loaded or diffed keeps them with a warning.
		static void RecordTouchedInstanceOverrides(EditorContext& context, Scene& scene, std::string_view label)
		{
			const Scene& constScene = scene;
			const ChangeTracker& tracker = scene.GetChangeTracker();
			std::vector<std::pair<UUID, AssetHandle>> touched;
			constScene.ForEachCanonical([&constScene, &tracker, &touched](ConstEntity entity)
			{
				const PrefabInstanceComponent* instance = entity.TryGetComponent<PrefabInstanceComponent>();
				if (instance != nullptr && instance->Prefab.GetHandle().IsValid() && HasTouchedMember(constScene, tracker, entity, true))
					touched.emplace_back(entity.GetUUID(), instance->Prefab.GetHandle());
			});
			if (touched.empty())
				return;

			const PrefabOptions options{ .Schemas = nullptr };
			std::map<AssetHandle, Prefab> loaded;
			std::set<AssetHandle> unavailable;
			for (const auto& [rootID, handle] : touched)
			{
				if (unavailable.contains(handle))
					continue;
				auto prefab = loaded.find(handle);
				if (prefab == loaded.end())
				{
					LoadReport report;
					Result<Prefab> current = LoadPrefabAsset(context.GetAssets(), handle, scene.GetTypeRegistry(), report);
					if (!current)
					{
						if (current.error().GetCode() != ErrorCode::NotFound)
							ENGINE_WARN("'{}' keeps the recorded overrides of the instances of prefab {}: {}", label, handle, current.error().ToString());
						unavailable.insert(handle);
						continue;
					}
					prefab = loaded.emplace(handle, std::move(*current)).first;
				}
				const Entity root = scene.FindEntityByID(rootID);
				if (Status refreshed = PrefabInstantiator::RefreshOverrides(root, prefab->second, options); !refreshed)
					ENGINE_WARN("'{}' keeps the recorded overrides of the prefab instance '{}': {}", label, scene.GetEntityPath(root), refreshed.error().ToString());
			}
		}

	}

	SceneEdit::SceneEdit(EditorContext& context, std::string label, std::string mergeKey)
		: m_Context(&context), m_Label(std::move(label)), m_MergeKey(std::move(mergeKey))
	{
		ENGINE_ASSERT(context.HasScene(), "SceneEdit '{}' needs an open scene", m_Label);
		Scene& scene = context.GetScene();
		ENGINE_ASSERT(!scene.GetChangeTracker().IsTracking(), "SceneEdit '{}': another SceneEdit of scene '{}' is active (edits do not nest)",
			m_Label, scene.GetName());
		m_RevisionBefore = context.GetRevision();
#if defined(ENGINE_DEBUG)
		// §12.3: Commit checks that the state of every entity the tracker does not report is unchanged. The state compared
		// is the canonical entity JSON, the input of the state hash, so the check is exact and needs no hashing.
		const Scene& constScene = scene;
		m_EntityStates.reserve(scene.GetEntityCount());
		constScene.ForEachCanonical([this](ConstEntity entity)
		{
			Result<Json> json = SceneSerializer::EntityToJson(entity);
			m_EntityStates.emplace_back(entity.GetUUID(), json ? std::move(*json) : Json());
		});
#endif
		scene.GetChangeTracker().Begin();
	}

	SceneEdit::~SceneEdit()
	{
		Cancel();
	}

	Result<uint64_t> SceneEdit::Commit()
	{
		ENGINE_ASSERT(m_IsActive, "SceneEdit '{}' committed after it ended", m_Label);
		if (!m_IsActive)
			return 0;
		m_IsActive = false;
		Scene& scene = GetScene();
		Utils::RecordTouchedInstanceOverrides(*m_Context, scene, m_Label);
		const std::vector<EntityChange> tracked = scene.GetChangeTracker().End();
#if defined(ENGINE_DEBUG)
		// §12.3: every entity the tracker did not report is unchanged (see the constructor).
		for (const auto& [id, state] : m_EntityStates)
		{
			// ChangeTracker::End sorts by UUID.
			const auto found = std::lower_bound(tracked.begin(), tracked.end(), id, [](const EntityChange& change, UUID value)
			{
				return change.EntityID < value;
			});
			if (found != tracked.end() && found->EntityID == id)
				continue;
			const ConstEntity entity = std::as_const(scene).FindEntityByID(id);
			ENGINE_ASSERT(entity.IsValid() && SceneSerializer::EntityToJson(entity) == state,
				"SceneEdit '{}' changed entity {} without the change tracker recording it", m_Label, id);
		}
		m_EntityStates.clear();
#endif

		if (tracked.empty())
			return 0;

		std::vector<SceneEntityChange> changes;
		changes.reserve(tracked.size());
		for (const EntityChange& change : tracked)
		{
			if (change.Kind != EntityChangeKind::Created && change.Before == nullptr)
			{
				Utils::RollBackTrackedChanges(scene, tracked, m_Label);
				return MakeError(ErrorCode::Validation, "'{}' cannot be recorded: the state of entity {} before the edit could not be serialized", m_Label,
					change.EntityID);
			}
			SceneEntityChange recorded{ .EntityID = change.EntityID,
				.Before = change.Kind == EntityChangeKind::Created ? nullptr : change.Before,
				.After = nullptr,
				.ParentBefore = change.ParentBefore,
				.SiblingIndexBefore = change.SiblingIndexBefore,
				.ParentAfter = UUID(),
				.SiblingIndexAfter = 0 };
			if (const ConstEntity entity = std::as_const(scene).FindEntityByID(change.EntityID); entity.IsValid())
			{
				Result<Json> after = SceneSerializer::EntityToJson(entity);
				if (!after)
				{
					Utils::RollBackTrackedChanges(scene, tracked, m_Label);
					return std::unexpected(std::move(after).error().WithContext(std::format("recording '{}'", m_Label)));
				}
				recorded.After = CreateRef<const Json>(std::move(*after));
				recorded.ParentAfter = Utils::GetParentID(entity);
				recorded.SiblingIndexAfter = entity.GetSiblingIndex();
			}
			if (recorded.Before == nullptr && recorded.After == nullptr)
				continue; // ChangeTracker never reports these, but such a change would have nothing to restore
			changes.push_back(std::move(recorded));
		}
		if (changes.empty())
			return 0;

		// Kept to roll back if the context refuses the command (a read-only project); the snapshots are shared.
		std::vector<SceneEntityChange> rollback = changes;
		m_Context->m_PendingRevisionBefore = m_RevisionBefore;
		Result<uint64_t> executed = m_Context->Execute(CreateScope<SceneEditCommand>(m_Label, std::move(changes), m_MergeKey));
		m_Context->m_PendingRevisionBefore.reset();
		if (!executed)
		{
			const Status restored = SceneEditCommand::ApplyChanges(scene, rollback, false);
			ENGINE_ASSERT(restored.has_value(), "rolling back '{}' failed: {}", m_Label, restored ? std::string() : restored.error().ToString());
			if (!restored)
				ENGINE_ERROR("Rolling back '{}' failed: {}", m_Label, restored.error().ToString());
			return std::unexpected(std::move(executed).error());
		}
		// The command was built applied, so its Execute appended nothing: the events of the edit are appended here, once it
		// is recorded (a dry run suppresses them).
		SceneEditCommand::AppendChangeEvents(*m_Context, rollback, true);
		return *executed;
	}

	void SceneEdit::Cancel()
	{
		if (!m_IsActive)
			return;
		m_IsActive = false;
		Scene& scene = GetScene();
		const std::vector<EntityChange> tracked = scene.GetChangeTracker().End();
		m_EntityStates.clear();
		if (!tracked.empty())
			Utils::RollBackTrackedChanges(scene, tracked, m_Label);
	}

	Scene& SceneEdit::GetScene() const
	{
		return m_Context->GetScene();
	}

}
