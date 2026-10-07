#include "EditorPCH.h"
#include "EditorCore/Commands/SceneEditCommand.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace Utils {

		// An estimate of the heap bytes `value` holds: one node per value plus string and key storage. Iterative, because
		// Variant values may nest up to MaxJsonDepth levels.
		static size_t EstimateJsonBytes(const Json& value)
		{
			size_t total = 0;
			std::vector<const Json*> pending{ &value };
			while (!pending.empty())
			{
				const Json& node = *pending.back();
				pending.pop_back();
				total += sizeof(Json);
				if (const std::string* text = node.get_ptr<const std::string*>())
				{
					total += text->capacity();
				}
				else if (node.is_object())
				{
					for (auto member = node.begin(); member != node.end(); ++member)
					{
						total += sizeof(std::pair<std::string, Json>) + member.key().capacity();
						pending.push_back(&member.value());
					}
				}
				else if (node.is_array())
				{
					for (const Json& element : node)
						pending.push_back(&element);
				}
			}
			return total;
		}

		static size_t EstimateChangeBytes(const SceneEntityChange& change)
		{
			size_t total = sizeof(SceneEntityChange);
			if (change.Before != nullptr)
				total += EstimateJsonBytes(*change.Before);
			if (change.After != nullptr)
				total += EstimateJsonBytes(*change.After);
			return total;
		}

		// The registry names under the entity object's "Components" (none when the member is missing).
		static std::vector<std::string_view> GetComponentNames(const Json& entity)
		{
			std::vector<std::string_view> names;
			const auto components = entity.find("Components");
			if (components == entity.end() || !components->is_object())
				return names;
			for (auto component = components->begin(); component != components->end(); ++component)
				names.push_back(component.key());
			return names;
		}

		// The "Components" object of an entity snapshot, or null when it has none.
		static const Json* FindComponents(const Json& entity)
		{
			const auto components = entity.find("Components");
			return components != entity.end() && components->is_object() ? &*components : nullptr;
		}

		// `entity` with "Parent" set to null, so it applies to (or creates) an entity in the root list. Hierarchy positions
		// are restored separately, with Scene::SetParent.
		static Json WithRootParent(const Json& entity)
		{
			Json copy = entity;
			copy["Parent"] = nullptr;
			return copy;
		}

		// A located Validation error for a recorded state that no longer applies, keeping a Validation cause as it is.
		static Error ToValidationError(Error error)
		{
			if (error.GetCode() == ErrorCode::Validation)
				return error;
			return Error(ErrorCode::Validation, error.ToString());
		}

		// Brings every entity of `changes` to the Before (`after` false) or After (`after` true) state, from whatever state
		// the scene holds (existence is read from the scene; the other side's snapshot only saves work):
		//   1. every touched entity that exists and has a target state is moved to the end of the root list, so each child
		//      list holds only untouched entities, in their target relative order (ChangeTracker, ADR 0006 decision 17);
		//   2. entities without a target state are destroyed (their subtrees hold only touched entities);
		//   3. components that the target state lacks are removed first (an intersection of two valid component sets is
		//      valid), so a unique-per-scene component can move between entities within one change;
		//   4. the target JSON is applied to the existing entities and the missing ones are created, all in the root list;
		//   5. each touched entity moves to its target parent, every parent's restored children in ascending index order,
		//      which puts each at its recorded index among the untouched siblings.
		// Errors: those of the serializer and the scene (nothing is rolled back).
		static Status RestoreEntities(Scene& scene, std::span<const SceneEntityChange> changes, bool after)
		{
			const auto targetOf = [after](const SceneEntityChange& change) -> const Json*
			{
				return (after ? change.After : change.Before).get();
			};
			const auto sourceOf = [after](const SceneEntityChange& change) -> const Json*
			{
				return (after ? change.Before : change.After).get();
			};
			const LoadOptions options;
			LoadReport report;

			// 1. Detach.
			for (const SceneEntityChange& change : changes)
			{
				const Entity entity = scene.FindEntityByID(change.EntityID);
				if (entity.IsValid() && targetOf(change) != nullptr)
					ENGINE_TRY(scene.SetParent(entity, Entity(), std::nullopt, false));
			}

			// 2. Destroy.
			for (const SceneEntityChange& change : changes)
			{
				const Entity entity = scene.FindEntityByID(change.EntityID);
				if (entity.IsValid() && targetOf(change) == nullptr)
					scene.DestroyEntity(entity);
			}

			// 3. Strip.
			for (const SceneEntityChange& change : changes)
			{
				const Json* target = targetOf(change);
				const Entity entity = scene.FindEntityByID(change.EntityID);
				if (target == nullptr || !entity.IsValid())
					continue;
				Json current;
				if (const Json* source = sourceOf(change); source != nullptr)
				{
					current = *source;
				}
				else
				{
					Result<Json> serialized = SceneSerializer::EntityToJson(entity);
					if (!serialized)
						continue; // the target state replaces it whole in step 4 anyway
					current = std::move(*serialized);
				}
				const std::vector<std::string_view> targetNames = GetComponentNames(*target);
				std::vector<std::string> removed;
				for (const std::string_view name : GetComponentNames(current))
				{
					if (std::find(targetNames.begin(), targetNames.end(), name) == targetNames.end())
						removed.emplace_back(name);
				}
				if (removed.empty())
					continue;
				Json& components = current["Components"];
				for (const std::string& name : removed)
					components.erase(name);
				current["Parent"] = nullptr;
				ENGINE_TRY(SceneSerializer::ApplyEntityJson(entity, JsonReader(current), options, report));
			}

			// 4. Apply and create.
			for (const SceneEntityChange& change : changes)
			{
				const Json* target = targetOf(change);
				if (target == nullptr)
					continue;
				const Json rooted = WithRootParent(*target);
				if (const Entity entity = scene.FindEntityByID(change.EntityID); entity.IsValid())
				{
					ENGINE_TRY(SceneSerializer::ApplyEntityJson(entity, JsonReader(rooted), options, report));
				}
				else
				{
					ENGINE_TRY(SceneSerializer::EntityFromJson(scene, JsonReader(rooted), std::nullopt, options, report));
				}
			}

			// 5. Place.
			struct Placement
			{
				UUID Parent;
				uint32_t Index = 0;
				UUID Entity;
			};
			std::vector<Placement> placements;
			placements.reserve(changes.size());
			for (const SceneEntityChange& change : changes)
			{
				if (targetOf(change) != nullptr)
				{
					placements.push_back(after ? Placement{ change.ParentAfter, change.SiblingIndexAfter, change.EntityID }
											   : Placement{ change.ParentBefore, change.SiblingIndexBefore, change.EntityID });
				}
			}
			std::sort(placements.begin(), placements.end(), [](const Placement& left, const Placement& right)
			{
				if (left.Parent != right.Parent)
					return left.Parent < right.Parent;
				if (left.Index != right.Index)
					return left.Index < right.Index;
				return left.Entity < right.Entity;
			});
			for (const Placement& placement : placements)
			{
				Entity parent;
				if (placement.Parent.IsValid())
				{
					parent = scene.FindEntityByID(placement.Parent);
					if (!parent.IsValid())
						return MakeError(ErrorCode::Validation, "the recorded parent {} of entity {} is not an entity of the scene", placement.Parent, placement.Entity);
				}
				ENGINE_TRY(scene.SetParent(scene.FindEntityByID(placement.Entity), parent, placement.Index, false));
			}
			return {};
		}

		// The index of an untouched entity in a child list after an edit, from its index before: `index` minus the touched
		// entities in the list before it (`touchedBefore`, the touched entities' indexes in the list before the edit) is its
		// rank among the untouched entities, which keep their relative order; the touched entities in the list after the
		// edit (`touchedAfter`, ascending) are then placed around it.
		static uint32_t RebaseSiblingIndex(uint32_t index, std::span<const uint32_t> touchedBefore, std::vector<uint32_t> touchedAfter)
		{
			uint32_t rank = index;
			for (const uint32_t touched : touchedBefore)
			{
				if (touched < index)
					--rank;
			}
			std::sort(touchedAfter.begin(), touchedAfter.end());
			uint32_t rebased = rank;
			for (const uint32_t touched : touchedAfter)
			{
				if (touched > rebased)
					break;
				++rebased;
			}
			return rebased;
		}

		// The indexes under `parent` of the changes that have a Before (`before` true) or After state there.
		static std::vector<uint32_t> CollectSiblingIndexes(std::span<const SceneEntityChange> changes, UUID parent, bool before)
		{
			std::vector<uint32_t> indexes;
			for (const SceneEntityChange& change : changes)
			{
				if (before && change.Before != nullptr && change.ParentBefore == parent)
					indexes.push_back(change.SiblingIndexBefore);
				else if (!before && change.After != nullptr && change.ParentAfter == parent)
					indexes.push_back(change.SiblingIndexAfter);
			}
			return indexes;
		}

	}

	SceneEditCommand::SceneEditCommand(std::string label, std::vector<SceneEntityChange> changes, std::string mergeKey)
		: m_Label(std::move(label)), m_Changes(std::move(changes)), m_MergeKey(std::move(mergeKey))
	{
		ENGINE_ASSERT(!m_Changes.empty(), "SceneEditCommand '{}' needs at least one change", m_Label);
		for (const SceneEntityChange& change : m_Changes)
		{
			ENGINE_ASSERT(change.Before != nullptr || change.After != nullptr, "SceneEditCommand '{}': the change of {} has neither state",
				m_Label, change.EntityID);
			m_MemorySize += Utils::EstimateChangeBytes(change);
		}
		m_MemorySize += sizeof(SceneEditCommand) + m_Label.capacity() + m_MergeKey.capacity();
	}

	Status SceneEditCommand::Execute(EditorContext& context)
	{
		if (m_IsApplied)
			return {};
		Scene& scene = context.GetScene();
		if (Status applied = ApplyChanges(scene, m_Changes, true); !applied)
		{
			// Back to the Before state, from whatever part of the After state was reached.
			if (Status restored = Utils::RestoreEntities(scene, m_Changes, false); !restored)
				ENGINE_ERROR("Redoing '{}' failed and its Before state could not be restored: {}", m_Label, restored.error().ToString());
			return std::unexpected(Utils::ToValidationError(std::move(applied).error()).WithContext(std::format("redoing '{}'", m_Label)));
		}
		m_IsApplied = true;
		AppendChangeEvents(context, m_Changes, true);
		return {};
	}

	Status SceneEditCommand::Undo(EditorContext& context)
	{
		Status restored = ApplyChanges(context.GetScene(), m_Changes, false);
		ENGINE_ASSERT(restored.has_value(), "undoing '{}' failed although its Before state was valid when recorded: {}", m_Label,
			restored ? std::string() : restored.error().ToString());
		if (!restored)
			return std::unexpected(std::move(restored).error().WithContext(std::format("undoing '{}'", m_Label)));
		m_IsApplied = false;
		AppendChangeEvents(context, m_Changes, false);
		return {};
	}

	void SceneEditCommand::AppendChangeEvents(EditorContext& context, std::span<const SceneEntityChange> changes, bool after)
	{
		const auto makeEvent = [](EngineEventType type, UUID id, std::string name)
		{
			EngineEvent event;
			event.Type = type;
			event.Id = id;
			event.Name = std::move(name);
			return event;
		};
		for (const SceneEntityChange& change : changes)
		{
			const Json* target = (after ? change.After : change.Before).get();
			const Json* source = (after ? change.Before : change.After).get();
			if (target != nullptr && source == nullptr)
			{
				const std::optional<JsonReader> name = JsonReader(*target).FindMember("Name");
				context.AppendEvent(makeEvent(EngineEventType::EntityCreated, change.EntityID, name ? name->ReadString().value_or(std::string()) : std::string()));
				continue;
			}
			if (target == nullptr)
			{
				if (source != nullptr)
					context.AppendEvent(makeEvent(EngineEventType::EntityDestroyed, change.EntityID, {}));
				continue;
			}

			const Json empty = Json::object();
			const Json* targetComponents = Utils::FindComponents(*target);
			const Json* sourceComponents = Utils::FindComponents(*source);
			const Json& now = targetComponents != nullptr ? *targetComponents : empty;
			const Json& before = sourceComponents != nullptr ? *sourceComponents : empty;
			for (auto component = now.begin(); component != now.end(); ++component)
			{
				const auto previous = before.find(component.key());
				if (previous == before.end() || *previous != component.value())
					context.AppendEvent(makeEvent(EngineEventType::ComponentChanged, change.EntityID, component.key()));
			}
			for (auto component = before.begin(); component != before.end(); ++component)
			{
				if (!now.contains(component.key()))
					context.AppendEvent(makeEvent(EngineEventType::ComponentChanged, change.EntityID, component.key()));
			}
		}
	}

	bool SceneEditCommand::MergeWith(const Command& next)
	{
		// Equal merge keys imply equal command types (Command::GetMergeKey).
		const SceneEditCommand& other = static_cast<const SceneEditCommand&>(next);
		std::vector<SceneEntityChange> merged;
		merged.reserve(m_Changes.size() + other.m_Changes.size());

		// Sibling indexes are relative to the child lists at the start (Before) and end (After) of one edit. The merged
		// command spans both edits, so an entity only the later edit touched has its Before index rebased from the lists
		// between the edits to those before the first, and an entity only this edit touched has its After index rebased to
		// the lists after the second.
		auto mine = m_Changes.begin();
		auto theirs = other.m_Changes.begin();
		while (mine != m_Changes.end() || theirs != other.m_Changes.end())
		{
			if (theirs == other.m_Changes.end() || (mine != m_Changes.end() && mine->EntityID < theirs->EntityID))
			{
				SceneEntityChange change = *mine++;
				if (change.After != nullptr)
				{
					change.SiblingIndexAfter = Utils::RebaseSiblingIndex(change.SiblingIndexAfter,
						Utils::CollectSiblingIndexes(other.m_Changes, change.ParentAfter, true),
						Utils::CollectSiblingIndexes(other.m_Changes, change.ParentAfter, false));
				}
				merged.push_back(std::move(change));
			}
			else if (mine == m_Changes.end() || theirs->EntityID < mine->EntityID)
			{
				SceneEntityChange change = *theirs++;
				if (change.Before != nullptr)
				{
					change.SiblingIndexBefore = Utils::RebaseSiblingIndex(change.SiblingIndexBefore,
						Utils::CollectSiblingIndexes(m_Changes, change.ParentBefore, false),
						Utils::CollectSiblingIndexes(m_Changes, change.ParentBefore, true));
				}
				merged.push_back(std::move(change));
			}
			else
			{
				SceneEntityChange change = *mine++;
				change.After = theirs->After;
				change.ParentAfter = theirs->ParentAfter;
				change.SiblingIndexAfter = theirs->SiblingIndexAfter;
				++theirs;
				if (change.Before != nullptr || change.After != nullptr)
					merged.push_back(std::move(change)); // created by this edit and destroyed by the next: nothing to keep
			}
		}
		if (merged.empty())
			return false; // the two edits cancel out; the history keeps them as two steps instead of an empty one

		m_Changes = std::move(merged);
		m_MemorySize = sizeof(SceneEditCommand) + m_Label.capacity() + m_MergeKey.capacity();
		for (const SceneEntityChange& change : m_Changes)
			m_MemorySize += Utils::EstimateChangeBytes(change);
		return true;
	}

	size_t SceneEditCommand::GetMemorySize() const
	{
		return m_MemorySize;
	}

	Status SceneEditCommand::ReplayOnSceneCopy(Scene& scene, bool after) const
	{
		if (Status applied = ApplyChanges(scene, m_Changes, after); !applied)
			return std::unexpected(std::move(applied).error().WithContext(std::format("replaying '{}' on a scene copy", m_Label)));
		return {};
	}

	Status SceneEditCommand::ApplyChanges(Scene& scene, std::span<const SceneEntityChange> changes, bool after)
	{
		// The scene must be in the opposite state: every entity that has a state there exists.
		for (const SceneEntityChange& change : changes)
		{
			const bool hasSource = (after ? change.Before : change.After) != nullptr;
			if (hasSource && !scene.FindEntityByID(change.EntityID).IsValid())
			{
				ErrorLocation location;
				location.Entity = change.EntityID;
				return std::unexpected(Error(ErrorCode::Validation,
					std::format("the entity {} of a recorded edit is not in scene '{}'", change.EntityID, scene.GetName()))
						.WithLocation(std::move(location)));
			}
		}
		if (Status restored = Utils::RestoreEntities(scene, changes, after); !restored)
			return std::unexpected(Utils::ToValidationError(std::move(restored).error()));
		return {};
	}

}
