#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStaticSet.h"

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

// The scene's render static cache (M43, Docs/Decisions/0061-m43-static-scenery.md decision 3; Architecture §8.2): keeps the
// scene's RenderStaticSet (Renderer/RenderStaticSet.h) up to date between frames, so render extraction hands every view the
// same immutable cells and visits only the dynamic items.
//
// Membership. An item for every entity that is effectively static (HierarchyStaticTag), effectively enabled (no
// HierarchyDisabledTag), not pending destruction, and holds a MeshRenderer that is Visible with a Mesh, except an entity that
// also holds a SkinnedMesh, a PlanarReflection, or a MaterialParameters whose UVScroll is not zero: those stay in
// RenderSnapshot::Meshes. Whether an item morphs depends also on its mesh (morph targets with a non-zero default weight
// deform an item that sets no weight), which extraction never reads: such an item joins the set, and the renderer draws it
// undeformed and reports it (Docs/Decisions/0061-m43-static-scenery.md, the W43.2 record). The item is render
// extraction's MeshRenderer item at the entity's current world matrix (its WorldTransformComponent, or
// TransformSystem::ComputeWorldMatrix before the first TransformSystem::Update gave it one), with no interpolation and no
// PreviousWorld, its MaterialParameters resolved at time 0 (ResolveMaterialParameters: without a scroll they are the same
// at any time), and PickId 0.
//
// Cells. Each item lies in the cell of its world origin: (floor(x / StaticCellSize), floor(z / StaticCellSize)) of its
// matrix's translation, a non-finite coordinate in cell 0 and a huge one clamped to the int32 range. The set lists its cells
// in key order, x then z, ascending and signed, and a cell lists its items in canonical order of their entities (§5.1), each
// item's entity at the same index of the cell's PickTable.
//
// Change feed. The cache opens a world-change list (Scene::OpenWorldChangeList) for the transforms, and listens to EnTT's
// on_construct, on_update and on_destroy of MeshRenderer, MaterialParameters, SkinnedMesh, PlanarReflection,
// HierarchyStaticTag and HierarchyDisabledTag, and to PendingDestroyTag's on_construct, marking the entity dirty when it is
// static or its item is held (a dynamic entity's changes cost a tag lookup and are not recorded). A component
// written past those signals (through a mutable reference; no engine path writes these components so) is missed, which the
// Debug check below reports. Dirty marks beyond four an entity of the scene (at least 4,096) between two refreshes are
// dropped for a full pass, as an overflowed world-change list is.
//
// Refresh. Each dirty entity's membership and item are computed again; an item whose cell, matrix bits or content changed
// marks its old and new cells dirty, and an item written to the same bits rebuilds nothing. When the scene's structure
// revision moved, every cell's canonical order is checked again (a key lookup an item) and a cell that a reparent or a
// sibling move reordered is dirty too. Each dirty cell is published again as a new RenderStaticCell with a new Version (copy
// on write: a cell a snapshot holds is never modified), or removed once it holds no item; the set gets a new Ref and Version
// whenever a cell was rebuilt or removed, and otherwise the same Ref is returned. The first refresh, an overflowed
// world-change list and dropped dirty marks evaluate every static entity and every held item again, which rebuilds only the
// cells whose items changed. Versions come from one counter of the cache, from 1, shared by its cells and sets: unique only
// within this scene's set, for change detection within it (the edit scene and its play copy hand out the same versions);
// what a renderer prepares from a cell is keyed by the cell object, the Ref<const RenderStaticCell> it holds while cached,
// and its meshes' and materials' mirror versions (an asset reload rebuilds no cell).
//
// The Debug check (ENGINE_DEBUG builds, Dependencies.lua): each refresh first checks every HierarchyStaticTag against the
// Static rule (VerifyHierarchyStaticTags, StaticScenery.h), since the set follows the tags, then builds the whole set afresh
// from the scene (Scene::CollectCanonical over HierarchyStaticTag and MeshRenderer) and ENGINE_CORE_VERIFYs that it equals
// the cached one: the cells and their keys, each item field by field, the pick tables and each cell's Dissolving, naming the
// first difference.
//
// Ownership: the Scene creates its cache at its first Static (Scene::CommitComponentChange) and owns it; extraction reaches
// it through Scene::RefreshStaticSet and Scene::GetStaticCache. Neither the set nor the cache is serialized or hashed. Main
// thread only, like the scene (§4.11).

namespace Engine {

	class Scene;

	class RenderStaticCache
	{
	public:
		// Connects the signals and opens the world-change list of `scene`, which outlives the cache (a documented
		// back-reference, §4.7). The first Refresh builds every cell.
		explicit RenderStaticCache(Scene& scene);
		~RenderStaticCache();

		RenderStaticCache(const RenderStaticCache&) = delete;
		RenderStaticCache& operator=(const RenderStaticCache&) = delete;
		RenderStaticCache(RenderStaticCache&&) = delete;
		RenderStaticCache& operator=(RenderStaticCache&&) = delete;

		// Brings the set up to date with the scene (the file comment's refresh) and returns it: never null, an empty set while
		// no entity qualifies.
		[[nodiscard]] Ref<const RenderStaticSet> Refresh();

		// Whether the set holds `entity`'s MeshRenderer as a static item, as of the last Refresh.
		[[nodiscard]] bool HoldsItem(entt::entity entity) const;

		// The items of the last Refresh's set.
		[[nodiscard]] size_t GetItemCount() const { return m_Items.size() - m_FreeItems.size(); }
		// The cells the last Refresh rebuilt or removed, whichever reader refreshed.
		[[nodiscard]] uint32_t GetLastCellRebuilds() const { return m_LastCellRebuilds; }
		// The cells every Refresh rebuilt or removed, summed over the cache's life (monotonic), so a play session counts those
		// of a tick's window (PlaySessionTickWork::StaticCellRebuilds, the readout's StaticCellRebuilds).
		[[nodiscard]] uint64_t GetStaticCellRebuilds() const { return m_CellRebuilds; }
	private:
		static constexpr uint32_t NoItem = std::numeric_limits<uint32_t>::max();

		// One entity's record, at its index (entt::to_entity) with its handle, so a recycled index never reads its
		// predecessor's item. Item indexes m_Items.
		struct EntitySlot
		{
			entt::entity Handle{ entt::null };
			uint32_t Item = NoItem;
			glm::ivec2 Cell = glm::ivec2(0);
		};
		// Cell keys in the set's order: x, then z.
		struct CellKeyLess
		{
			[[nodiscard]] bool operator()(const glm::ivec2& left, const glm::ivec2& right) const;
		};
		struct CellState
		{
			// The entities whose items lie in the cell, in canonical order once it is rebuilt; a dirty cell may also list
			// entities that left it, which its rebuild drops.
			std::vector<entt::entity> Entities{};
			Ref<const RenderStaticCell> Published{};
			bool Dirty = false;
		};

		// The listener of every connected signal: marks `entity` dirty.
		void OnChanged(entt::registry& registry, entt::entity entity);
		// Computes `entity`'s membership and item again and updates its slot and cells.
		void Evaluate(entt::entity entity);
		// The slot of `entity`, or nullptr when its index holds another entity's record or none.
		[[nodiscard]] const EntitySlot* FindSlot(entt::entity entity) const;
		// The slot of `entity`, (re)claimed for it: a previous holder's item at the same index leaves its cell.
		[[nodiscard]] EntitySlot& ClaimSlot(entt::entity entity);
		[[nodiscard]] uint32_t AllocateItem(MeshDrawItem item);
		void ReleaseItem(EntitySlot& slot);
		void MarkCellDirty(const glm::ivec2& key);
		// Whether the entities of the clean cell `cell` are no longer in canonical order.
		[[nodiscard]] bool IsOutOfOrder(const CellState& cell) const;
		// Drops the entities that left the cell and, unless none is left, publishes it again in canonical order.
		void RebuildCell(const glm::ivec2& key, CellState& cell);
#if defined(ENGINE_DEBUG)
		// The Debug check of the file comment.
		void VerifyAgainstFullRebuild() const;
#endif
	private:
		Scene* m_Scene = nullptr; // back-reference: the owning scene outlives its cache
		uint32_t m_WorldChangeList = 0;
		std::vector<EntitySlot> m_Slots;
		std::vector<MeshDrawItem> m_Items; // the items by slot, reused through m_FreeItems
		std::vector<uint32_t> m_FreeItems;
		std::vector<entt::entity> m_Dirty; // marked since the last refresh, possibly repeated or destroyed since
		// Entities whose items took TransformSystem::ComputeWorldMatrix (no WorldTransformComponent yet): evaluated again at
		// every refresh, since their chain's writes reach no world-change list before their first update.
		std::vector<entt::entity> m_ChainMatrixEntities;
		std::vector<entt::entity> m_WorldChanges; // scratch for Scene::TakeWorldChanges
		std::map<glm::ivec2, CellState, CellKeyLess> m_Cells;
		Ref<const RenderStaticSet> m_Set;
		uint64_t m_LastVersion = 0;       // the latest cell or set Version handed out
		uint64_t m_StructureRevision = 0; // the scene's at the last refresh
		uint32_t m_LastCellRebuilds = 0;
		uint64_t m_CellRebuilds = 0;
		bool m_RebuildAll = true; // the first refresh, an overflowed world-change list or dropped dirty marks: evaluate everything
	};

}
