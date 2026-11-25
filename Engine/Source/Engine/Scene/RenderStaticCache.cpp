#include "EnginePCH.h"
#include "Engine/Scene/RenderStaticCache.h"

#include "Engine/Core/Assert.h"
#include "Engine/Scene/Components/IDComponent.h"
#include "Engine/Scene/Components/MaterialParametersComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PlanarReflectionComponent.h"
#include "Engine/Scene/Components/RenderDistanceComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/SkinnedMeshComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/StaticScenery.h"
#include "Engine/Scene/TransformSystem.h"

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// Simulation path (§4.12): the cell keys use floor and a division by a power of two, both exact, so a static entity lies in
// the same cell in every configuration.

namespace Engine {

	namespace Utils {

		// M43: dirty marks the cache keeps between two refreshes, as a world-change list keeps its entries (Scene.cpp), before
		// it drops them for a full pass.
		constexpr size_t StaticDirtyEntriesPerEntity = 4;
		constexpr size_t MinStaticDirtyEntries = 4096;

		// The padding-free layout the bitwise comparison of MeshMaterialParameters relies on.
		static_assert(sizeof(MeshMaterialParameters) == 15 * sizeof(float), "MeshMaterialParameters is compared bit for bit");
		static_assert(sizeof(MeshDrawDistance) == 4 * sizeof(float), "MeshDrawDistance is compared bit for bit"); // M46

		// Whether `entity` (a live entity) gives a static item (RenderStaticCache.h's membership).
		static bool IsStaticItem(const entt::registry& registry, entt::entity entity)
		{
			if (!registry.all_of<HierarchyStaticTag>(entity)
				|| registry.any_of<HierarchyDisabledTag, PendingDestroyTag, SkinnedMeshComponent, PlanarReflectionComponent>(entity))
				return false;
			const MeshRendererComponent* renderer = registry.try_get<MeshRendererComponent>(entity);
			if (renderer == nullptr || !renderer->Visible || !renderer->Mesh.IsValid())
				return false;
			const MaterialParametersComponent* parameters = registry.try_get<MaterialParametersComponent>(entity);
			return parameters == nullptr || (parameters->UVScroll.x == 0.0f && parameters->UVScroll.y == 0.0f);
		}

		struct StaticItem
		{
			MeshDrawItem Item{};
			bool ChainMatrix = false; // the entity had no WorldTransformComponent yet
		};

		// The static item of `entity` (IsStaticItem holds): render extraction's MeshRenderer item at the entity's current
		// world matrix, without interpolation, its MaterialParameters resolved at time 0 (they do not scroll).
		static StaticItem MakeStaticItem(const Scene& scene, entt::entity entity)
		{
			const entt::registry& registry = scene.GetRegistry();
			const MeshRendererComponent& renderer = registry.get<MeshRendererComponent>(entity);
			StaticItem made;
			MeshDrawItem& item = made.Item;
			if (const WorldTransformComponent* world = registry.try_get<WorldTransformComponent>(entity))
			{
				item.World = world->Matrix;
			}
			else
			{
				item.World = TransformSystem::ComputeWorldMatrix(ConstEntity(entity, &scene));
				made.ChainMatrix = true;
			}
			item.Mesh = renderer.Mesh.GetHandle();
			item.Materials.reserve(renderer.Materials.size());
			for (const TypedAssetHandle<AssetType::Material>& material : renderer.Materials)
				item.Materials.push_back(material.GetHandle());
			item.CastShadows = renderer.CastShadows;
			item.ReceiveShadows = renderer.ReceiveShadows;
			item.Entity = registry.get<IDComponent>(entity).ID;
			if (const MaterialParametersComponent* parameters = registry.try_get<MaterialParametersComponent>(entity))
				item.Parameters = ResolveMaterialParameters(*parameters, 0.0);
			// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3E, W46.4): its RenderDistance, as render
			// extraction gives a dynamic item its.
			if (const RenderDistanceComponent* distance = registry.try_get<RenderDistanceComponent>(entity))
			{
				item.Distance = MeshDrawDistance{ .CullDistance = distance->CullDistance, .LodBias = distance->LodBias, .ShadowLodOffset = distance->ShadowLodOffset, .FadeWidth = distance->FadeWidth };
			}
			return made;
		}

		// One coordinate of a cell key: floor(position / StaticCellSize), exact (a division by a power of two in double), a
		// non-finite position in cell 0 and a huge one clamped to the int32 range.
		static int32_t ToCellCoordinate(float position)
		{
			if (!std::isfinite(position))
				return 0;
			const double cell = std::floor(static_cast<double>(position) / static_cast<double>(StaticCellSize));
			constexpr double Lowest = static_cast<double>(std::numeric_limits<int32_t>::min());
			constexpr double Highest = static_cast<double>(std::numeric_limits<int32_t>::max());
			return static_cast<int32_t>(std::clamp(cell, Lowest, Highest));
		}

		// The cell of an item at `world`: its translation's x and z.
		static glm::ivec2 ComputeCellKey(const glm::mat4& world)
		{
			return glm::ivec2(ToCellCoordinate(world[3].x), ToCellCoordinate(world[3].z));
		}

		template<typename T>
		static bool SameBits(const T& first, const T& second)
		{
			static_assert(std::is_trivially_copyable_v<T>, "compared as bytes");
			return std::memcmp(&first, &second, sizeof(T)) == 0;
		}

		template<typename T>
		static bool SameBits(const std::vector<T>& first, const std::vector<T>& second)
		{
			static_assert(std::is_trivially_copyable_v<T>, "compared as bytes");
			return first.size() == second.size() && (first.empty() || std::memcmp(first.data(), second.data(), first.size() * sizeof(T)) == 0);
		}

		// The first field in which two items differ (floating-point fields bit for bit, so a write of the same bits is no
		// change), or an empty view when they are equal.
		static std::string_view FindItemDifference(const MeshDrawItem& first, const MeshDrawItem& second)
		{
			if (!SameBits(first.World, second.World))
				return "World";
			if (first.PreviousWorld.has_value() != second.PreviousWorld.has_value()
				|| (first.PreviousWorld.has_value() && !SameBits(*first.PreviousWorld, *second.PreviousWorld)))
				return "PreviousWorld";
			if (first.Mesh != second.Mesh)
				return "Mesh";
			if (first.Materials != second.Materials)
				return "Materials";
			if (first.CastShadows != second.CastShadows)
				return "CastShadows";
			if (first.ReceiveShadows != second.ReceiveShadows)
				return "ReceiveShadows";
			if (first.Entity != second.Entity)
				return "Entity";
			if (first.PickId != second.PickId)
				return "PickId";
			if (!SameBits(first.JointPalette, second.JointPalette))
				return "JointPalette";
			if (!SameBits(first.MorphWeights, second.MorphWeights))
				return "MorphWeights";
			if (!SameBits(first.Parameters, second.Parameters))
				return "Parameters";
			if (!SameBits(first.Distance, second.Distance))
				return "Distance"; // M46
			return {};
		}

#if defined(ENGINE_DEBUG)
		// "'<path>' (<id>)" of a live entity, or "<id>" of one the scene no longer holds.
		static std::string DescribeStaticEntity(const Scene& scene, UUID id)
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			return entity.IsValid() ? std::format("'{}' ({})", scene.GetEntityPath(entity), id) : std::format("{}", id);
		}
#endif

	}

	bool RenderStaticCache::CellKeyLess::operator()(const glm::ivec2& left, const glm::ivec2& right) const
	{
		return left.x != right.x ? left.x < right.x : left.y < right.y;
	}

	RenderStaticCache::RenderStaticCache(Scene& scene)
		: m_Scene(&scene)
	{
		entt::registry& registry = scene.GetRegistry();
		const auto connect = [this, &registry]<typename T>(std::type_identity<T>)
		{
			registry.on_construct<T>().template connect<&RenderStaticCache::OnChanged>(*this);
			registry.on_update<T>().template connect<&RenderStaticCache::OnChanged>(*this);
			registry.on_destroy<T>().template connect<&RenderStaticCache::OnChanged>(*this);
		};
		connect(std::type_identity<MeshRendererComponent>{});
		connect(std::type_identity<MaterialParametersComponent>{});
		connect(std::type_identity<SkinnedMeshComponent>{});
		connect(std::type_identity<PlanarReflectionComponent>{});
		connect(std::type_identity<RenderDistanceComponent>{}); // M46
		connect(std::type_identity<HierarchyStaticTag>{});
		connect(std::type_identity<HierarchyDisabledTag>{});
		registry.on_construct<PendingDestroyTag>().connect<&RenderStaticCache::OnChanged>(*this);
		m_WorldChangeList = scene.OpenWorldChangeList();
		m_StructureRevision = scene.GetStructureRevision();
	}

	RenderStaticCache::~RenderStaticCache()
	{
		entt::registry& registry = m_Scene->GetRegistry();
		const auto disconnect = [this, &registry]<typename T>(std::type_identity<T>)
		{
			registry.on_construct<T>().disconnect(this);
			registry.on_update<T>().disconnect(this);
			registry.on_destroy<T>().disconnect(this);
		};
		disconnect(std::type_identity<MeshRendererComponent>{});
		disconnect(std::type_identity<MaterialParametersComponent>{});
		disconnect(std::type_identity<SkinnedMeshComponent>{});
		disconnect(std::type_identity<PlanarReflectionComponent>{});
		disconnect(std::type_identity<RenderDistanceComponent>{}); // M46
		disconnect(std::type_identity<HierarchyStaticTag>{});
		disconnect(std::type_identity<HierarchyDisabledTag>{});
		registry.on_construct<PendingDestroyTag>().disconnect(this);
		m_Scene->CloseWorldChangeList(m_WorldChangeList);
	}

	Ref<const RenderStaticSet> RenderStaticCache::Refresh()
	{
		Scene& scene = *m_Scene;
		const entt::registry& registry = scene.GetRegistry();
		m_LastCellRebuilds = 0;

		// What may have changed: the dirty entities, those of the world changes that are static or held, and those still on
		// their chain's matrix; after an overflow (or at the first refresh) every static entity and every held item.
		m_WorldChanges.clear();
		if (!scene.TakeWorldChanges(m_WorldChangeList, m_WorldChanges))
			m_RebuildAll = true;
		if (m_RebuildAll)
		{
			// Evaluate, ClaimSlot, the release and the order check dirty exactly the cells that change, so the full pass
			// rebuilds only those.
			m_Dirty.clear();
			for (const entt::entity entity : registry.view<HierarchyStaticTag>())
				m_Dirty.push_back(entity);
			for (const EntitySlot& slot : m_Slots)
			{
				if (slot.Item != NoItem)
					m_Dirty.push_back(slot.Handle);
			}
		}
		else
		{
			for (const entt::entity entity : m_WorldChanges)
			{
				if (HoldsItem(entity) || (registry.valid(entity) && registry.all_of<HierarchyStaticTag>(entity)))
					m_Dirty.push_back(entity);
			}
		}
		m_RebuildAll = false;
		m_Dirty.insert(m_Dirty.end(), m_ChainMatrixEntities.begin(), m_ChainMatrixEntities.end());
		m_ChainMatrixEntities.clear();
		std::ranges::sort(m_Dirty);
		m_Dirty.erase(std::unique(m_Dirty.begin(), m_Dirty.end()), m_Dirty.end());
		for (const entt::entity entity : m_Dirty)
			Evaluate(entity);
		m_Dirty.clear();

		// Only a reparent or a sibling move reorders entities that stay, and neither need change a matrix.
		if (scene.GetStructureRevision() != m_StructureRevision)
		{
			m_StructureRevision = scene.GetStructureRevision();
			for (auto& [key, cell] : m_Cells)
			{
				if (!cell.Dirty && IsOutOfOrder(cell))
					cell.Dirty = true;
			}
		}

		bool changed = m_Set == nullptr;
		for (auto cell = m_Cells.begin(); cell != m_Cells.end();)
		{
			if (!cell->second.Dirty)
			{
				++cell;
				continue;
			}
			++m_LastCellRebuilds;
			changed = true;
			RebuildCell(cell->first, cell->second);
			if (cell->second.Published == nullptr)
				cell = m_Cells.erase(cell);
			else
				++cell;
		}
		if (changed)
		{
			Ref<RenderStaticSet> set = CreateRef<RenderStaticSet>();
			set->Cells.reserve(m_Cells.size());
			for (const auto& [key, cell] : m_Cells)
				set->Cells.push_back(cell.Published);
			set->Version = ++m_LastVersion;
			m_Set = std::move(set);
		}
		m_CellRebuilds += m_LastCellRebuilds;
#if defined(ENGINE_DEBUG)
		VerifyAgainstFullRebuild();
#endif
		return m_Set;
	}

	bool RenderStaticCache::HoldsItem(entt::entity entity) const
	{
		const EntitySlot* slot = FindSlot(entity);
		return slot != nullptr && slot->Item != NoItem;
	}

	void RenderStaticCache::OnChanged(entt::registry& registry, entt::entity entity)
	{
		if (m_RebuildAll)
			return; // the next refresh evaluates everything anyway
		// Only a static entity, or one whose item the set holds, can change the set. EnTT publishes a construction after the
		// emplace and a destruction before the pop, so a static entity still holds its tag when its signal fires (its tag's
		// own destruction included); a held item is caught by its handle even when a destroy cleared the tag's pool first;
		// and an entity that becomes static is marked when its tag is constructed.
		if (!registry.all_of<HierarchyStaticTag>(entity) && !HoldsItem(entity))
			return;
		const size_t capacity = std::max(Utils::MinStaticDirtyEntries, Utils::StaticDirtyEntriesPerEntity * m_Scene->GetEntityCount());
		if (m_Dirty.size() >= capacity)
		{
			m_Dirty = {};
			m_RebuildAll = true;
			return;
		}
		m_Dirty.push_back(entity);
	}

	void RenderStaticCache::Evaluate(entt::entity entity)
	{
		const entt::registry& registry = m_Scene->GetRegistry();
		if (!registry.valid(entity) || !Utils::IsStaticItem(registry, entity))
		{
			const auto index = static_cast<size_t>(entt::to_entity(entity));
			if (index < m_Slots.size() && m_Slots[index].Handle == entity && m_Slots[index].Item != NoItem)
			{
				MarkCellDirty(m_Slots[index].Cell);
				ReleaseItem(m_Slots[index]);
			}
			return;
		}

		Utils::StaticItem made = Utils::MakeStaticItem(*m_Scene, entity);
		if (made.ChainMatrix)
			m_ChainMatrixEntities.push_back(entity);
		const glm::ivec2 key = Utils::ComputeCellKey(made.Item.World);
		EntitySlot& slot = ClaimSlot(entity);
		if (slot.Item != NoItem)
		{
			MeshDrawItem& current = m_Items[slot.Item];
			const bool sameCell = slot.Cell == key;
			if (sameCell && Utils::FindItemDifference(current, made.Item).empty())
				return; // written to the same bits: nothing to rebuild
			MarkCellDirty(slot.Cell);
			current = std::move(made.Item);
			if (sameCell)
				return;
		}
		else
		{
			slot.Item = AllocateItem(std::move(made.Item));
		}
		slot.Cell = key;
		CellState& cell = m_Cells[key];
		cell.Entities.push_back(entity);
		cell.Dirty = true;
	}

	const RenderStaticCache::EntitySlot* RenderStaticCache::FindSlot(entt::entity entity) const
	{
		const auto index = static_cast<size_t>(entt::to_entity(entity));
		return index < m_Slots.size() && m_Slots[index].Handle == entity ? &m_Slots[index] : nullptr;
	}

	RenderStaticCache::EntitySlot& RenderStaticCache::ClaimSlot(entt::entity entity)
	{
		const auto index = static_cast<size_t>(entt::to_entity(entity));
		if (index >= m_Slots.size())
			m_Slots.resize(index + 1);
		EntitySlot& slot = m_Slots[index];
		if (slot.Handle != entity)
		{
			if (slot.Item != NoItem)
			{
				MarkCellDirty(slot.Cell);
				ReleaseItem(slot);
			}
			slot = EntitySlot{ .Handle = entity };
		}
		return slot;
	}

	uint32_t RenderStaticCache::AllocateItem(MeshDrawItem item)
	{
		if (!m_FreeItems.empty())
		{
			const uint32_t index = m_FreeItems.back();
			m_FreeItems.pop_back();
			m_Items[index] = std::move(item);
			return index;
		}
		m_Items.push_back(std::move(item));
		return static_cast<uint32_t>(m_Items.size() - 1);
	}

	void RenderStaticCache::ReleaseItem(EntitySlot& slot)
	{
		m_Items[slot.Item] = MeshDrawItem{};
		m_FreeItems.push_back(slot.Item);
		slot.Item = NoItem;
	}

	void RenderStaticCache::MarkCellDirty(const glm::ivec2& key)
	{
		const auto found = m_Cells.find(key);
		ENGINE_CORE_ASSERT(found != m_Cells.end(), "A static item lies in cell ({}, {}), which the cache does not hold", key.x, key.y);
		if (found != m_Cells.end())
			found->second.Dirty = true;
	}

	bool RenderStaticCache::IsOutOfOrder(const CellState& cell) const
	{
		const Scene& scene = *m_Scene;
		for (size_t index = 1; index < cell.Entities.size(); ++index)
		{
			if (!(scene.GetCanonicalKey(ConstEntity(cell.Entities[index - 1], &scene)) < scene.GetCanonicalKey(ConstEntity(cell.Entities[index], &scene))))
				return true;
		}
		return false;
	}

	void RenderStaticCache::RebuildCell(const glm::ivec2& key, CellState& cell)
	{
		std::erase_if(cell.Entities, [this, &key](entt::entity entity)
		{
			const EntitySlot* slot = FindSlot(entity);
			return slot == nullptr || slot->Item == NoItem || slot->Cell != key;
		});
		cell.Dirty = false;
		if (cell.Entities.empty())
		{
			cell.Published = nullptr;
			return;
		}
		m_Scene->SortCanonical(cell.Entities);
		Ref<RenderStaticCell> published = CreateRef<RenderStaticCell>();
		published->Key = key;
		published->Version = ++m_LastVersion;
		published->Items.reserve(cell.Entities.size());
		published->PickTable.reserve(cell.Entities.size());
		for (const entt::entity entity : cell.Entities)
		{
			const MeshDrawItem& item = m_Items[FindSlot(entity)->Item];
			published->Items.push_back(item);
			published->PickTable.push_back(item.Entity);
			published->Dissolving = published->Dissolving || item.Parameters.DissolveAmount > 0.0f;
		}
		cell.Published = std::move(published);
	}

#if defined(ENGINE_DEBUG)
	void RenderStaticCache::VerifyAgainstFullRebuild() const
	{
		const Scene& scene = *m_Scene;
		// The set follows the tags, so the tags are checked against the Static rule first.
		VerifyHierarchyStaticTags(scene);
		const entt::registry& registry = scene.GetRegistry();
		std::vector<entt::entity> candidates;
		scene.CollectCanonical<HierarchyStaticTag, MeshRendererComponent>(candidates);
		std::map<glm::ivec2, std::vector<MeshDrawItem>, CellKeyLess> expected;
		size_t expectedItems = 0;
		for (const entt::entity entity : candidates)
		{
			if (!Utils::IsStaticItem(registry, entity))
				continue;
			Utils::StaticItem made = Utils::MakeStaticItem(scene, entity);
			expected[Utils::ComputeCellKey(made.Item.World)].push_back(std::move(made.Item));
			++expectedItems;
		}

		constexpr std::string_view Hint = "a MeshRenderer, MaterialParameters, SkinnedMesh or PlanarReflection was written past its EnTT signals (write it "
										  "through Entity::Patch), or the cache missed a change";
		const std::vector<Ref<const RenderStaticCell>>& cells = m_Set->Cells;
		ENGINE_CORE_VERIFY(cells.size() == expected.size(), "Stale static set of scene '{}': it holds {} cells where a full rebuild holds {}: {}",
			scene.GetName(), cells.size(), expected.size(), Hint);
		ENGINE_CORE_VERIFY(GetItemCount() == expectedItems, "Stale static set of scene '{}': it holds {} items where a full rebuild holds {}: {}",
			scene.GetName(), GetItemCount(), expectedItems, Hint);
		size_t index = 0;
		for (const auto& [key, items] : expected)
		{
			const RenderStaticCell& cell = *cells[index++];
			ENGINE_CORE_VERIFY(cell.Key == key, "Stale static set of scene '{}': it holds cell ({}, {}) where a full rebuild holds ({}, {}): {}",
				scene.GetName(), cell.Key.x, cell.Key.y, key.x, key.y, Hint);
			ENGINE_CORE_VERIFY(cell.Items.size() == items.size() && cell.PickTable.size() == items.size(),
				"Stale static set of scene '{}': cell ({}, {}) holds {} items and {} pick entries where a full rebuild holds {} items: {}",
				scene.GetName(), key.x, key.y, cell.Items.size(), cell.PickTable.size(), items.size(), Hint);
			for (size_t item = 0; item < items.size(); ++item)
			{
				const std::string_view difference = Utils::FindItemDifference(cell.Items[item], items[item]);
				ENGINE_CORE_VERIFY(difference.empty(),
					"Stale static set of scene '{}': item {} of cell ({}, {}), entity {}, differs in {} from a full rebuild's, entity {}: {}",
					scene.GetName(), item, key.x, key.y, Utils::DescribeStaticEntity(scene, cell.Items[item].Entity), difference,
					Utils::DescribeStaticEntity(scene, items[item].Entity), Hint);
				ENGINE_CORE_VERIFY(cell.PickTable[item] == items[item].Entity,
					"Stale static set of scene '{}': pick entry {} of cell ({}, {}) is {} where its item's entity is {}", scene.GetName(), item,
					key.x, key.y, cell.PickTable[item], items[item].Entity);
			}
			const bool dissolving = std::ranges::any_of(items, [](const MeshDrawItem& item)
			{
				return item.Parameters.DissolveAmount > 0.0f;
			});
			ENGINE_CORE_VERIFY(cell.Dissolving == dissolving, "Stale static set of scene '{}': cell ({}, {}) has Dissolving {} where its items say {}",
				scene.GetName(), key.x, key.y, cell.Dissolving, dissolving);
		}
	}
#endif

}
