#include "EnginePCH.h"
#include "Engine/Scene/Scene.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/DisabledTag.h"
#include "Engine/Scene/Components/IDComponent.h"
#include "Engine/Scene/Components/NameComponent.h"
#include "Engine/Scene/Components/RelationshipComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TagsComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <optional>
#include <utility>

namespace Engine {

	namespace {

		// One segment of an entity path: the unescaped name and the optional "[n]" index among same-named siblings.
		struct EntityPathSegment
		{
			std::string Name = {};
			std::optional<uint64_t> Index = std::nullopt; // saturated at the uint64_t maximum, which no sibling list reaches
		};

	}

	namespace Utils {

		static ErrorIssue MakeIssue(std::string message, std::string hint, std::vector<std::string> suggestions)
		{
			ErrorIssue issue;
			issue.Message = std::move(message);
			issue.Hint = std::move(hint);
			issue.Suggestions = std::move(suggestions);
			return issue;
		}

		static bool IsPathSyntaxCharacter(char character)
		{
			return character == '\\' || character == '/' || character == '[' || character == ']';
		}

		static bool IsDecimalDigit(char character)
		{
			return character >= '0' && character <= '9';
		}

		// `name` with '\', '/', '[' and ']' escaped by a backslash (Scene::FindEntityByPath grammar).
		static std::string EscapePathName(std::string_view name)
		{
			std::string escaped;
			escaped.reserve(name.size());
			for (const char character : name)
			{
				if (IsPathSyntaxCharacter(character))
					escaped.push_back('\\');
				escaped.push_back(character);
			}
			return escaped;
		}

		static std::string FormatPathSegment(const EntityPathSegment& segment)
		{
			std::string text = EscapePathName(segment.Name);
			if (segment.Index.has_value())
				text += std::format("[{}]", *segment.Index);
			return text;
		}

		static Error MakeMalformedPathError(std::string_view path, size_t offset, std::string_view problem)
		{
			return Error(ErrorCode::InvalidArgument, std::format("malformed entity path '{}': {} at offset {}", path, problem, offset))
				.WithHint("paths look like '/Game/Board' or '/Track/Piece[3]'; write '\\', '/', '[' and ']' in names as '\\\\', '\\/', '\\[' "
						  "and '\\]'");
		}

		// Splits `path` into its segments. Every unescaped '/' starts a segment, so "/" is one segment with an empty name.
		static Result<std::vector<EntityPathSegment>> ParseEntityPath(std::string_view path)
		{
			if (path.empty() || path.front() != '/')
				return std::unexpected(MakeMalformedPathError(path, 0, "a path starts with '/'"));

			std::vector<EntityPathSegment> segments(1);
			bool afterIndex = false; // only '/' or the end may follow "[n]"
			for (size_t offset = 1; offset < path.size(); ++offset)
			{
				const char character = path[offset];
				if (character == '/')
				{
					segments.emplace_back();
					afterIndex = false;
					continue;
				}
				if (afterIndex)
					return std::unexpected(MakeMalformedPathError(path, offset, "an index ends its segment"));

				EntityPathSegment& segment = segments.back();
				if (character == '\\')
				{
					if (offset + 1 >= path.size() || !IsPathSyntaxCharacter(path[offset + 1]))
						return std::unexpected(MakeMalformedPathError(path, offset, "a backslash escapes only '\\', '/', '[' or ']'"));
					segment.Name.push_back(path[offset + 1]);
					++offset;
					continue;
				}
				if (character == ']')
					return std::unexpected(MakeMalformedPathError(path, offset, "an unescaped ']'"));
				if (character == '[')
				{
					const size_t first = offset + 1;
					size_t end = first;
					uint64_t index = 0;
					while (end < path.size() && IsDecimalDigit(path[end]))
					{
						const uint64_t digit = static_cast<uint64_t>(path[end] - '0');
						const uint64_t maximum = std::numeric_limits<uint64_t>::max();
						index = index > (maximum - digit) / 10 ? maximum : index * 10 + digit;
						++end;
					}
					if (end == first || end >= path.size() || path[end] != ']')
						return std::unexpected(MakeMalformedPathError(path, offset, "an index is '[' followed by decimal digits and ']'"));
					if (end - first > 1 && path[first] == '0')
						return std::unexpected(MakeMalformedPathError(path, offset, "an index has no leading zeros"));
					segment.Index = index;
					offset = end;
					afterIndex = true;
					continue;
				}
				segment.Name.push_back(character);
			}
			return segments;
		}

		// "the scene root" or "'/Game/Board'", for messages about the children of `parent`.
		static std::string DescribePathParent(const Scene& scene, ConstEntity parent)
		{
			return parent.IsValid() ? std::format("'{}'", scene.GetEntityPath(parent)) : std::string("the scene root");
		}

		static Result<ConstEntity> ResolveEntityPath(const Scene& scene, std::string_view path)
		{
			ENGINE_TRY_ASSIGN(const std::vector<EntityPathSegment> segments, ParseEntityPath(path));

			ConstEntity current;
			std::span<const UUID> siblings = scene.GetRootEntities();
			for (const EntityPathSegment& segment : segments)
			{
				std::vector<ConstEntity> matches;
				for (const UUID id : siblings)
				{
					const ConstEntity sibling = scene.FindEntityByID(id);
					if (sibling.IsValid() && sibling.GetName() == segment.Name)
						matches.push_back(sibling);
				}

				if (matches.empty())
				{
					std::vector<std::string> names;
					names.reserve(siblings.size());
					for (const UUID id : siblings)
					{
						const ConstEntity sibling = scene.FindEntityByID(id);
						if (sibling.IsValid())
							names.push_back(sibling.GetName());
					}
					std::vector<std::string> suggestions = FuzzySuggest(segment.Name, names);
					std::string hint = MakeDidYouMeanHint(suggestions);
					std::string message = std::format("no entity named '{}' under {} (path '{}')", EscapePathName(segment.Name),
						DescribePathParent(scene, current), path);
					return std::unexpected(Error(ErrorCode::NotFound, message)
							.WithHint(hint)
							.WithIssue(MakeIssue(std::move(message), std::move(hint), std::move(suggestions))));
				}

				if (segment.Index.has_value())
				{
					if (*segment.Index >= matches.size())
					{
						return std::unexpected(Error(ErrorCode::NotFound,
							std::format("no entity at '{}': {} has {} {} named '{}', so index {} is out of range", path,
								DescribePathParent(scene, current), matches.size(), matches.size() == 1 ? "entity" : "entities",
								EscapePathName(segment.Name), *segment.Index))
								.WithHint(std::format("indices are zero-based: use '[0]' to '[{}]'", matches.size() - 1)));
					}
					current = matches[static_cast<size_t>(*segment.Index)];
				}
				else if (matches.size() == 1)
				{
					current = matches.front();
				}
				else
				{
					std::vector<ErrorIssue> candidates;
					candidates.reserve(matches.size());
					for (const ConstEntity match : matches)
					{
						std::string candidate = scene.GetEntityPath(match);
						std::string message = std::format("candidate '{}'", candidate);
						candidates.push_back(MakeIssue(std::move(message), {}, { std::move(candidate) }));
					}
					return std::unexpected(Error(ErrorCode::InvalidArgument,
						std::format("entity path '{}' is ambiguous: {} entities named '{}' under {}", path, matches.size(),
							EscapePathName(segment.Name), DescribePathParent(scene, current)))
							.WithHint(std::format("add the zero-based index among them, such as '{}[0]'", FormatPathSegment(segment)))
							.WithIssues(std::move(candidates)));
				}
				siblings = current.GetChildren();
			}
			return current;
		}

	}

	Scene::Scene(const SceneSpecification& specification)
		: m_Specification(specification), m_MainThread(std::this_thread::get_id())
	{
		ENGINE_CORE_ASSERT(m_Specification.Registry != nullptr, "A scene needs a type registry");
		ENGINE_CORE_ASSERT(m_Specification.Registry == nullptr || m_Specification.Registry->IsFrozen(),
			"A scene needs a frozen type registry (TypeRegistry::Freeze)");
		ENGINE_CORE_ASSERT(m_Specification.IdGenerator != nullptr, "A scene needs a UUID generator");
	}

	Scene::~Scene() = default;

	Scope<Scene> Scene::Create(const SceneSpecification& specification)
	{
		return CreateScope<Scene>(specification);
	}

	Entity Scene::CreateEntity(std::string_view name)
	{
		return CreateEntity(name, Entity{});
	}

	Entity Scene::CreateEntity(std::string_view name, Entity parent)
	{
		AssertMainThread();
		// §4.8: a collision with an existing ID draws again (the deterministic generator re-hashes with the next counter).
		UUID id = m_Specification.IdGenerator->Next();
		while (m_EntityIndex.contains(id))
			id = m_Specification.IdGenerator->Next();
		return CreateEntityWithID(id, name, parent);
	}

	Entity Scene::CreateEntityWithID(UUID id, std::string_view name)
	{
		return CreateEntityWithID(id, name, Entity{});
	}

	Entity Scene::CreateEntityWithID(UUID id, std::string_view name, Entity parent)
	{
		AssertMainThread();
		ENGINE_CORE_ASSERT(id.IsValid(), "Scene::CreateEntityWithID needs a valid ID (entity '{}')", name);
		ENGINE_CORE_ASSERT(!m_EntityIndex.contains(id), "Entity ID {} is already used in scene '{}' (creating '{}')", id,
			m_Specification.Name, name);

		entt::entity parentHandle = entt::null;
		if (parent != Entity{})
		{
			ENGINE_CORE_ASSERT(parent.IsValid() && parent.GetScene() == this,
				"The parent of new entity '{}' is not a valid entity of scene '{}'", name, m_Specification.Name);
			parentHandle = parent.GetHandle();
		}
		return CreateEntityInternal(id, name, parentHandle);
	}

	void Scene::DestroyEntity(Entity entity)
	{
		AssertMainThread();
		ENGINE_CORE_ASSERT(entity.IsValid() && entity.GetScene() == this, "DestroyEntity needs a valid entity of scene '{}'",
			m_Specification.Name);

		const std::vector<entt::entity> subtree = CollectSubtreeForDestruction(entity.GetHandle());
		RecordDestruction(subtree);

		std::vector<UUID>& siblings = GetChildList(m_Registry.get<RelationshipComponent>(entity.GetHandle()).Parent);
		std::erase(siblings, entity.GetUUID());
		for (const entt::entity handle : subtree)
			m_EntityIndex.erase(m_Registry.get<IDComponent>(handle).ID);

		if (m_Specification.Runtime)
		{
			// §5.7: the entities stay in the registry until the flush point, invisible to lookups, the hierarchy and IsValid.
			for (const entt::entity handle : subtree)
			{
				m_Registry.emplace<PendingDestroyTag>(handle);
				m_PendingDestroys.push_back(handle);
			}
		}
		else
		{
			for (const entt::entity handle : subtree)
				m_Registry.destroy(handle);
		}

		++m_Revision;
		m_CanonicalOrderDirty = true;
	}

	void Scene::FlushPendingDestroys()
	{
		AssertMainThread();
		for (const entt::entity handle : m_PendingDestroys)
		{
			if (m_Registry.valid(handle))
				m_Registry.destroy(handle);
		}
		m_PendingDestroys.clear();
	}

	void Scene::Clear()
	{
		AssertMainThread();
		if (m_ChangeTracker.IsTracking())
		{
			std::vector<entt::entity> entities;
			entities.reserve(m_EntityIndex.size());
			for (auto root = m_RootEntities.rbegin(); root != m_RootEntities.rend(); ++root)
			{
				const std::vector<entt::entity> subtree = CollectSubtreeForDestruction(FindHandle(*root));
				entities.insert(entities.end(), subtree.begin(), subtree.end());
			}
			RecordDestruction(entities);
		}

		m_Registry.clear();
		m_EntityIndex.clear();
		m_RootEntities.clear();
		m_PendingDestroys.clear();
		++m_Revision;
		m_CanonicalOrderDirty = true;
	}

	Entity Scene::FindEntityByID(UUID id)
	{
		AssertMainThread();
		const entt::entity handle = FindHandle(id);
		return handle == entt::null ? Entity{} : Entity(handle, this);
	}

	ConstEntity Scene::FindEntityByID(UUID id) const
	{
		AssertMainThread();
		const entt::entity handle = FindHandle(id);
		return handle == entt::null ? ConstEntity{} : ConstEntity(handle, this);
	}

	Entity Scene::FindEntityByPath(std::string_view path)
	{
		const Result<Entity> entity = ResolveEntityPath(path);
		return entity.has_value() ? *entity : Entity{};
	}

	ConstEntity Scene::FindEntityByPath(std::string_view path) const
	{
		const Result<ConstEntity> entity = ResolveEntityPath(path);
		return entity.has_value() ? *entity : ConstEntity{};
	}

	Result<Entity> Scene::ResolveEntityPath(std::string_view path)
	{
		ENGINE_TRY_ASSIGN(const ConstEntity entity, std::as_const(*this).ResolveEntityPath(path));
		return Entity(entity.GetHandle(), this);
	}

	Result<ConstEntity> Scene::ResolveEntityPath(std::string_view path) const
	{
		AssertMainThread();
		return Utils::ResolveEntityPath(*this, path);
	}

	std::string Scene::GetEntityPath(ConstEntity entity) const
	{
		AssertMainThread();
		ENGINE_CORE_ASSERT(entity.IsValid() && entity.GetScene() == this, "GetEntityPath needs a valid entity of scene '{}'",
			m_Specification.Name);

		std::vector<std::string> segments;
		for (ConstEntity current = entity; current.IsValid(); current = current.GetParent())
		{
			const ConstEntity parent = current.GetParent();
			const std::span<const UUID> siblings = parent.IsValid() ? parent.GetChildren() : GetRootEntities();
			const std::string& name = current.GetName();
			const UUID id = current.GetUUID();

			size_t sameNamed = 0;
			size_t index = 0;
			for (const UUID siblingID : siblings)
			{
				const ConstEntity sibling = FindEntityByID(siblingID);
				if (sibling.GetName() != name)
					continue;
				if (siblingID == id)
					index = sameNamed;
				++sameNamed;
			}

			EntityPathSegment segment{ .Name = name };
			if (sameNamed > 1)
				segment.Index = index;
			segments.push_back(Utils::FormatPathSegment(segment));
		}

		std::string path;
		for (auto segment = segments.rbegin(); segment != segments.rend(); ++segment)
		{
			path.push_back('/');
			path += *segment;
		}
		return path;
	}

	Status Scene::SetParent(Entity child, Entity parent, std::optional<uint32_t> siblingIndex, bool keepWorld)
	{
		AssertMainThread();
		if (!child.IsValid() || child.GetScene() != this)
			return MakeError(ErrorCode::InvalidArgument, "SetParent needs a valid entity of scene '{}' to move", m_Specification.Name);

		const bool toRoot = parent == Entity{};
		if (!toRoot && (!parent.IsValid() || parent.GetScene() != this))
		{
			return MakeError(ErrorCode::InvalidArgument, "cannot move '{}': the new parent is not a valid entity of scene '{}'",
				child.GetName(), m_Specification.Name);
		}

		const UUID childID = child.GetUUID();
		const UUID newParentID = toRoot ? UUID() : parent.GetUUID();
		for (ConstEntity ancestor = parent; ancestor.IsValid(); ancestor = ancestor.GetParent())
		{
			if (ancestor.GetUUID() == childID)
			{
				ErrorLocation location;
				location.Entity = childID;
				return std::unexpected(Error(ErrorCode::InvalidArgument,
					std::format("cannot move '{}' under '{}': an entity cannot become its own descendant", GetEntityPath(child),
						GetEntityPath(parent)))
						.WithLocation(std::move(location)));
			}
		}

		const UUID oldParentID = m_Registry.get<RelationshipComponent>(child.GetHandle()).Parent;
		const bool sameParent = oldParentID == newParentID;
		const uint32_t currentIndex = child.GetSiblingIndex();
		const size_t newListSize = GetChildList(newParentID).size() - (sameParent ? 1 : 0);
		const size_t requestedIndex = siblingIndex.value_or(std::numeric_limits<uint32_t>::max());
		const uint32_t targetIndex = static_cast<uint32_t>(std::min(requestedIndex, newListSize));
		if (sameParent && targetIndex == currentIndex)
			return {}; // already there: not a mutation

		// The world transform is preserved by re-expressing it in the new parent's space (§5.2). Under the same parent the
		// local transform already has the same world transform, so a reorder never touches it.
		std::optional<TransformDecomposition> newLocal;
		if (keepWorld && !sameParent)
		{
			const glm::mat4 world = TransformSystem::ComputeWorldMatrix(child);
			const glm::mat4 parentWorld = toRoot ? glm::mat4(1.0f) : TransformSystem::ComputeWorldMatrix(parent);
			Result<TransformDecomposition> local = TransformSystem::DecomposeMatrix(glm::affineInverse(parentWorld) * world);
			if (!local.has_value())
			{
				return std::unexpected(std::move(local.error())
						.WithContext(std::format("while keeping the world transform of '{}' under {}", GetEntityPath(child),
							toRoot ? std::string("the scene root") : std::format("'{}'", GetEntityPath(parent))))
						.WithHint("move it with keepWorld = false to keep its local transform instead"));
			}
			newLocal = *local;
		}

		RecordChildOrderBeforeChange(oldParentID);
		if (!sameParent)
			RecordChildOrderBeforeChange(newParentID);
		RecordFirstTouch(child.GetHandle());

		std::erase(GetChildList(oldParentID), childID);
		std::vector<UUID>& newSiblings = GetChildList(newParentID);
		newSiblings.insert(newSiblings.begin() + static_cast<std::ptrdiff_t>(targetIndex), childID);
		m_Registry.get<RelationshipComponent>(child.GetHandle()).Parent = newParentID;
		RecordComponentName(child.GetHandle(), TypeKeyOf<RelationshipComponent>());

		if (newLocal.has_value())
		{
			m_Registry.patch<TransformComponent>(child.GetHandle(), [&newLocal](TransformComponent& transform)
			{
				transform.Translation = newLocal->Translation;
				transform.Rotation = newLocal->Rotation;
				transform.Scale = newLocal->Scale;
			});
			RecordComponentName(child.GetHandle(), TypeKeyOf<TransformComponent>());
		}
		RefreshHierarchyDisabled(child.GetHandle());

		++m_Revision;
		m_CanonicalOrderDirty = true;
		return {};
	}

	std::span<const UUID> Scene::GetCanonicalOrder() const
	{
		AssertMainThread();
		if (m_CanonicalOrderDirty)
			RebuildCanonicalOrder();
		return m_CanonicalOrder;
	}

	size_t Scene::GetEntityCount() const
	{
		return m_EntityIndex.size();
	}

	void Scene::SetName(std::string name)
	{
		m_Specification.Name = std::move(name);
		++m_Revision;
	}

	void Scene::SetSeed(uint32_t seed)
	{
		m_Specification.Seed = seed;
		++m_Revision;
	}

	void Scene::SetInterpolationAlpha(float alpha)
	{
		ENGINE_CORE_ASSERT(alpha >= 0.0f && alpha <= 1.0f, "Interpolation alpha {} is outside [0, 1]", alpha);
		m_InterpolationAlpha = alpha;
	}

	void Scene::PrepareEntityChange(entt::entity entity, TypeKey component)
	{
		AssertMainThread();
		if (!IsTrackedComponent(component))
			return;

		++m_Revision;
		RecordFirstTouch(entity);
	}

	void Scene::CommitComponentChange(entt::entity entity, TypeKey component, ComponentChangeKind kind)
	{
		// Every entity always has these (the scene's invariant, §6.2 "every entity key is always written").
		ENGINE_CORE_ASSERT(kind != ComponentChangeKind::Removed
				|| (component != TypeKeyOf<IDComponent>() && component != TypeKeyOf<NameComponent>() && component != TypeKeyOf<TagsComponent>()
					&& component != TypeKeyOf<RelationshipComponent>() && component != TypeKeyOf<TransformComponent>()),
			"ID, Name, Tags, Relationship and Transform are never removed from an entity");
		if (!IsTrackedComponent(component))
			return;

		RecordComponentName(entity, component);
		if (component == TypeKeyOf<RelationshipComponent>())
			m_CanonicalOrderDirty = true;
		if (component == TypeKeyOf<DisabledTag>())
			RefreshHierarchyDisabled(entity);
	}

	void Scene::AssertMainThread() const
	{
		ENGINE_CORE_ASSERT(std::this_thread::get_id() == m_MainThread, "Scene '{}' is used off the thread that created it (§4.11)",
			m_Specification.Name);
	}

	bool Scene::IsTrackedComponent(TypeKey component) const
	{
		return component == TypeKeyOf<DisabledTag>() || m_Specification.Registry->FindComponentByKey(component) != nullptr;
	}

	entt::entity Scene::FindHandle(UUID id) const
	{
		const auto found = m_EntityIndex.find(id);
		if (found == m_EntityIndex.end())
			return entt::null;
		ENGINE_CORE_ASSERT(m_Registry.valid(found->second) && !m_Registry.all_of<PendingDestroyTag>(found->second),
			"The UUID index of scene '{}' names a dead entity for {}", m_Specification.Name, id);
		return found->second;
	}

	std::vector<UUID>& Scene::GetChildList(UUID parent)
	{
		if (!parent.IsValid())
			return m_RootEntities;
		const entt::entity handle = FindHandle(parent);
		ENGINE_CORE_ASSERT(handle != entt::null, "Parent {} is not an entity of scene '{}'", parent, m_Specification.Name);
		return m_Registry.get<RelationshipComponent>(handle).Children;
	}

	Entity Scene::CreateEntityInternal(UUID id, std::string_view name, entt::entity parent)
	{
		const UUID parentID = parent == entt::null ? UUID() : m_Registry.get<IDComponent>(parent).ID;
		RecordChildOrderBeforeChange(parentID);

		const entt::entity handle = m_Registry.create();
		m_Registry.emplace<IDComponent>(handle, id);
		m_Registry.emplace<NameComponent>(handle, std::string(name));
		m_Registry.emplace<TagsComponent>(handle);
		m_Registry.emplace<RelationshipComponent>(handle, parentID);
		m_Registry.emplace<TransformComponent>(handle);
		m_EntityIndex.emplace(id, handle);
		GetChildList(parentID).push_back(id);
		RefreshHierarchyDisabled(handle);

		if (m_ChangeTracker.IsTracking())
			m_ChangeTracker.RecordCreated(id);
		++m_Revision;
		m_CanonicalOrderDirty = true;
		return Entity(handle, this);
	}

	void Scene::RecordChildOrderBeforeChange(UUID parent)
	{
		if (m_ChangeTracker.NeedsChildOrder(parent))
			m_ChangeTracker.RecordChildOrder(parent, GetChildList(parent));
	}

	void Scene::RecordFirstTouch(entt::entity entity)
	{
		const UUID id = m_Registry.get<IDComponent>(entity).ID;
		if (!m_ChangeTracker.NeedsSnapshot(id))
			return;

		const UUID parent = m_Registry.get<RelationshipComponent>(entity).Parent;
		const std::vector<UUID>& siblings = GetChildList(parent);
		const auto position = std::ranges::find(siblings, id);
		ENGINE_CORE_ASSERT(position != siblings.end(), "Entity {} is missing from its parent's child list", id);
		m_ChangeTracker.RecordSnapshot(id, CaptureEntitySnapshot(entity), parent, static_cast<uint32_t>(position - siblings.begin()));
	}

	void Scene::RecordComponentName(entt::entity entity, TypeKey component)
	{
		if (!m_ChangeTracker.IsTracking())
			return;

		const UUID id = m_Registry.get<IDComponent>(entity).ID;
		if (component == TypeKeyOf<DisabledTag>())
		{
			m_ChangeTracker.RecordComponent(id, "Active");
			return;
		}
		if (const ComponentInfo* info = m_Specification.Registry->FindComponentByKey(component))
			m_ChangeTracker.RecordComponent(id, info->GetName());
	}

	std::vector<entt::entity> Scene::CollectSubtreeForDestruction(entt::entity root) const
	{
		// The reverse of the subtree's canonical (depth-first pre-order) order: every entity follows all of its descendants,
		// and later siblings' subtrees precede earlier ones.
		std::vector<entt::entity> order;
		std::vector<entt::entity> stack = { root };
		while (!stack.empty())
		{
			const entt::entity current = stack.back();
			stack.pop_back();
			order.push_back(current);
			const std::vector<UUID>& children = m_Registry.get<RelationshipComponent>(current).Children;
			for (auto child = children.rbegin(); child != children.rend(); ++child)
				stack.push_back(FindHandle(*child));
		}
		std::ranges::reverse(order);
		return order;
	}

	void Scene::RecordDestruction(std::span<const entt::entity> entities)
	{
		if (!m_ChangeTracker.IsTracking())
			return;

		// Everything is recorded before anything changes, so snapshots, parents and indices describe the scene as it was.
		for (const entt::entity handle : entities)
			RecordChildOrderBeforeChange(m_Registry.get<RelationshipComponent>(handle).Parent);
		for (const entt::entity handle : entities)
			RecordFirstTouch(handle);
		for (const entt::entity handle : entities)
			m_ChangeTracker.RecordDestroyed(m_Registry.get<IDComponent>(handle).ID);
	}

	void Scene::RebuildCanonicalOrder() const
	{
		m_CanonicalOrder.clear();
		m_CanonicalOrder.reserve(m_EntityIndex.size());

		// Depth-first pre-order: the stack holds the entities still to visit, each list pushed in reverse so the first
		// child is visited first.
		std::vector<UUID> stack(m_RootEntities.rbegin(), m_RootEntities.rend());
		while (!stack.empty())
		{
			const UUID id = stack.back();
			stack.pop_back();
			m_CanonicalOrder.push_back(id);
			const std::vector<UUID>& children = m_Registry.get<RelationshipComponent>(FindHandle(id)).Children;
			stack.insert(stack.end(), children.rbegin(), children.rend());
		}
		m_CanonicalOrderDirty = false;
	}

	void Scene::RefreshHierarchyDisabled(entt::entity root)
	{
		const UUID parent = m_Registry.get<RelationshipComponent>(root).Parent;
		const bool parentDisabled = parent.IsValid() && m_Registry.all_of<HierarchyDisabledTag>(FindHandle(parent));

		// (entity, whether an ancestor is disabled), depth-first.
		std::vector<std::pair<entt::entity, bool>> stack = { { root, parentDisabled } };
		while (!stack.empty())
		{
			const auto [handle, ancestorDisabled] = stack.back();
			stack.pop_back();
			const bool disabled = ancestorDisabled || m_Registry.all_of<DisabledTag>(handle);
			if (disabled && !m_Registry.all_of<HierarchyDisabledTag>(handle))
				m_Registry.emplace<HierarchyDisabledTag>(handle);
			else if (!disabled && m_Registry.all_of<HierarchyDisabledTag>(handle))
				m_Registry.remove<HierarchyDisabledTag>(handle);

			for (const UUID child : m_Registry.get<RelationshipComponent>(handle).Children)
				stack.emplace_back(FindHandle(child), disabled);
		}
	}

}
