#pragma once

#include "Engine/Core/Assert.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/TypeInfo.h"
#include "Engine/Scene/Scene.h"

#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Engine {

	// A read-only handle to an entity of a scene: {entt::entity, const Scene*}, what the const members of Scene return
	// (FindEntityByID, FindEntityByPath, ResolveEntityPath, ForEachCanonical) and what read-only code takes
	// (SceneSerializer::EntityToJson, ComponentHostOps::Has and GetConst, the TransformSystem getters). Every Entity
	// converts to it implicitly; the reverse conversion does not exist. Validity rules and the component-reference warning
	// are Entity's. Main thread only, like the scene.
	class ConstEntity
	{
	public:
		ConstEntity() = default;
		ConstEntity(entt::entity handle, const Scene* scene)
			: m_Handle(handle), m_Scene(scene)
		{
		}

		// True when the handle names a live entity of a live registry that is not marked for destruction.
		[[nodiscard]] bool IsValid() const;
		explicit operator bool() const { return IsValid(); }

		[[nodiscard]] UUID GetUUID() const;
		[[nodiscard]] const std::string& GetName() const;

		// The component T (asserted present).
		template<typename T>
		[[nodiscard]] const T& GetComponent() const;

		// The component T, or nullptr when absent.
		template<typename T>
		[[nodiscard]] const T* TryGetComponent() const;

		template<typename T>
		[[nodiscard]] bool HasComponent() const;

		// Hierarchy, active state and tags, as the Entity members of the same names.
		[[nodiscard]] ConstEntity GetParent() const;
		[[nodiscard]] std::span<const UUID> GetChildren() const;
		[[nodiscard]] uint32_t GetSiblingIndex() const;
		[[nodiscard]] bool IsActiveSelf() const;
		[[nodiscard]] bool IsActive() const;
		[[nodiscard]] bool HasTag(std::string_view tag) const;
		[[nodiscard]] std::span<const std::string> GetTags() const;

		[[nodiscard]] entt::entity GetHandle() const { return m_Handle; }
		[[nodiscard]] const Scene* GetScene() const { return m_Scene; }

		// Same handle in the same scene.
		[[nodiscard]] bool operator==(const ConstEntity& other) const = default;
	private:
		void AssertValid() const;
	private:
		entt::entity m_Handle{ entt::null };
		const Scene* m_Scene = nullptr; // back-reference (§4.7)
	};

	// A handle to an entity of a scene (Architecture §5.1): {entt::entity, Scene*}, a small value type that is cheap to copy
	// and compare, passed by value. It does not own anything; it stays valid until the entity is destroyed (for a runtime
	// scene: until it is marked for destruction). Every accessor asserts validity; IsValid is the only member that accepts
	// an invalid handle. Mutating members are const because they change the scene, not the handle.
	//
	// Never keep a component reference across a structural change (adding or removing components of the same type, or
	// creating and destroying entities): EnTT storage moves (§4.7). Store the Entity or the UUID instead.
	//
	// Components are mutated through AddComponent, RemoveComponent and Patch, which notify the scene's change tracker and
	// increment its revision (§5.1) unless the component is runtime-only (RuntimeComponents.h); GetComponent returns a
	// mutable reference for runtime systems, which may write components directly. Main thread only, like the scene.
	class Entity
	{
	public:
		Entity() = default;
		Entity(entt::entity handle, Scene* scene)
			: m_Handle(handle), m_Scene(scene)
		{
		}

		// True when the handle names a live entity of a live registry that is not marked for destruction.
		[[nodiscard]] bool IsValid() const;
		explicit operator bool() const { return IsValid(); }

		// Implicit on purpose: a mutable handle is usable wherever a read-only one is expected, like T* to const T*.
		operator ConstEntity() const { return ConstEntity(m_Handle, m_Scene); }

		[[nodiscard]] UUID GetUUID() const;
		[[nodiscard]] const std::string& GetName() const;
		void SetName(std::string_view name) const;

		// Adds component T constructed from `args` and returns a reference to it (nothing for an empty tag type). Asserts a
		// valid entity that does not have T yet. Recorded by the change tracker, and counted in the revision, when T is a
		// registered component or DisabledTag; runtime-only components (RuntimeComponents.h) are neither.
		template<typename T, typename... Args>
		decltype(auto) AddComponent(Args&&... args) const;

		// The component T (asserted present). Writing through the reference is the runtime-system path: it bypasses the
		// change tracker; editor and automation code use Patch.
		template<typename T>
		[[nodiscard]] T& GetComponent() const;

		// The component T, or nullptr when absent.
		template<typename T>
		[[nodiscard]] T* TryGetComponent() const;

		template<typename T>
		[[nodiscard]] bool HasComponent() const;

		// Removes component T (asserted present). Removing a Required component (ID, Name, Relationship, Transform) or Tags
		// is a programmer error, asserted by the scene's change hook. Recorded and counted as AddComponent.
		template<typename T>
		void RemoveComponent() const;

		// Calls `func(T&)` on the component T through entt::registry::patch, which fires EnTT's on_update, and records the
		// change (§5.1) as AddComponent does. Asserts that T is present.
		template<typename T, typename Func>
		void Patch(Func&& func) const;

		// Hierarchy (RelationshipComponent). GetParent is invalid for a root; GetChildren is the ordered child list, valid
		// until the next structural change; GetSiblingIndex is the position among the parent's children or the roots.
		[[nodiscard]] Entity GetParent() const;
		[[nodiscard]] std::span<const UUID> GetChildren() const;
		[[nodiscard]] uint32_t GetSiblingIndex() const;

		// Active state (§5.2): IsActiveSelf is false when the entity has DisabledTag; IsActive also considers every ancestor.
		// SetActive adds or removes DisabledTag (recorded; increments the revision). OnEnable/OnDisable dispatch is the
		// play session's (M7).
		[[nodiscard]] bool IsActiveSelf() const;
		[[nodiscard]] bool IsActive() const;
		void SetActive(bool active) const;

		// Tags (TagsComponent). AddTag ignores a tag the entity already has; RemoveTag ignores one it does not have; tags
		// keep insertion order. An empty tag is asserted against (callers validate input).
		[[nodiscard]] bool HasTag(std::string_view tag) const;
		void AddTag(std::string_view tag) const;
		void RemoveTag(std::string_view tag) const;
		[[nodiscard]] std::span<const std::string> GetTags() const;

		[[nodiscard]] entt::entity GetHandle() const { return m_Handle; }
		[[nodiscard]] Scene* GetScene() const { return m_Scene; }

		// Same handle in the same scene.
		[[nodiscard]] bool operator==(const Entity& other) const = default;
	private:
		void AssertValid() const;
	private:
		entt::entity m_Handle{ entt::null };
		Scene* m_Scene = nullptr; // back-reference (§4.7)
	};

	template<typename T>
	const T& ConstEntity::GetComponent() const
	{
		static_assert(!std::is_empty_v<T>, "Tag components have no data; use HasComponent");
		AssertValid();
		ENGINE_CORE_ASSERT(HasComponent<T>(), "The entity does not have this component");
		return m_Scene->GetRegistry().get<T>(m_Handle);
	}

	template<typename T>
	const T* ConstEntity::TryGetComponent() const
	{
		static_assert(!std::is_empty_v<T>, "Tag components have no data; use HasComponent");
		AssertValid();
		return m_Scene->GetRegistry().try_get<T>(m_Handle);
	}

	template<typename T>
	bool ConstEntity::HasComponent() const
	{
		AssertValid();
		return m_Scene->GetRegistry().all_of<T>(m_Handle);
	}

	template<typename T, typename... Args>
	decltype(auto) Entity::AddComponent(Args&&... args) const
	{
		AssertValid();
		ENGINE_CORE_ASSERT(!HasComponent<T>(), "The entity already has this component");
		m_Scene->PrepareEntityChange(m_Handle, TypeKeyOf<T>());
		if constexpr (std::is_empty_v<T>)
		{
			m_Scene->m_Registry.emplace<T>(m_Handle);
			m_Scene->CommitComponentChange(m_Handle, TypeKeyOf<T>(), ComponentChangeKind::Added);
		}
		else
		{
			T& component = m_Scene->m_Registry.emplace<T>(m_Handle, std::forward<Args>(args)...);
			m_Scene->CommitComponentChange(m_Handle, TypeKeyOf<T>(), ComponentChangeKind::Added);
			return component;
		}
	}

	template<typename T>
	T& Entity::GetComponent() const
	{
		static_assert(!std::is_empty_v<T>, "Tag components have no data; use HasComponent");
		AssertValid();
		ENGINE_CORE_ASSERT(HasComponent<T>(), "The entity does not have this component");
		return m_Scene->m_Registry.get<T>(m_Handle);
	}

	template<typename T>
	T* Entity::TryGetComponent() const
	{
		static_assert(!std::is_empty_v<T>, "Tag components have no data; use HasComponent");
		AssertValid();
		return m_Scene->m_Registry.try_get<T>(m_Handle);
	}

	template<typename T>
	bool Entity::HasComponent() const
	{
		AssertValid();
		return m_Scene->m_Registry.all_of<T>(m_Handle);
	}

	template<typename T>
	void Entity::RemoveComponent() const
	{
		AssertValid();
		ENGINE_CORE_ASSERT(HasComponent<T>(), "The entity does not have this component");
		m_Scene->PrepareEntityChange(m_Handle, TypeKeyOf<T>());
		m_Scene->m_Registry.remove<T>(m_Handle);
		m_Scene->CommitComponentChange(m_Handle, TypeKeyOf<T>(), ComponentChangeKind::Removed);
	}

	template<typename T, typename Func>
	void Entity::Patch(Func&& func) const
	{
		static_assert(!std::is_empty_v<T>, "Tag components have no data to patch");
		AssertValid();
		ENGINE_CORE_ASSERT(HasComponent<T>(), "The entity does not have this component");
		m_Scene->PrepareEntityChange(m_Handle, TypeKeyOf<T>());
		m_Scene->m_Registry.patch<T>(m_Handle, std::forward<Func>(func));
		m_Scene->CommitComponentChange(m_Handle, TypeKeyOf<T>(), ComponentChangeKind::Patched);
	}

	template<typename Func>
	void Scene::ForEachCanonical(Func&& func)
	{
		const std::span<const UUID> current = GetCanonicalOrder();
		const std::vector<UUID> order(current.begin(), current.end());
		for (const UUID id : order)
		{
			const Entity entity = FindEntityByID(id);
			if (entity.IsValid())
				func(entity);
		}
	}

	template<typename Func>
	void Scene::ForEachCanonical(Func&& func) const
	{
		const std::span<const UUID> current = GetCanonicalOrder();
		const std::vector<UUID> order(current.begin(), current.end());
		for (const UUID id : order)
		{
			const ConstEntity entity = FindEntityByID(id);
			if (entity.IsValid())
				func(entity);
		}
	}

}
