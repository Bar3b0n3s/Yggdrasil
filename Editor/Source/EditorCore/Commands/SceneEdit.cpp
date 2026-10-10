#include "EditorPCH.h"
#include "EditorCore/Commands/SceneEdit.h"

#include "EditorCore/Commands/SceneEditCommand.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Private/SceneEditRollback.h"
#include "Engine/Asset/ScriptData.h"
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
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <span>
#include <string_view>
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

			// Only edit-scene overrides are persisted. Keep their current asset schemas alive for the whole diff, even
			// while a play session retains an older snapshot of the same scripts.
			const Result<Ref<const ScriptFieldSchemaSource>> schemas = context.GetScriptSchemaSnapshot();
			if (!schemas)
			{
				ENGINE_WARN("'{}' keeps the recorded prefab overrides: {}", label, schemas.error().ToString());
				return;
			}
			const PrefabOptions options{ .Schemas = schemas->get() };
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

	namespace Utils {

		// The SceneEditCommand changes of one tracked scene: the tracker's Before snapshots with the scene's current state
		// (After). Errors: Validation when a state cannot be serialized; nothing is rolled back here.
		static Result<std::vector<SceneEntityChange>> BuildSceneEntityChanges(const Scene& scene, std::span<const EntityChange> tracked,
			std::string_view label)
		{
			std::vector<SceneEntityChange> changes;
			changes.reserve(tracked.size());
			for (const EntityChange& change : tracked)
			{
				if (change.Kind != EntityChangeKind::Created && change.Before == nullptr)
				{
					return MakeError(ErrorCode::Validation, "'{}' cannot be recorded: the state of entity {} before the edit could not be serialized", label,
						change.EntityID);
				}
				SceneEntityChange recorded{ .EntityID = change.EntityID,
					.Before = change.Kind == EntityChangeKind::Created ? nullptr : change.Before,
					.After = nullptr,
					.ParentBefore = change.ParentBefore,
					.SiblingIndexBefore = change.SiblingIndexBefore,
					.ParentAfter = UUID(),
					.SiblingIndexAfter = 0 };
				if (const ConstEntity entity = scene.FindEntityByID(change.EntityID); entity.IsValid())
				{
					Result<Json> after = SceneSerializer::EntityToJson(entity);
					if (!after)
						return std::unexpected(std::move(after).error().WithContext(std::format("recording '{}'", label)));
					recorded.After = CreateRef<const Json>(std::move(*after));
					recorded.ParentAfter = GetParentID(entity);
					recorded.SiblingIndexAfter = entity.GetSiblingIndex();
				}
				if (recorded.Before == nullptr && recorded.After == nullptr)
					continue; // ChangeTracker never reports these, but such a change would have nothing to restore
				changes.push_back(std::move(recorded));
			}
			return changes;
		}

		// Brings `scene`'s entities of `changes` back to their Before state. Every Before state was valid when it was
		// captured, so a failure is a bug: asserted, and logged in configurations without asserts.
		static void RestoreBefore(Scene& scene, std::span<const SceneEntityChange> changes, std::string_view label)
		{
			const Status restored = SceneEditCommand::ApplyChanges(scene, changes, false);
			ENGINE_ASSERT(restored.has_value(), "rolling back '{}' failed: {}", label, restored ? std::string() : restored.error().ToString());
			if (!restored)
				ENGINE_ERROR("Rolling back '{}' failed: {}", label, restored.error().ToString());
		}

		// The editor's play session when it is still the one with `serial` (PlaySession::GetSerial); nullptr after it ended,
		// also when a new session took its place, possibly at the same address.
		static PlaySession* FindPlaySession(const EditorContext& context, uint64_t serial)
		{
			PlaySession* session = context.GetPlay().GetSession();
			return session != nullptr && session->GetSerial() == serial ? session : nullptr;
		}

	}

	void SceneEdit::TrackedScene::Begin(Scene& scene, std::string_view label)
	{
		ENGINE_ASSERT(!scene.GetChangeTracker().IsTracking(), "SceneEdit '{}': another SceneEdit of scene '{}' is active (edits do not nest)", label,
			scene.GetName());
		Target = &scene;
#if defined(ENGINE_DEBUG)
		// §12.3: Commit checks that the state of every entity the tracker does not report is unchanged. The state compared
		// is the canonical entity JSON, the input of the state hash, so the check is exact and needs no hashing.
		const Scene& constScene = scene;
		EntityStates.reserve(scene.GetEntityCount());
		constScene.ForEachCanonical([this](ConstEntity entity)
		{
			Result<Json> json = SceneSerializer::EntityToJson(entity);
			EntityStates.emplace_back(entity.GetUUID(), json ? std::move(*json) : Json());
		});
#endif
		scene.GetChangeTracker().Begin();
	}

	std::vector<EntityChange> SceneEdit::TrackedScene::End([[maybe_unused]] std::string_view label)
	{
		if (Target == nullptr)
			return {};
		std::vector<EntityChange> tracked = Target->GetChangeTracker().End();
#if defined(ENGINE_DEBUG)
		// §12.3: every entity the tracker did not report is unchanged (see Begin).
		for (const auto& [id, state] : EntityStates)
		{
			// ChangeTracker::End sorts by UUID.
			const auto found = std::lower_bound(tracked.begin(), tracked.end(), id, [](const EntityChange& change, UUID value)
			{
				return change.EntityID < value;
			});
			if (found != tracked.end() && found->EntityID == id)
				continue;
			const ConstEntity entity = std::as_const(*Target).FindEntityByID(id);
			ENGINE_ASSERT(entity.IsValid() && SceneSerializer::EntityToJson(entity) == state,
				"SceneEdit '{}' changed entity {} without the change tracker recording it", label, id);
		}
#endif
		EntityStates.clear();
		return tracked;
	}

	SceneEdit::SceneEdit(EditorContext& context, std::string label, std::string mergeKey)
		: m_Context(&context), m_Label(std::move(label)), m_MergeKey(std::move(mergeKey))
	{
		PlaySession* session = context.GetPlay().GetSession();
		ENGINE_ASSERT(context.HasScene() || session != nullptr, "SceneEdit '{}' needs an open scene or a play session", m_Label);
		m_RevisionBefore = context.GetRevision();
		if (context.HasScene())
			m_EditScene.Begin(context.GetScene(), m_Label);
		if (session != nullptr)
		{
			m_PlayScene.Begin(session->GetScene(), m_Label);
			m_PlaySerial = session->GetSerial();
			m_PlayIdsBefore = session->GetIdGenerator();
		}
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
		// Play-only edits bypass the persistent command history; check the same policy before either target commits.
		if (Status permission = m_Context->CheckMutationPermission(); !permission)
		{
			Cancel();
			return std::unexpected(std::move(permission).error());
		}
		m_IsActive = false;
		if (m_EditScene.Target != nullptr)
			Utils::RecordTouchedInstanceOverrides(*m_Context, *m_EditScene.Target, m_Label);
		const std::vector<EntityChange> editTracked = m_EditScene.End(m_Label);
		const std::vector<EntityChange> playTracked = m_PlayScene.End(m_Label);

		std::vector<SceneEntityChange> editChanges;
		std::vector<SceneEntityChange> playChanges;
		Status built = {};
		if (!editTracked.empty())
		{
			Result<std::vector<SceneEntityChange>> changes = Utils::BuildSceneEntityChanges(*m_EditScene.Target, editTracked, m_Label);
			if (changes)
				editChanges = std::move(*changes);
			else
				built = std::unexpected(std::move(changes).error());
		}
		if (built && !playTracked.empty())
		{
			Result<std::vector<SceneEntityChange>> changes = Utils::BuildSceneEntityChanges(*m_PlayScene.Target, playTracked, m_Label);
			if (changes)
				playChanges = std::move(*changes);
			else
				built = std::unexpected(std::move(changes).error());
		}
		if (!built)
		{
			if (!editTracked.empty())
				Utils::RollBackTrackedChanges(*m_EditScene.Target, editTracked, m_Label);
			if (!playTracked.empty())
				Utils::RollBackTrackedChanges(*m_PlayScene.Target, playTracked, m_Label);
			RestorePlayIds();
			return std::unexpected(std::move(built).error());
		}

		uint64_t undoIndex = 0;
		if (!editChanges.empty())
		{
			// Kept to roll back if the context refuses the command (a read-only project); the snapshots are shared.
			std::vector<SceneEntityChange> rollback = editChanges;
			m_Context->m_PendingRevisionBefore = m_RevisionBefore;
			Result<uint64_t> executed = m_Context->Execute(CreateScope<SceneEditCommand>(m_Label, std::move(editChanges), m_MergeKey));
			m_Context->m_PendingRevisionBefore.reset();
			if (!executed)
			{
				Utils::RestoreBefore(*m_EditScene.Target, rollback, m_Label);
				if (!playChanges.empty())
					Utils::RestoreBefore(*m_PlayScene.Target, playChanges, m_Label);
				RestorePlayIds();
				return std::unexpected(std::move(executed).error());
			}
			// The command was built applied, so its Execute appended nothing: the events of the edit are appended here, once it
			// is recorded (a dry run suppresses them).
			SceneEditCommand::AppendChangeEvents(*m_Context, rollback, true);
			undoIndex = *executed;
		}

		// Play-scene changes are transient (§13.4): nothing records them, and they raise none of the edit scene's events. A dry
		// run or a transaction must still be able to take them back, with the ids they drew (see the class comment).
		PlaySession* session = m_PlayIdsBefore.has_value() ? Utils::FindPlaySession(*m_Context, m_PlaySerial) : nullptr;
		const bool drewIds = session != nullptr && session->GetIdGenerator().GetDrawCount() != m_PlayIdsBefore->GetDrawCount();
		if ((!playChanges.empty() || drewIds) && (m_Context->IsDryRun() || m_Context->GetTransaction() != nullptr))
		{
			m_Context->m_TransientPlayUndos.push_back(
				[context = m_Context, serial = m_PlaySerial, ids = *m_PlayIdsBefore, changes = std::move(playChanges), label = m_Label]()
			{
				// The changes belong to the session they were made in; one that ended has nothing to undo.
				PlaySession* owner = Utils::FindPlaySession(*context, serial);
				if (owner == nullptr)
					return;
				if (!changes.empty())
					Utils::RestoreBefore(owner->GetScene(), changes, label);
				owner->GetIdGenerator() = ids;
			});
		}
		return undoIndex;
	}

	void SceneEdit::Cancel()
	{
		if (!m_IsActive)
			return;
		m_IsActive = false;
		const std::vector<EntityChange> editTracked = m_EditScene.End(m_Label);
		if (!editTracked.empty())
			Utils::RollBackTrackedChanges(*m_EditScene.Target, editTracked, m_Label);
		const std::vector<EntityChange> playTracked = m_PlayScene.End(m_Label);
		if (!playTracked.empty())
			Utils::RollBackTrackedChanges(*m_PlayScene.Target, playTracked, m_Label);
		RestorePlayIds();
	}

	void SceneEdit::RestorePlayIds() const
	{
		if (!m_PlayIdsBefore.has_value())
			return;
		if (PlaySession* session = Utils::FindPlaySession(*m_Context, m_PlaySerial))
			session->GetIdGenerator() = *m_PlayIdsBefore;
	}

	Scene& SceneEdit::GetScene() const
	{
		return m_Context->GetScene();
	}

}
