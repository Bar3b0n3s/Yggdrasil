#include "TestsPCH.h"

#include "Engine/Scene/RenderStaticCache.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Renderer/RenderStaticSet.h"
#include "Engine/Scene/Components/MaterialParametersComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PlanarReflectionComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/SkinnedMeshComponent.h"
#include "Engine/Scene/Components/StaticComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/DeathTest.h"
#include "Support/SceneTestFixture.h"

#include <entt/entity/entity.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3, W43.2): the scene's render static cache, through the scene's
// Scene::RefreshStaticSet as render extraction calls it.

namespace Engine {

	namespace {

		Entity CreateStaticRoot(Scene& scene, std::string_view name, StaticScope scope = StaticScope::Subtree)
		{
			const Entity root = scene.CreateEntity(name);
			root.AddComponent<StaticComponent>(StaticComponent{ .Scope = scope });
			return root;
		}

		void SetTranslation(Entity entity, const glm::vec3& translation)
		{
			entity.Patch<TransformComponent>([translation](TransformComponent& transform)
			{
				transform.Translation = translation;
			});
		}

		// A cube under `parent` at the local `position`.
		Entity CreateMesh(Scene& scene, std::string_view name, Entity parent, const glm::vec3& position)
		{
			const Entity entity = scene.CreateEntity(name, parent);
			entity.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
			SetTranslation(entity, position);
			return entity;
		}

		// The set's cell at `key`, or nullptr.
		const RenderStaticCell* FindCell(const RenderStaticSet& set, glm::ivec2 key)
		{
			for (const Ref<const RenderStaticCell>& cell : set.Cells)
			{
				if (cell->Key == key)
					return cell.get();
			}
			return nullptr;
		}

		std::vector<glm::ivec2> CellKeys(const RenderStaticSet& set)
		{
			std::vector<glm::ivec2> keys;
			for (const Ref<const RenderStaticCell>& cell : set.Cells)
				keys.push_back(cell->Key);
			return keys;
		}

		// The entities of the cell at `key` (its pick table), empty without the cell.
		std::vector<UUID> CellEntities(const RenderStaticSet& set, glm::ivec2 key)
		{
			const RenderStaticCell* cell = FindCell(set, key);
			return cell != nullptr ? cell->PickTable : std::vector<UUID>{};
		}

		// Every entity of the set, cells in order.
		std::vector<UUID> SetEntities(const RenderStaticSet& set)
		{
			std::vector<UUID> entities;
			for (const Ref<const RenderStaticCell>& cell : set.Cells)
				entities.insert(entities.end(), cell->PickTable.begin(), cell->PickTable.end());
			return entities;
		}

		// The set after a transform update, as a play session's frame extracts it.
		Ref<const RenderStaticSet> UpdateAndRefresh(Scene& scene)
		{
			TransformSystem::Update(scene);
			return scene.RefreshStaticSet();
		}

	}

#if defined(ENGINE_DEBUG)
	// M43: a static entity's MeshRenderer written past its EnTT signals after a refresh fails the next refresh's full-rebuild
	// check.
	ENGINE_DEATH_TEST("Scene/StaticItemWrittenPastSignals")
	{
		Test::SceneTestFixture fixture;
		Scene& scene = fixture.GetScene();
		const Entity rock = CreateMesh(scene, "Rock", CreateStaticRoot(scene, "Island"), glm::vec3(1.0f, 0.0f, 1.0f));
		static_cast<void>(UpdateAndRefresh(scene));
		// Deliberately past the signals: a mutable registry write fires no on_update.
		scene.GetRegistry().get<MeshRendererComponent>(rock.GetHandle()).CastShadows = false;
		static_cast<void>(scene.RefreshStaticSet());
	}
#endif

	TEST_SUITE("Scene")
	{
		TEST_CASE("RenderStaticCache: the scene creates its cache at its first Static")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity rock = CreateMesh(scene, "Rock", Entity{}, glm::vec3(0.0f));
			TransformSystem::Update(scene);
			CHECK(scene.GetStaticCache() == nullptr);
			CHECK(scene.RefreshStaticSet() == nullptr);
			CHECK(scene.GetStaticCellRebuilds() == 0);

			rock.AddComponent<StaticComponent>();
			const RenderStaticCache* cache = scene.GetStaticCache();
			REQUIRE(cache != nullptr);
			const Ref<const RenderStaticSet> set = scene.RefreshStaticSet();
			REQUIRE(set != nullptr);
			REQUIRE(set->Cells.size() == 1);
			const RenderStaticCell& cell = *set->Cells[0];
			CHECK(cell.Key == glm::ivec2(0));
			REQUIRE(cell.Items.size() == 1);
			const MeshDrawItem& item = cell.Items[0];
			CHECK(item.Entity == rock.GetUUID());
			CHECK(item.Mesh == BuiltinAssetHandles::CubeMesh);
			CHECK(item.PickId == 0);
			CHECK_FALSE(item.PreviousWorld.has_value());
			CHECK(cell.PickTable == std::vector<UUID>{ rock.GetUUID() });
			CHECK(cache->HoldsItem(rock.GetHandle()));
			CHECK(cache->GetItemCount() == 1);

			// Removing the last Static keeps the cache, with an empty set.
			rock.RemoveComponent<StaticComponent>();
			CHECK(scene.GetStaticCache() == cache);
			const Ref<const RenderStaticSet> empty = scene.RefreshStaticSet();
			REQUIRE(empty != nullptr);
			CHECK(empty->Cells.empty());
			CHECK(empty->Version > set->Version);
			CHECK_FALSE(cache->HoldsItem(rock.GetHandle()));
		}

		TEST_CASE("RenderStaticCache: a write to a static entity rebuilds only its cell")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity island = CreateStaticRoot(scene, "Island");
			// Four cells of StaticCellSize (32 m), keyed by (floor(x / 32), floor(z / 32)).
			const Entity a = CreateMesh(scene, "A", island, glm::vec3(1.0f, 0.0f, 1.0f));
			const Entity b = CreateMesh(scene, "B", island, glm::vec3(2.0f, 0.0f, 3.0f));
			const Entity c = CreateMesh(scene, "C", island, glm::vec3(33.0f, 0.0f, 1.0f));
			const Entity d = CreateMesh(scene, "D", island, glm::vec3(40.0f, 5.0f, 2.0f));
			const Entity e = CreateMesh(scene, "E", island, glm::vec3(1.0f, 0.0f, 33.0f));
			const Entity f = CreateMesh(scene, "F", island, glm::vec3(-1.0f, 0.0f, 40.0f));
			const glm::ivec2 west(-1, 1);
			const glm::ivec2 origin(0, 0);
			const glm::ivec2 north(0, 1);
			const glm::ivec2 east(1, 0);

			const Ref<const RenderStaticSet> first = UpdateAndRefresh(scene);
			const RenderStaticCache* cache = scene.GetStaticCache();
			REQUIRE(cache != nullptr);
			REQUIRE(first != nullptr);
			// Cells in key order, x then z, signed; items in canonical order.
			CHECK(CellKeys(*first) == std::vector<glm::ivec2>{ west, origin, north, east });
			CHECK(CellEntities(*first, origin) == std::vector<UUID>{ a.GetUUID(), b.GetUUID() });
			CHECK(CellEntities(*first, east) == std::vector<UUID>{ c.GetUUID(), d.GetUUID() });
			CHECK(CellEntities(*first, north) == std::vector<UUID>{ e.GetUUID() });
			CHECK(CellEntities(*first, west) == std::vector<UUID>{ f.GetUUID() });
			CHECK(cache->GetLastCellRebuilds() == 4);
			CHECK(cache->GetItemCount() == 6);

			// Nothing changed: the same set.
			CHECK(UpdateAndRefresh(scene) == first);
			CHECK(cache->GetLastCellRebuilds() == 0);

			// A write inside its cell rebuilds that cell alone; the others keep their Ref and Version.
			SetTranslation(b, glm::vec3(5.0f, 0.0f, 3.0f));
			const Ref<const RenderStaticSet> second = UpdateAndRefresh(scene);
			CHECK(cache->GetLastCellRebuilds() == 1);
			REQUIRE(second != first);
			CHECK(second->Version > first->Version);
			REQUIRE(second->Cells.size() == first->Cells.size());
			for (size_t index = 0; index < second->Cells.size(); ++index)
			{
				CAPTURE(index);
				if (second->Cells[index]->Key == origin)
				{
					CHECK(second->Cells[index] != first->Cells[index]);
					CHECK(second->Cells[index]->Version > first->Cells[index]->Version);
				}
				else
				{
					CHECK(second->Cells[index] == first->Cells[index]);
					CHECK(second->Cells[index]->Version == first->Cells[index]->Version);
				}
			}
			REQUIRE(FindCell(*second, origin)->Items.size() == 2);
			CHECK(FindCell(*second, origin)->Items[1].World[3].x == 5.0f);
			// The cell a snapshot already holds is never modified (copy on write).
			CHECK(FindCell(*first, origin)->Items[1].World[3].x == 2.0f);

			// A move across a cell boundary rebuilds the two cells; B joins C and D in canonical order.
			SetTranslation(b, glm::vec3(35.0f, 0.0f, 3.0f));
			const Ref<const RenderStaticSet> third = UpdateAndRefresh(scene);
			CHECK(cache->GetLastCellRebuilds() == 2);
			CHECK(CellEntities(*third, origin) == std::vector<UUID>{ a.GetUUID() });
			CHECK(CellEntities(*third, east) == std::vector<UUID>{ b.GetUUID(), c.GetUUID(), d.GetUUID() });
			CHECK(FindCell(*third, north) == FindCell(*second, north));
			CHECK(FindCell(*third, west) == FindCell(*second, west));

			// A write of the same bits rebuilds nothing.
			SetTranslation(b, glm::vec3(35.0f, 0.0f, 3.0f));
			CHECK(UpdateAndRefresh(scene) == third);
			CHECK(cache->GetLastCellRebuilds() == 0);

			// A renderer write rebuilds its cell alone.
			c.Patch<MeshRendererComponent>([](MeshRendererComponent& renderer)
			{
				renderer.CastShadows = false;
			});
			const Ref<const RenderStaticSet> fourth = UpdateAndRefresh(scene);
			CHECK(cache->GetLastCellRebuilds() == 1);
			REQUIRE(FindCell(*fourth, east)->Items.size() == 3);
			CHECK_FALSE(FindCell(*fourth, east)->Items[1].CastShadows);
			CHECK(FindCell(*fourth, origin) == FindCell(*third, origin));

			// The cumulative counter, the scene's too.
			CHECK(cache->GetStaticCellRebuilds() == 4 + 1 + 2 + 1);
			CHECK(scene.GetStaticCellRebuilds() == cache->GetStaticCellRebuilds());
		}

		TEST_CASE("RenderStaticCache: an item leaves the set when its entity stops qualifying and returns when it qualifies again")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity island = CreateStaticRoot(scene, "Island");
			const Entity rock = CreateMesh(scene, "Rock", island, glm::vec3(1.0f, 0.0f, 1.0f));
			const Entity tree = CreateMesh(scene, "Tree", island, glm::vec3(2.0f, 0.0f, 2.0f));
			const std::vector<UUID> both{ rock.GetUUID(), tree.GetUUID() };
			const std::vector<UUID> treeOnly{ tree.GetUUID() };
			CHECK(SetEntities(*UpdateAndRefresh(scene)) == both);
			const RenderStaticCache* cache = scene.GetStaticCache();
			REQUIRE(cache != nullptr);

			// A hidden renderer.
			rock.Patch<MeshRendererComponent>([](MeshRendererComponent& renderer)
			{
				renderer.Visible = false;
			});
			CHECK(SetEntities(*UpdateAndRefresh(scene)) == treeOnly);
			CHECK_FALSE(cache->HoldsItem(rock.GetHandle()));
			rock.Patch<MeshRendererComponent>([](MeshRendererComponent& renderer)
			{
				renderer.Visible = true;
			});
			CHECK(SetEntities(*UpdateAndRefresh(scene)) == both);

			// A disabled entity, then a disabled root.
			rock.SetActive(false);
			CHECK(SetEntities(*UpdateAndRefresh(scene)) == treeOnly);
			rock.SetActive(true);
			CHECK(SetEntities(*UpdateAndRefresh(scene)) == both);
			island.SetActive(false);
			CHECK(UpdateAndRefresh(scene)->Cells.empty());
			island.SetActive(true);
			CHECK(SetEntities(*UpdateAndRefresh(scene)) == both);

			// A scrolling MaterialParameters stays dynamic; without the scroll its resolved parameters ride in the item.
			rock.AddComponent<MaterialParametersComponent>(MaterialParametersComponent{ .Tint = glm::vec3(0.5f), .UVScroll = glm::vec2(0.0f, 0.25f) });
			CHECK(SetEntities(*UpdateAndRefresh(scene)) == treeOnly);
			rock.Patch<MaterialParametersComponent>([](MaterialParametersComponent& parameters)
			{
				parameters.UVScroll = glm::vec2(0.0f);
			});
			const Ref<const RenderStaticSet> tinted = UpdateAndRefresh(scene);
			REQUIRE(SetEntities(*tinted) == both);
			CHECK(tinted->Cells[0]->Items[0].Parameters.Tint == glm::vec3(0.5f));
			rock.RemoveComponent<MaterialParametersComponent>();
			CHECK(UpdateAndRefresh(scene)->Cells[0]->Items[0].Parameters.IsNeutral());

			// A skinned item and mirrored glass stay dynamic.
			rock.AddComponent<SkinnedMeshComponent>();
			CHECK(SetEntities(*UpdateAndRefresh(scene)) == treeOnly);
			rock.RemoveComponent<SkinnedMeshComponent>();
			CHECK(SetEntities(*UpdateAndRefresh(scene)) == both);
			rock.AddComponent<PlanarReflectionComponent>();
			CHECK(SetEntities(*UpdateAndRefresh(scene)) == treeOnly);
			rock.RemoveComponent<PlanarReflectionComponent>();
			CHECK(SetEntities(*UpdateAndRefresh(scene)) == both);

			// NotStatic on the entity, then the root's Static removed and added again.
			rock.AddComponent<StaticComponent>(StaticComponent{ .Scope = StaticScope::NotStatic });
			CHECK(SetEntities(*UpdateAndRefresh(scene)) == treeOnly);
			rock.RemoveComponent<StaticComponent>();
			CHECK(SetEntities(*UpdateAndRefresh(scene)) == both);
			island.RemoveComponent<StaticComponent>();
			CHECK(UpdateAndRefresh(scene)->Cells.empty());
			island.AddComponent<StaticComponent>();
			CHECK(SetEntities(*UpdateAndRefresh(scene)) == both);

			// A removed renderer, and a renderer without a mesh.
			rock.RemoveComponent<MeshRendererComponent>();
			CHECK(SetEntities(*UpdateAndRefresh(scene)) == treeOnly);
			static_cast<void>(rock.AddComponent<MeshRendererComponent>());
			CHECK(SetEntities(*UpdateAndRefresh(scene)) == treeOnly);
			rock.Patch<MeshRendererComponent>([](MeshRendererComponent& renderer)
			{
				renderer.Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::SphereMesh);
			});
			const Ref<const RenderStaticSet> sphere = UpdateAndRefresh(scene);
			REQUIRE(SetEntities(*sphere) == both);
			CHECK(sphere->Cells[0]->Items[0].Mesh == BuiltinAssetHandles::SphereMesh);
			CHECK(cache->GetItemCount() == 2);
		}

		TEST_CASE("RenderStaticCache: a destroyed static entity leaves the set, in a runtime scene before the flush")
		{
			for (const bool runtime : { false, true })
			{
				CAPTURE(runtime);
				Test::SceneTestFixture fixture(1, runtime);
				Scene& scene = fixture.GetScene();
				const Entity island = CreateStaticRoot(scene, "Island");
				const Entity rock = CreateMesh(scene, "Rock", island, glm::vec3(1.0f, 0.0f, 1.0f));
				const Entity tree = CreateMesh(scene, "Tree", island, glm::vec3(2.0f, 0.0f, 2.0f));
				const entt::entity rockHandle = rock.GetHandle();
				CHECK(SetEntities(*UpdateAndRefresh(scene)) == std::vector<UUID>{ rock.GetUUID(), tree.GetUUID() });

				scene.DestroyEntity(rock);
				CHECK(SetEntities(*UpdateAndRefresh(scene)) == std::vector<UUID>{ tree.GetUUID() });
				CHECK_FALSE(scene.GetStaticCache()->HoldsItem(rockHandle));
				scene.FlushPendingDestroys();
				CHECK(SetEntities(*UpdateAndRefresh(scene)) == std::vector<UUID>{ tree.GetUUID() });

				// An entity that may reuse the destroyed one's index takes its own item.
				const Entity stone = CreateMesh(scene, "Stone", island, glm::vec3(3.0f, 0.0f, 3.0f));
				const Ref<const RenderStaticSet> set = UpdateAndRefresh(scene);
				CHECK(SetEntities(*set) == std::vector<UUID>{ tree.GetUUID(), stone.GetUUID() });
				CHECK(set->Cells[0]->Items[1].World[3].x == 3.0f);

				// The whole subtree, its root first.
				scene.DestroyEntity(island);
				CHECK(UpdateAndRefresh(scene)->Cells.empty());
				CHECK(scene.GetStaticCache()->GetItemCount() == 0);
			}
		}

		TEST_CASE("RenderStaticCache: a sibling move or a root move reorders a static cell")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity island = CreateStaticRoot(scene, "Island");
			const Entity first = CreateMesh(scene, "First", island, glm::vec3(1.0f, 0.0f, 1.0f));
			const Entity second = CreateMesh(scene, "Second", island, glm::vec3(2.0f, 0.0f, 2.0f));
			const Entity reef = CreateStaticRoot(scene, "Reef");
			const Entity coral = CreateMesh(scene, "Coral", reef, glm::vec3(3.0f, 0.0f, 3.0f));
			static_cast<void>(CreateMesh(scene, "Far", reef, glm::vec3(100.0f, 0.0f, 3.0f)));
			const glm::ivec2 origin(0, 0);
			const glm::ivec2 distant(3, 0);
			const Ref<const RenderStaticSet> initial = UpdateAndRefresh(scene);
			const RenderStaticCache* cache = scene.GetStaticCache();
			REQUIRE(cache != nullptr);
			CHECK(CellEntities(*initial, origin) == std::vector<UUID>{ first.GetUUID(), second.GetUUID(), coral.GetUUID() });
			REQUIRE(FindCell(*initial, distant) != nullptr);

			// A sibling move changes no matrix, but the order: the cell is rebuilt, the distant one kept.
			REQUIRE(scene.SetParent(second, island, 0u).has_value());
			const Ref<const RenderStaticSet> swapped = UpdateAndRefresh(scene);
			CHECK(cache->GetLastCellRebuilds() == 1);
			CHECK(CellEntities(*swapped, origin) == std::vector<UUID>{ second.GetUUID(), first.GetUUID(), coral.GetUUID() });
			CHECK(FindCell(*swapped, distant) == FindCell(*initial, distant));

			// A root move among the roots.
			REQUIRE(scene.SetParent(reef, Entity{}, 0u).has_value());
			CHECK(CellEntities(*UpdateAndRefresh(scene), origin) == std::vector<UUID>{ coral.GetUUID(), second.GetUUID(), first.GetUUID() });
			CHECK(cache->GetLastCellRebuilds() == 1);

			// A structural change that keeps the order (a new entity elsewhere) rebuilds nothing.
			static_cast<void>(scene.CreateEntity("Bystander"));
			static_cast<void>(UpdateAndRefresh(scene));
			CHECK(cache->GetLastCellRebuilds() == 0);
		}

		TEST_CASE("RenderStaticCache: an overflowed world-change list re-evaluates every static entity and rebuilds only the cells that changed")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity island = CreateStaticRoot(scene, "Island");
			// Nine rocks in three cells, 40 m apart along x, and a ledge in a fourth cell that no update writes.
			std::vector<Entity> rocks;
			const auto rockX = [](size_t index)
			{
				return static_cast<float>(index % 3) * 40.0f;
			};
			for (size_t index = 0; index < 9; ++index)
				rocks.push_back(CreateMesh(scene, "Rock", island, glm::vec3(rockX(index), 0.0f, 1.0f)));
			const Entity ledge = CreateMesh(scene, "Ledge", island, glm::vec3(130.0f, 0.0f, 1.0f));
			const Ref<const RenderStaticSet> before = UpdateAndRefresh(scene);
			REQUIRE(before->Cells.size() == 4);
			const RenderStaticCache* cache = scene.GetStaticCache();

			// 10 written entities an update for 420 updates: more than 4,096 entries since the last take. The rocks end where
			// they were, but the first ends 4 m further along z, in its cell.
			for (int update = 0; update < 420; ++update)
			{
				for (size_t index = 0; index < rocks.size(); ++index)
					SetTranslation(rocks[index], glm::vec3(rockX(index), 0.0f, update % 2 == 0 ? 2.0f : 1.0f));
				SetTranslation(island, glm::vec3(0.0f));
				TransformSystem::Update(scene);
			}
			SetTranslation(rocks[0], glm::vec3(rockX(0), 0.0f, 5.0f));
			TransformSystem::Update(scene);
			// Deliberately past the signals: only a pass over every static entity sees the ledge's write, which shows that the
			// overflow's full pass ran.
			scene.GetRegistry().get<MeshRendererComponent>(ledge.GetHandle()).CastShadows = false;
			const Ref<const RenderStaticSet> after = scene.RefreshStaticSet();
			CHECK(cache->GetLastCellRebuilds() == 2);
			REQUIRE(after->Cells.size() == 4);
			CHECK(after->Cells[0] != before->Cells[0]);
			CHECK(after->Cells[0]->Items[0].World[3].z == 5.0f);
			CHECK(after->Cells[1] == before->Cells[1]);
			CHECK(after->Cells[2] == before->Cells[2]);
			CHECK(after->Cells[3] != before->Cells[3]);
			CHECK_FALSE(after->Cells[3]->Items[0].CastShadows);

			// Then incremental again: nothing, then one cell.
			CHECK(UpdateAndRefresh(scene) == after);
			SetTranslation(rocks[1], glm::vec3(rockX(1), 0.0f, 3.0f));
			const Ref<const RenderStaticSet> moved = UpdateAndRefresh(scene);
			CHECK(cache->GetLastCellRebuilds() == 1);
			CHECK(moved->Cells[0] == after->Cells[0]);
			CHECK(moved->Cells[1] != after->Cells[1]);
		}

		TEST_CASE("RenderStaticCache: dirty marks beyond the cap make the next refresh a full pass, and later ones incremental")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity island = CreateStaticRoot(scene, "Island");
			const Entity rock = CreateMesh(scene, "Rock", island, glm::vec3(1.0f, 0.0f, 1.0f));
			const Entity tree = CreateMesh(scene, "Tree", island, glm::vec3(40.0f, 0.0f, 1.0f));
			const Entity bush = CreateMesh(scene, "Bush", island, glm::vec3(80.0f, 0.0f, 1.0f));
			const Ref<const RenderStaticSet> before = UpdateAndRefresh(scene);
			const RenderStaticCache* cache = scene.GetStaticCache();
			REQUIRE(before->Cells.size() == 3);

			// More than 4,096 marks between two refreshes (four an entity of four entities, at least 4,096): they are dropped.
			for (int patch = 0; patch < 4200; ++patch)
			{
				rock.Patch<MeshRendererComponent>([patch](MeshRendererComponent& renderer)
				{
					renderer.CastShadows = patch % 2 == 0;
				});
			}
			// Deliberately past the signals, as above: only the full pass sees the tree's write.
			scene.GetRegistry().get<MeshRendererComponent>(tree.GetHandle()).ReceiveShadows = false;
			const Ref<const RenderStaticSet> after = scene.RefreshStaticSet();
			CHECK(cache->GetLastCellRebuilds() == 2);
			CHECK_FALSE(after->Cells[0]->Items[0].CastShadows);
			CHECK_FALSE(after->Cells[1]->Items[0].ReceiveShadows);
			CHECK(after->Cells[2] == before->Cells[2]);

			// Incremental again: one patch, one cell.
			bush.Patch<MeshRendererComponent>([](MeshRendererComponent& renderer)
			{
				renderer.CastShadows = false;
			});
			const Ref<const RenderStaticSet> patched = scene.RefreshStaticSet();
			CHECK(cache->GetLastCellRebuilds() == 1);
			CHECK(patched->Cells[0] == after->Cells[0]);
			CHECK_FALSE(patched->Cells[2]->Items[0].CastShadows);
		}

		TEST_CASE("RenderStaticCache: a play session's serializer copy holds the same static set")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity island = CreateStaticRoot(scene, "Island");
			const Entity rock = CreateMesh(scene, "Rock", island, glm::vec3(1.0f, 0.0f, 1.0f));
			const Entity mill = CreateMesh(scene, "Mill", island, glm::vec3(3.0f, 0.0f, 3.0f));
			mill.AddComponent<StaticComponent>(StaticComponent{ .Scope = StaticScope::NotStatic });
			static_cast<void>(CreateMesh(scene, "Sail", mill, glm::vec3(0.0f, 2.0f, 0.0f)));
			const Entity reef = CreateMesh(scene, "Reef", island, glm::vec3(40.0f, 0.0f, 1.0f));
			const Entity hut = CreateMesh(scene, "Hut", Entity{}, glm::vec3(2.0f, 0.0f, 2.0f));
			hut.AddComponent<StaticComponent>(StaticComponent{ .Scope = StaticScope::EntityOnly });
			static_cast<void>(CreateMesh(scene, "Door", hut, glm::vec3(0.5f, 0.0f, 0.0f)));
			const Ref<const RenderStaticSet> original = UpdateAndRefresh(scene);
			REQUIRE(CellKeys(*original) == std::vector<glm::ivec2>{ glm::ivec2(0, 0), glm::ivec2(1, 0) });
			CHECK(CellEntities(*original, glm::ivec2(0, 0)) == std::vector<UUID>{ rock.GetUUID(), hut.GetUUID() });
			CHECK(CellEntities(*original, glm::ivec2(1, 0)) == std::vector<UUID>{ reef.GetUUID() });

			// The runtime copy a play session makes of its edit scene.
			const Result<Json> document = SceneSerializer::ToJson(scene);
			REQUIRE(document.has_value());
			const Scope<Scene> copy = fixture.CreateEmptyScene(true);
			LoadReport report;
			REQUIRE(SceneSerializer::FromJson(*copy, *document, LoadOptions{}, report).has_value());
			REQUIRE(copy->GetStaticCache() != nullptr);
			const Ref<const RenderStaticSet> copied = UpdateAndRefresh(*copy);
			REQUIRE(CellKeys(*copied) == CellKeys(*original));
			for (const glm::ivec2& key : CellKeys(*original))
			{
				CAPTURE(key.x);
				CHECK(CellEntities(*copied, key) == CellEntities(*original, key));
				const RenderStaticCell& copiedCell = *FindCell(*copied, key);
				const RenderStaticCell& originalCell = *FindCell(*original, key);
				REQUIRE(copiedCell.Items.size() == originalCell.Items.size());
				for (size_t index = 0; index < originalCell.Items.size(); ++index)
					CHECK(copiedCell.Items[index].World == originalCell.Items[index].World);
			}
		}

		TEST_CASE("RenderStaticCache: a cleared scene's set is empty, and fills again with the entities made after")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity island = CreateStaticRoot(scene, "Island");
			static_cast<void>(CreateMesh(scene, "Rock", island, glm::vec3(1.0f, 0.0f, 1.0f)));
			static_cast<void>(CreateMesh(scene, "Tree", island, glm::vec3(40.0f, 0.0f, 1.0f)));
			REQUIRE(UpdateAndRefresh(scene)->Cells.size() == 2);
			const RenderStaticCache* cache = scene.GetStaticCache();

			scene.Clear();
			CHECK(scene.GetStaticCache() == cache);
			CHECK(UpdateAndRefresh(scene)->Cells.empty());
			CHECK(cache->GetItemCount() == 0);

			const Entity reef = CreateStaticRoot(scene, "Reef");
			const Entity coral = CreateMesh(scene, "Coral", reef, glm::vec3(1.0f, 0.0f, 1.0f));
			const Ref<const RenderStaticSet> refilled = UpdateAndRefresh(scene);
			CHECK(SetEntities(*refilled) == std::vector<UUID>{ coral.GetUUID() });
			CHECK(scene.GetStaticCache() == cache);
		}

		TEST_CASE("RenderStaticCache: an index reused before the next refresh holds the new entity's item")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity island = CreateStaticRoot(scene, "Island");
			const Entity rock = CreateMesh(scene, "Rock", island, glm::vec3(1.0f, 0.0f, 1.0f));
			const Entity tree = CreateMesh(scene, "Tree", island, glm::vec3(2.0f, 0.0f, 2.0f));
			static_cast<void>(UpdateAndRefresh(scene));
			const RenderStaticCache* cache = scene.GetStaticCache();
			const entt::entity rockHandle = rock.GetHandle();

			// The edit scene destroys at once, and the next entity takes the freed index.
			scene.DestroyEntity(rock);
			const Entity stone = CreateMesh(scene, "Stone", island, glm::vec3(3.0f, 0.0f, 3.0f));
			REQUIRE(entt::to_entity(stone.GetHandle()) == entt::to_entity(rockHandle));
			const Ref<const RenderStaticSet> reused = UpdateAndRefresh(scene);
			CHECK(SetEntities(*reused) == std::vector<UUID>{ tree.GetUUID(), stone.GetUUID() });
			CHECK(reused->Cells[0]->Items[1].World[3].x == 3.0f);
			CHECK_FALSE(cache->HoldsItem(rockHandle));
			CHECK(cache->HoldsItem(stone.GetHandle()));
			CHECK(cache->GetItemCount() == 2);

			// A reused index whose version wrapped below the held entity's sorts first among the dirty entities, so the new
			// entity claims the slot while the old item is still in it (ClaimSlot's release).
			const entt::entity stoneHandle = stone.GetHandle();
			scene.DestroyEntity(stone);
			Entity wrapped;
			for (int attempt = 0; attempt < (1 << 16) && !wrapped.IsValid(); ++attempt)
			{
				const Entity candidate = scene.CreateEntity("Recycled");
				REQUIRE(entt::to_entity(candidate.GetHandle()) == entt::to_entity(stoneHandle));
				if (entt::to_integral(candidate.GetHandle()) < entt::to_integral(stoneHandle))
					wrapped = candidate;
				else
					scene.DestroyEntity(candidate);
			}
			REQUIRE(wrapped.IsValid());
			REQUIRE(scene.SetParent(wrapped, island).has_value());
			wrapped.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
			const Ref<const RenderStaticSet> claimed = UpdateAndRefresh(scene);
			CHECK(SetEntities(*claimed) == std::vector<UUID>{ tree.GetUUID(), wrapped.GetUUID() });
			CHECK_FALSE(cache->HoldsItem(stoneHandle));
			CHECK(cache->HoldsItem(wrapped.GetHandle()));
			CHECK(cache->GetItemCount() == 2);
		}

		TEST_CASE("RenderStaticCache: a cell knows whether any of its items dissolves")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity island = CreateStaticRoot(scene, "Island");
			const Entity rock = CreateMesh(scene, "Rock", island, glm::vec3(1.0f, 0.0f, 1.0f));
			static_cast<void>(CreateMesh(scene, "Tree", island, glm::vec3(40.0f, 0.0f, 1.0f)));
			rock.AddComponent<MaterialParametersComponent>(MaterialParametersComponent{ .DissolveAmount = 0.5f });
			const Ref<const RenderStaticSet> dissolving = UpdateAndRefresh(scene);
			REQUIRE(dissolving->Cells.size() == 2);
			CHECK(dissolving->Cells[0]->Dissolving);
			CHECK_FALSE(dissolving->Cells[1]->Dissolving);
			rock.Patch<MaterialParametersComponent>([](MaterialParametersComponent& parameters)
			{
				parameters.DissolveAmount = 0.0f;
			});
			CHECK_FALSE(UpdateAndRefresh(scene)->Cells[0]->Dissolving);
		}

		TEST_CASE("RenderStaticCache: before its first transform update a static entity draws at its chain's matrix")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity island = CreateStaticRoot(scene, "Island");
			SetTranslation(island, glm::vec3(1.0f, 0.0f, 0.0f));
			const Entity rock = CreateMesh(scene, "Rock", island, glm::vec3(2.0f, 0.0f, 0.0f));
			const Ref<const RenderStaticSet> first = scene.RefreshStaticSet();
			REQUIRE(SetEntities(*first) == std::vector<UUID>{ rock.GetUUID() });
			CHECK(first->Cells[0]->Items[0].World[3].x == 3.0f);

			// Its parent written before any update: the item follows the chain.
			SetTranslation(island, glm::vec3(40.0f, 0.0f, 0.0f));
			const Ref<const RenderStaticSet> moved = scene.RefreshStaticSet();
			REQUIRE(moved->Cells.size() == 1);
			CHECK(moved->Cells[0]->Key == glm::ivec2(1, 0));
			CHECK(moved->Cells[0]->Items[0].World[3].x == 42.0f);

			// The first update computes the same matrix: nothing to rebuild.
			CHECK(UpdateAndRefresh(scene) == moved);
			CHECK(scene.GetStaticCache()->GetLastCellRebuilds() == 0);
		}

		TEST_CASE("RenderStaticCache: a static item written past its signals fails the Debug full-rebuild check")
		{
#if defined(ENGINE_DEBUG)
			ENGINE_CHECK_DEATH("Scene/StaticItemWrittenPastSignals", "Stale static set of scene 'Test': item 0 of cell (0, 0), entity '/Island/Rock'");
#else
			// Without the check (Release, Dist) the cache does not see the write: the item stays as it was until a write that
			// fires the signals, which is why Debug runs check every refresh.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity rock = CreateMesh(scene, "Rock", CreateStaticRoot(scene, "Island"), glm::vec3(1.0f, 0.0f, 1.0f));
			static_cast<void>(UpdateAndRefresh(scene));
			scene.GetRegistry().get<MeshRendererComponent>(rock.GetHandle()).CastShadows = false;
			CHECK(scene.RefreshStaticSet()->Cells[0]->Items[0].CastShadows);
			rock.Patch<MeshRendererComponent>([](MeshRendererComponent& renderer)
			{
				renderer.ReceiveShadows = false;
			});
			CHECK_FALSE(scene.RefreshStaticSet()->Cells[0]->Items[0].CastShadows);
#endif
		}
	}

}
