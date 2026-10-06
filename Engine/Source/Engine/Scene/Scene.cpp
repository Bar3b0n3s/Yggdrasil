#include "EnginePCH.h"
#include "Engine/Scene/Scene.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"

// M3 contract stub (Roadmap rule 3): stream B (Scene core) implements entity lifetime, the UUID index, the hierarchy,
// canonical order, entity paths and the change hooks. The constructor, Create and the plain setters are complete.

namespace Engine {

	Scene::Scene(const SceneSpecification& specification)
		: m_Specification(specification)
	{
		ENGINE_CORE_ASSERT(m_Specification.Registry != nullptr, "A scene needs a type registry");
		ENGINE_CORE_ASSERT(m_Specification.IdGenerator != nullptr, "A scene needs a UUID generator");
	}

	Scene::~Scene() = default;

	Scope<Scene> Scene::Create(const SceneSpecification& specification)
	{
		return CreateScope<Scene>(specification);
	}

	Entity Scene::CreateEntity(std::string_view /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Entity Scene::CreateEntity(std::string_view /*name*/, Entity /*parent*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Entity Scene::CreateEntityWithID(UUID /*id*/, std::string_view /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Entity Scene::CreateEntityWithID(UUID /*id*/, std::string_view /*name*/, Entity /*parent*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void Scene::DestroyEntity(Entity /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Scene::FlushPendingDestroys()
	{
		ENGINE_CONTRACT_STUB();
	}

	void Scene::Clear()
	{
		ENGINE_CONTRACT_STUB();
	}

	Entity Scene::FindEntityByID(UUID /*id*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	ConstEntity Scene::FindEntityByID(UUID /*id*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Entity Scene::FindEntityByPath(std::string_view /*path*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	ConstEntity Scene::FindEntityByPath(std::string_view /*path*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<Entity> Scene::ResolveEntityPath(std::string_view /*path*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Scene::ResolveEntityPath is an M3 contract stub");
	}

	Result<ConstEntity> Scene::ResolveEntityPath(std::string_view /*path*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Scene::ResolveEntityPath is an M3 contract stub");
	}

	std::string Scene::GetEntityPath(ConstEntity /*entity*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status Scene::SetParent(Entity /*child*/, Entity /*parent*/, std::optional<uint32_t> /*siblingIndex*/, bool /*keepWorld*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Scene::SetParent is an M3 contract stub");
	}

	std::span<const UUID> Scene::GetCanonicalOrder() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	size_t Scene::GetEntityCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
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

	void Scene::PrepareEntityChange(entt::entity /*entity*/, TypeKey /*component*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Scene::CommitComponentChange(entt::entity /*entity*/, TypeKey /*component*/, ComponentChangeKind /*kind*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
