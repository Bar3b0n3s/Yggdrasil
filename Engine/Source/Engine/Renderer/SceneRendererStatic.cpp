#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/MeshLod.h"
#include "Engine/Renderer/MeshWind.h"
#include "Engine/Renderer/PlanarReflections.h"
#include "Engine/Renderer/Private/ForwardPipelines.h"
#include "Engine/Renderer/Private/SceneRendererState.h"
#include "Engine/Renderer/RenderPrepare.h"
#include "Engine/Renderer/RenderStaticDraws.h"
#include "Engine/Renderer/RenderStaticSet.h"
#include "Engine/Renderer/ShadowPass.h"
#include "Shared/DrawConstants.h"
#include "Shared/InstanceRecord.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <utility>
#include <vector>

// The renderer's static path (M43, Docs/Decisions/0061-m43-static-scenery.md decision 3; Architecture §8.3). A snapshot's
// static set (Renderer/RenderStaticSet.h) reaches the renderer as cells it already saw, which keep what it prepared from them,
// and cells it did not, which it prepares once: the Prepare stage acquires the set's GPU-resident records
// (GpuResourceCache::AcquireStaticRecords), prepares each new cell and each cell a changed mirror invalidates (its draws'
// keys, bounds and casters, PrepareStaticCell) and releases the cells the set no longer holds (PrepareStaticSet). Every mesh
// view then culls the cells by their boxes and the items of the cells it cuts (AppendStaticViewDraws, as PrepareViewDraws'
// part), gathers the visible static records by key and draws each key once, its instances' records read through the view's
// InstanceIndices; each shadow face does the same with the casters (PrepareStaticShadows, ShadowPass::PrepareStaticFaces).
// The tests are the dynamic path's own, so a scene renders the same pixels with its scenery static and without; only the
// order of exactly coplanar instances of one key can differ, since static instances follow cell order, then item order.
// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3E and its W46.4 record): a prepared cell holds
// each draw's keys at every level of its mesh (unfaded and fading), so a view chooses each visible item's levels as
// PrepareViewDraws does (SelectViewMeshLod) and writes its signed fade into the item's InstanceIndices entries
// (InstanceIndex::Reserved); a cell beyond every item's CullDistance is culled by distance before its items; a key carries
// its material's wind, which its draws push (DrawConstantsWind) in a render with a Wind, so the static records hold nothing
// from a material; and the casters choose their levels per face (ShadowPass::PrepareStaticFaces).

namespace Engine {

	namespace Utils {

		// The unused keys a renderer's key tables may hold beyond twice the keys in use before they start afresh.
		constexpr size_t StaticKeySlack = 256;

		// The InstanceIndices buffer's smallest capacity, in entries.
		constexpr uint64_t MinInstanceIndexCapacity = 64;

		// The world box of `bounds` (local) at `world`: ShadowPass.h's caster box as an Aabb.
		[[nodiscard]] static Aabb GetStaticWorldBox(const Aabb& bounds, const glm::mat4& world)
		{
			const ShadowBox box = ComputeShadowCasterBox(bounds, world);
			return Aabb{ .Min = box.Centre - box.HalfExtent, .Max = box.Centre + box.HalfExtent };
		}

		// M46 (W46.4): whether every drawable item of `cell` lies at or beyond its CullDistance from `position` (the view's level
		// camera's), so SelectViewMeshLod culls each by distance: the distance from `position` to the box of their level spheres'
		// centres, in double, clears the cell's largest CullDistance by a margin above the per-item test's single-precision
		// rounding (a ten-thousandth of it and a millimetre). A cell this does not settle is measured item by item.
		[[nodiscard]] static bool IsStaticCellBeyondCullDistance(const Detail::SceneRendererStaticCell& cell, const glm::vec3& position)
		{
			if (cell.DrawableItems == 0 || !std::isfinite(cell.MaxCullDistance) || cell.Centres.IsEmpty())
				return false;
			double squared = 0.0;
			for (int axis = 0; axis < 3; ++axis)
			{
				const double value = position[axis];
				const double below = static_cast<double>(cell.Centres.Min[axis]) - value;
				const double above = value - static_cast<double>(cell.Centres.Max[axis]);
				const double gap = std::max({ below, above, 0.0 });
				squared += gap * gap;
			}
			const double reach = static_cast<double>(cell.MaxCullDistance) * 1.0001 + 0.001;
			return squared >= reach * reach;
		}

		// Whether the mesh of mirror `mesh` deforms an item that sets no morph weight: it has morph targets with a default weight
		// that is not 0 (ResolveMorphWeights resolves an empty MorphWeights to the defaults, SceneRenderer.cpp's
		// ResolveItemDeform).
		[[nodiscard]] static bool MorphsByDefault(const GpuMesh& mesh)
		{
			return mesh.MorphTargetCount > 0 && std::ranges::any_of(mesh.DeformBounds.DefaultWeights, [](float weight)
			{
				return weight != 0.0f;
			});
		}

	}

	void SceneRenderer::State::PrepareStaticSet(Detail::SceneRendererFrame& frame)
	{
		frame.StaticCells.clear();
		frame.StaticInstances = nullptr;
		const RenderSnapshot& snapshot = *frame.Snapshot;
		const RenderStaticSet* set = snapshot.StaticSet.get();
		if (set == nullptr || set->Cells.empty())
		{
			// Nothing static: the cells and the keys go, with every GPU object they hold.
			ClearStaticKeys();
			return;
		}
		frame.StaticCellCount = static_cast<uint32_t>(set->Cells.size());
		for (const Ref<const RenderStaticCell>& cell : set->Cells)
			frame.StaticItems += static_cast<uint32_t>(cell->Items.size());

		// The key tables start afresh once they hold more than twice the keys in use and the slack (cells prepared again with
		// other keys leave theirs behind, unused), every cell prepared again with them.
		if (StaticKeys.size() + StaticCasterKeys.size() > 2 * StaticKeysInUse + Utils::StaticKeySlack)
			ClearStaticKeys();

		// The records first, before any mirror reference is held: their acquisition may trim (RetryAfterTrim).
		Result<std::vector<uint32_t>> firsts = Cache->AcquireStaticRecords(*set);
		if (!firsts.has_value())
		{
			frame.KeepFirstError(std::unexpected(
				std::move(firsts).error().WithContext(std::format("while writing the {} records of the static set's {} cells; its items are not drawn this render",
					frame.StaticItems, frame.StaticCellCount))));
			return;
		}
		frame.StaticInstances = Cache->GetStaticRecordBuffer();

		// The mirrors the cells depend on, each looked up once per render: a cell prepared from another version of one (an asset
		// reload, a placeholder replaced by its asset, a material rebuilt) is prepared again, since a reload rebuilds no cell.
		std::map<AssetHandle, std::pair<uint64_t, const nvrhi::IBuffer*>> meshes;
		std::map<AssetHandle, uint64_t> materials;
		const auto isCurrent = [this, &meshes, &materials](const Detail::SceneRendererStaticCell& cell)
		{
			for (const Detail::SceneRendererStaticAsset& asset : cell.Meshes)
			{
				auto found = meshes.find(asset.Handle);
				if (found == meshes.end())
				{
					const GpuMesh& mesh = Cache->GetMesh(asset.Handle);
					found = meshes.emplace(asset.Handle, std::pair(mesh.Version, mesh.VertexBuffer.Get())).first;
				}
				if (found->second.first != asset.Version || found->second.second != asset.Vertices.Get())
					return false;
			}
			for (const Detail::SceneRendererStaticAsset& asset : cell.MaterialGenerations)
			{
				auto found = materials.find(asset.Handle);
				if (found == materials.end())
					found = materials.emplace(asset.Handle, Cache->GetMaterial(asset.Handle).Generation).first;
				if (found->second != asset.Version)
					return false;
			}
			return true;
		};
		for (Detail::SceneRendererStaticCell& cell : StaticCells | std::views::values)
			cell.Used = false;
		frame.StaticCells.reserve(set->Cells.size());
		for (size_t index = 0; index < set->Cells.size(); ++index)
		{
			const Ref<const RenderStaticCell>& cell = set->Cells[index];
			auto found = StaticCells.find(cell.get());
			if (found == StaticCells.end())
				found = StaticCells.emplace(cell.get(), PrepareStaticCell(cell)).first;
			else if (!isCurrent(found->second))
			{
				// Prepared again before the old preparation's keys are released, so a key both use keeps its objects.
				Detail::SceneRendererStaticCell prepared = PrepareStaticCell(cell);
				ReleaseStaticCellKeys(found->second);
				found->second = std::move(prepared);
			}
			found->second.Used = true;
			frame.StaticCells.push_back({ .Prepared = &found->second, .FirstRecord = (*firsts)[index] });
		}
		for (auto entry = StaticCells.begin(); entry != StaticCells.end();)
		{
			if (entry->second.Used)
			{
				++entry;
				continue;
			}
			ReleaseStaticCellKeys(entry->second);
			entry = StaticCells.erase(entry);
		}
		if (StaticBuckets.size() < StaticKeys.size())
			StaticBuckets.resize(StaticKeys.size());
	}

	Detail::SceneRendererStaticCell SceneRenderer::State::PrepareStaticCell(const Ref<const RenderStaticCell>& cell)
	{
		Detail::SceneRendererStaticCell prepared;
		prepared.Cell = cell;
		std::set<AssetHandle> meshesSeen;
		std::set<AssetHandle> materialsSeen;
		std::vector<StaticCasterDraw> casterDraws;
		// A key interned holds the current mirrors' objects, which every cell of the key then shares, and counts one more use.
		const auto internKey = [this](const Detail::SceneRendererStaticKey& key)
		{
			const auto [found, inserted] = StaticKeyIndex.try_emplace(key.Key, static_cast<uint32_t>(StaticKeys.size()));
			if (inserted)
			{
				StaticKeys.push_back(key);
				StaticKeyUses.push_back(0);
			}
			else
				StaticKeys[found->second] = key;
			if (StaticKeyUses[found->second]++ == 0)
				++StaticKeysInUse;
			return found->second;
		};
		const auto internCasterKey = [this](const StaticShadowKey& key)
		{
			const auto [found, inserted] = StaticCasterKeyIndex.try_emplace(key.Key, static_cast<uint32_t>(StaticCasterKeys.size()));
			if (inserted)
			{
				StaticCasterKeys.push_back(key);
				StaticCasterKeyUses.push_back(0);
			}
			else
				StaticCasterKeys[found->second] = key;
			if (StaticCasterKeyUses[found->second]++ == 0)
				++StaticKeysInUse;
			return found->second;
		};
		Aabb box;
		Aabb casterBox;
		for (uint32_t itemIndex = 0; itemIndex < cell->Items.size(); ++itemIndex)
		{
			const MeshDrawItem& item = cell->Items[itemIndex];
			prepared.Entities.emplace_back(item.Entity, itemIndex);
			Detail::SceneRendererStaticItem entry{ .World = item.World, .FirstDraw = static_cast<uint32_t>(prepared.Draws.size()) };
			if (!Detail::IsFiniteMatrix(item.World))
			{
				entry.Finite = false;
				if (!prepared.NonFinite.has_value())
					prepared.NonFinite = item.Entity;
				prepared.Items.push_back(entry);
				continue;
			}
			if (!item.Mesh.IsValid())
			{
				prepared.Items.push_back(entry); // a null handle draws nothing, as a dynamic item's
				continue;
			}
			// The mirror is valid until the next upload (resolving materials uploads textures): what is needed is copied now.
			const GpuMesh& mirror = Cache->GetMesh(item.Mesh);
			if (meshesSeen.insert(item.Mesh).second)
				prepared.Meshes.push_back({ .Handle = item.Mesh, .Version = mirror.Version, .Vertices = mirror.VertexBuffer });
			if (mirror.VertexBuffer == nullptr || mirror.AttributeBuffer == nullptr || mirror.IndexBuffer == nullptr || mirror.WindBuffer == nullptr)
			{
				prepared.Items.push_back(entry); // not even the placeholder could be created (reported by the cache)
				continue;
			}
			const nvrhi::BufferHandle vertices = mirror.VertexBuffer;
			const nvrhi::BufferHandle attributes = mirror.AttributeBuffer;
			const nvrhi::BufferHandle indices = mirror.IndexBuffer;
			const nvrhi::BufferHandle wind = mirror.WindBuffer;
			const std::vector<GpuSubmesh> submeshes = mirror.Submeshes;
			const std::vector<AssetHandle> defaults = mirror.DefaultMaterials;
			// M46 (W46.4): what chooses the item's levels in a view, and its level sphere's centre for the cell's distance cull.
			entry.Drawable = true;
			entry.LodCount = static_cast<uint32_t>(std::min<size_t>(mirror.LodScreenSizes.size(), entry.LodScreenSizes.size()));
			std::copy_n(mirror.LodScreenSizes.begin(), entry.LodCount, entry.LodScreenSizes.begin());
			entry.MeshBounds = mirror.Bounds;
			entry.Distance = item.Distance;
			++prepared.DrawableItems;
			prepared.Centres.Extend(ComputeMeshLodSphere(entry.MeshBounds, item.World).Center);
			if (item.Distance.CullDistance > 0.0f)
			{
				++prepared.DistanceItems;
				prepared.MaxCullDistance = std::max(prepared.MaxCullDistance, item.Distance.CullDistance);
			}
			else
				prepared.MaxCullDistance = std::numeric_limits<float>::infinity();
			// Decision 1's limit (the W43.2 record): the static path does not deform, and whether an item morphs depends on its
			// mesh, which extraction never reads, so an item of a mesh that morphs by its default weights draws undeformed.
			if (Utils::MorphsByDefault(mirror) && StaticMorphLogged.insert(item.Entity).second)
			{
				ENGINE_CORE_WARN("Entity {} is static scenery, but its mesh {} morphs by its default morph weights, which the static path does not apply: it "
								 "draws undeformed; give the entity Static {{Scope: NotStatic}} so it draws deformed (logged once per entity)",
					item.Entity, item.Mesh);
			}
			const bool mirrored = glm::determinant(glm::mat3(item.World)) < 0.0f;
			for (uint32_t submeshIndex = 0; submeshIndex < submeshes.size(); ++submeshIndex)
			{
				const GpuSubmesh& submesh = submeshes[submeshIndex];
				if (submesh.IndexCount == 0)
					continue;
				const AssetHandle materialHandle = Detail::SelectSubmeshMaterial(item, defaults, submesh.MaterialSlot);
				const GpuMaterial& material = Cache->GetMaterial(materialHandle);
				if (materialsSeen.insert(materialHandle).second)
				{
					prepared.Materials.push_back(materialHandle);
					prepared.MaterialGenerations.push_back({ .Handle = materialHandle, .Version = material.Generation });
				}
				// PrepareViewDraws' variants: M31's Mask variants for a dissolving or fading Opaque material, the cull mode of the
				// world matrix's mirroring and the material's sides. M46 (W46.4): one key per level (its index range) and per
				// fade state (a fading level of an Opaque material draws with the Mask variants), and the material's wind.
				const Utils::MeshCullMode cullMode = Utils::SelectCullMode(mirrored, material.DoubleSided);
				const bool windy = material.Wind.Enabled;
				const uint32_t windResponse = windy ? PackWindResponse(material.Wind) : 0U;
				Detail::SceneRendererStaticDraw draw{ .Levels = 1 + entry.LodCount, .Submesh = submeshIndex, .Bounds = submesh.Bounds };
				draw.Keys.fill(Detail::NoStaticDrawKey);
				draw.FadeKeys.fill(Detail::NoStaticDrawKey);
				for (uint32_t level = 0; level < draw.Levels; ++level)
				{
					const uint32_t indexOffset = level == 0 ? submesh.IndexOffset : submesh.Lods[level - 1].IndexOffset;
					const uint32_t indexCount = level == 0 ? submesh.IndexCount : submesh.Lods[level - 1].IndexCount;
					if (indexCount == 0)
						continue;
					for (const bool fading : { false, true })
					{
						const AlphaMode mode = SelectMeshAlphaMode(material.AlphaMode, item.Parameters, false, fading ? 1.0f : 0.0f);
						const bool blend = mode == AlphaMode::Blend;
						const Detail::SceneRendererStaticKey key{
							.Key = { .Pipeline = Utils::GetForwardVariant(mode, cullMode), .Material = materialHandle, .Mesh = item.Mesh, .Submesh = submeshIndex, .Lod = level },
							.VertexBuffer = vertices,
							.AttributeBuffer = attributes,
							.IndexBuffer = indices,
							.IndexOffset = indexOffset,
							.IndexCount = indexCount,
							.PrepassVariant = blend ? 0U : Utils::GetPrepassVariant(mode, cullMode),
							.Blend = blend,
							.WindBuffer = wind,
							.Wind = windy,
							.WindResponse = windResponse
						};
						(fading ? draw.FadeKeys : draw.Keys)[level] = internKey(key);
					}
				}
				prepared.Draws.push_back(draw);
				++prepared.SubmeshCount;
				if (submesh.Bounds.IsEmpty())
					prepared.Unbounded = true;
				else
					box.Extend(Utils::GetStaticWorldBox(submesh.Bounds, item.World));
			}
			entry.DrawCount = static_cast<uint32_t>(prepared.Draws.size()) - entry.FirstDraw;
			prepared.Items.push_back(entry);

			// The item's shadow casters, by PrepareCasters' rule (their keys and M29 signatures fixed for the cell's version). M46
			// (W46.4): level 0's draws make the casters, each further level's draws give their submesh's caster its key and
			// signature there; a face chooses the level (ShadowPass::PrepareStaticFaces).
			const size_t firstCaster = prepared.Casters.size();
			for (uint32_t level = 0; level <= entry.LodCount; ++level)
			{
				casterDraws.clear();
				ShadowPass::AppendStaticItemCasters(*Cache, item, level, casterDraws);
				for (const StaticCasterDraw& caster : casterDraws)
				{
					const ShadowCasterDraw& draw = caster.Draw;
					const StaticShadowKey key{ .Key = caster.Key,
						.Vertices = draw.Vertices,
						.Attributes = draw.Attributes,
						.Indices = draw.Indices,
						.IndexOffset = draw.IndexOffset,
						.IndexCount = draw.IndexCount,
						.Variant = draw.Variant,
						.ReflectedVariant = draw.ReflectedVariant,
						.MaterialConstants = draw.MaterialConstants,
						.BaseColorMap = draw.BaseColorMap,
						.Wind = draw.Wind,
						.WindCaster = caster.Wind,
						.WindResponse = caster.WindResponse };
					if (level != 0)
					{
						// The submesh's caster from level 0: a submesh casts at every level or none (its material decides), and has
						// a level-0 caster wherever it has indices there.
						const auto owner = std::find_if(prepared.Casters.begin() + static_cast<std::ptrdiff_t>(firstCaster), prepared.Casters.end(),
							[this, &caster](const StaticShadowCaster& candidate)
						{
							return StaticCasterKeys[candidate.Key].Key.Submesh == caster.Key.Submesh;
						});
						ENGINE_CORE_ASSERT(owner != prepared.Casters.end(), "a static caster's further level must have its level 0");
						if (owner != prepared.Casters.end())
							owner->Levels[level - 1] = { .Key = internCasterKey(key), .Signature = draw.Signature };
						continue;
					}
					const ShadowBox casterShadowBox = ComputeShadowCasterBox(draw.Bounds, draw.World);
					StaticShadowCaster made{ .Key = internCasterKey(key),
						.Item = itemIndex,
						.Bounds = draw.Bounds,
						.World = draw.World,
						.Box = casterShadowBox,
						.Signature = draw.Signature,
						.LodScreenSizes = entry.LodScreenSizes,
						.LodCount = entry.LodCount,
						.MeshBounds = entry.MeshBounds,
						.Distance = entry.Distance,
						.Wind = caster.Wind };
					prepared.Casters.push_back(made);
					if (draw.Bounds.IsEmpty())
						prepared.CastersUnbounded = true;
					else
						casterBox.Extend(Aabb{ .Min = casterShadowBox.Centre - casterShadowBox.HalfExtent, .Max = casterShadowBox.Centre + casterShadowBox.HalfExtent });
				}
			}
		}
		prepared.Box = box;
		prepared.CasterBox = casterBox;
		std::ranges::sort(prepared.Entities);
		return prepared;
	}

	void SceneRenderer::State::ReleaseStaticCellKeys(const Detail::SceneRendererStaticCell& cell)
	{
		const auto release = [this](uint32_t key)
		{
			if (key == Detail::NoStaticDrawKey || --StaticKeyUses[key] != 0)
				return;
			--StaticKeysInUse;
			StaticKeys[key] = { .Key = StaticKeys[key].Key };
		};
		const auto releaseCaster = [this](uint32_t key)
		{
			if (key == NoStaticShadowKey || --StaticCasterKeyUses[key] != 0)
				return;
			--StaticKeysInUse;
			StaticCasterKeys[key] = { .Key = StaticCasterKeys[key].Key };
		};
		for (const Detail::SceneRendererStaticDraw& draw : cell.Draws)
		{
			// M46 (W46.4): every level's keys, unfaded and fading.
			for (uint32_t level = 0; level < draw.Levels; ++level)
			{
				release(draw.Keys[level]);
				release(draw.FadeKeys[level]);
			}
		}
		for (const StaticShadowCaster& caster : cell.Casters)
		{
			releaseCaster(caster.Key);
			for (const StaticShadowLevel& level : caster.Levels)
				releaseCaster(level.Key);
		}
	}

	void SceneRenderer::State::ClearStaticKeys()
	{
		StaticCells.clear();
		StaticKeys.clear();
		StaticKeyIndex.clear();
		StaticKeyUses.clear();
		StaticCasterKeys.clear();
		StaticCasterKeyIndex.clear();
		StaticCasterKeyUses.clear();
		StaticKeysInUse = 0;
	}

	void SceneRenderer::State::AppendStaticViewDraws(Detail::SceneRendererFrame& frame, const CameraData& camera, const Detail::SceneRendererMirrorCull* mirror,
		std::vector<Detail::SceneRendererDraw>& opaque, std::vector<Detail::SceneRendererDraw>& transparent,
		std::map<AssetHandle, Detail::SceneRendererMaterial>& materials, std::set<AssetHandle>& culledMaterials, Status& status)
	{
		if (frame.StaticCells.empty())
			return;
		const bool main = mirror == nullptr;
		// The view's material set of each key, resolved once (null: its draws are skipped, as a dynamic draw's).
		std::vector<std::optional<nvrhi::IBindingSet*>> keySets(StaticKeys.size());
		const auto resolveKey = [this, &keySets, &materials, &status](uint32_t key) -> nvrhi::IBindingSet*
		{
			if (keySets[key].has_value())
				return *keySets[key];
			const AssetHandle handle = StaticKeys[key].Key.Material;
			auto found = materials.find(handle);
			if (found == materials.end())
			{
				// A material whose set cannot be created skips its draws; the first such error is the render's.
				Result<Detail::SceneRendererMaterial> resolved = ResolveMaterial(handle);
				if (!resolved.has_value() && status.has_value())
					status = std::unexpected(std::move(resolved).error());
				found = materials.emplace(handle, resolved.value_or(Detail::SceneRendererMaterial{})).first;
			}
			keySets[key] = found->second.Set;
			return found->second.Set;
		};
		// R-MATSET (M18): a material whose draws left the view keeps the set it has (PrepareViewDraws' rule).
		const auto keep = [this, &materials, &culledMaterials](AssetHandle handle)
		{
			if (!materials.contains(handle) && Materials.Contains(handle) && culledMaterials.insert(handle).second)
				Materials.Keep(handle, Cache->GetMaterial(handle).Generation);
		};
		const auto makeDraw = [this, &frame](uint32_t key, nvrhi::IBindingSet* set)
		{
			const Detail::SceneRendererStaticKey& data = StaticKeys[key];
			Detail::SceneRendererDraw draw;
			draw.VertexBuffer = data.VertexBuffer.Get();
			draw.AttributeBuffer = data.AttributeBuffer.Get();
			draw.IndexBuffer = data.IndexBuffer.Get();
			draw.IndexOffset = data.IndexOffset;
			draw.IndexCount = data.IndexCount;
			// M46 (W46.4): the wind stream at vertex binding 3, the key's level, and its material's wind in a render with a Wind
			// (the draw pushes it, DrawConstantsWind: a static record holds none).
			draw.WindBuffer = data.WindBuffer.Get();
			draw.Lod = data.Key.Lod;
			draw.Wind = frame.HasWind && data.Wind;
			draw.WindResponse = draw.Wind ? data.WindResponse : 0U;
			draw.Material = set;
			draw.ForwardVariant = data.Key.Pipeline;
			draw.PrepassVariant = data.PrepassVariant;
			draw.MaterialHandle = data.Key.Material;
			draw.Mesh = data.Key.Mesh;
			draw.Submesh = data.Key.Submesh;
			return draw;
		};

		// M46 (decision 3D): a view chooses levels by its own camera (a mirror view's, never its cull camera's, whose projection
		// is cropped to the glass), as PrepareViewDraws does.
		const CameraData& lodCamera = mirror != nullptr && mirror->View != nullptr ? *mirror->View : camera;
		for (const Detail::SceneRendererStaticCellRef& reference : frame.StaticCells)
		{
			const Detail::SceneRendererStaticCell& cell = *reference.Prepared;
			if (cell.NonFinite.has_value() && status.has_value())
			{
				status = MakeError(ErrorCode::InvalidArgument, "the world matrix of entity {} in the render snapshot's static set is not finite; its draw is skipped",
					*cell.NonFinite);
			}
			// M46 (W46.4): a cell whose every drawable item lies beyond its CullDistance is culled by distance whole, each item
			// counted as the dynamic path counts it, before its frustum test.
			if (Utils::IsStaticCellBeyondCullDistance(cell, lodCamera.Position))
			{
				if (main)
					Stats.DistanceCulledMeshes += cell.DrawableItems;
				continue;
			}
			// The cell against the view's frustum and FarClip and, in a mirror view, its glass's plane: its box inflated for the
			// view (InflateStaticCellBox), so the answer holds for each of its draws' own tests below.
			StaticCellVisibility visibility = StaticCellVisibility::Partial;
			if (!cell.Unbounded)
			{
				const Aabb box = InflateStaticCellBox(cell.Box, camera.Position);
				visibility = ClassifyStaticCell(box, camera);
				if (mirror != nullptr && visibility != StaticCellVisibility::Outside)
				{
					const StaticCellVisibility plane = ClassifyStaticCellClipPlane(box, mirror->ClipPlane);
					if (plane != StaticCellVisibility::Inside)
						visibility = plane;
				}
			}
			if (visibility == StaticCellVisibility::Outside)
			{
				if (main)
				{
					// As the dynamic path counts them: an item beyond its CullDistance is culled by distance before its submeshes
					// meet the frustum (M46), the others' submeshes are culled.
					if (cell.DistanceItems == 0)
						Stats.CulledSubmeshes += cell.SubmeshCount;
					else
					{
						for (const Detail::SceneRendererStaticItem& item : cell.Items)
						{
							if (!item.Finite || !item.Drawable)
								continue;
							if (item.Distance.CullDistance > 0.0f
								&& SelectViewMeshLod(std::span<const float>(item.LodScreenSizes.data(), item.LodCount), item.MeshBounds, item.World, item.Distance, lodCamera)
										.DrawCount
									== 0)
								++Stats.DistanceCulledMeshes;
							else
								Stats.CulledSubmeshes += item.DrawCount;
						}
					}
				}
				for (const AssetHandle handle : cell.Materials)
					keep(handle);
				continue;
			}
			if (main)
				++frame.StaticCellsVisible;
			for (uint32_t itemIndex = 0; itemIndex < cell.Items.size(); ++itemIndex)
			{
				const Detail::SceneRendererStaticItem& item = cell.Items[itemIndex];
				if (!item.Finite || !item.Drawable)
					continue;
				// M46 (decision 3D): the item's levels in this view; an item at or beyond its CullDistance draws nothing.
				const MeshLodSelection lods =
					SelectViewMeshLod(std::span<const float>(item.LodScreenSizes.data(), item.LodCount), item.MeshBounds, item.World, item.Distance, lodCamera);
				if (lods.DrawCount == 0)
				{
					if (main)
						++Stats.DistanceCulledMeshes;
					continue;
				}
				const uint32_t record = reference.FirstRecord + itemIndex;
				bool drawn = false;
				for (uint32_t index = item.FirstDraw; index < item.FirstDraw + item.DrawCount; ++index)
				{
					const Detail::SceneRendererStaticDraw& draw = cell.Draws[index];
					if (visibility != StaticCellVisibility::Inside
						&& (IsOutsideView(draw.Bounds, item.World, camera)
							|| (mirror != nullptr && IsBehindPlanarReflectionClipPlane(draw.Bounds, item.World, mirror->ClipPlane))))
					{
						if (main)
							++Stats.CulledSubmeshes;
						keep(StaticKeys[draw.Keys[0]].Key.Material);
						continue;
					}
					// M46 (decision 3E): one draw per level the view draws, its key that level's (the fading one while it fades),
					// its fade in its InstanceIndices entry.
					for (uint32_t lodIndex = 0; lodIndex < lods.DrawCount; ++lodIndex)
					{
						const MeshLodDraw& lod = lods.Draws[lodIndex];
						const uint32_t key = lod.Fade != 0.0f ? draw.FadeKeys[lod.Level] : draw.Keys[lod.Level];
						if (key == Detail::NoStaticDrawKey)
							continue;
						nvrhi::IBindingSet* set = resolveKey(key);
						if (set == nullptr)
							break; // its GPU objects are missing (reported); every level shares the material
						drawn = true;
						if (main)
							++Stats.MeshLodDraws[std::min<uint32_t>(lod.Level, static_cast<uint32_t>(Stats.MeshLodDraws.size()) - 1U)];
						if (StaticKeys[key].Blend)
						{
							// PrepareViewDraws' view depth, from the same products, so the back-to-front order is the dynamic path's.
							Detail::SceneRendererDraw transparentDraw = makeDraw(key, set);
							const glm::mat4 worldView = camera.View * item.World;
							const glm::vec3 centre = draw.Bounds.IsEmpty() ? glm::vec3(0.0f) : (draw.Bounds.Min + draw.Bounds.Max) * 0.5f;
							transparentDraw.ViewDepth = -(worldView * glm::vec4(centre, 1.0f)).z;
							transparentDraw.Entity = cell.Cell->Items[itemIndex].Entity;
							transparentDraw.Bounds = draw.Bounds;
							transparentDraw.Instance = record; // its InstanceIndices entry once PrepareViewDraws has sorted the view's draws
							transparentDraw.LodFade = lod.Fade;
							transparentDraw.StaticInstances = 1;
							transparent.push_back(transparentDraw);
							continue;
						}
						std::vector<InstanceIndex>& bucket = StaticBuckets[key];
						if (bucket.empty())
							StaticTouched.push_back(key);
						bucket.push_back({ .Record = record, .Reserved = std::bit_cast<uint32_t>(lod.Fade) });
					}
				}
				if (drawn && main)
				{
					++frame.StaticVisibleItems;
					if (lods.CrossFade)
						++Stats.LodCrossFades;
				}
			}
		}

		// One opaque draw per key the view sees, its instances in cell order then item order.
		for (const uint32_t key : StaticTouched)
		{
			std::vector<InstanceIndex>& bucket = StaticBuckets[key];
			Detail::SceneRendererDraw draw = makeDraw(key, *keySets[key]);
			draw.Instance = static_cast<uint32_t>(InstanceIndices.size());
			draw.StaticInstances = static_cast<uint32_t>(bucket.size());
			InstanceIndices.insert(InstanceIndices.end(), bucket.begin(), bucket.end());
			opaque.push_back(draw);
			bucket.clear();
		}
		StaticTouched.clear();
	}

	void SceneRenderer::State::PrepareStaticShadows(Detail::SceneRendererFrame& frame, const ShadowCasterView& view)
	{
		frame.StaticShadowCells.clear();
		for (const Detail::SceneRendererStaticCellRef& reference : frame.StaticCells)
		{
			const Detail::SceneRendererStaticCell& cell = *reference.Prepared;
			if (cell.Casters.empty())
				continue;
			frame.StaticShadowCells.push_back({ .Box = cell.CasterBox, .Unbounded = cell.CastersUnbounded, .FirstRecord = reference.FirstRecord, .Casters = cell.Casters });
		}
		if (frame.StaticShadowCells.empty())
			return;
		frame.StaticShadows = ShadowPass::PrepareStaticFaces(frame.StaticShadowCells, StaticCasterKeys.size(), frame.Cascades.Count != 0 ? &frame.Cascades : nullptr,
			!frame.Spots.Tiles.empty() ? &frame.Spots : nullptr, !frame.Points.Lights.empty() ? &frame.Points : nullptr, Device->GetInfo().DepthClamp,
			StaticFaceBuckets, InstanceIndices, view);
	}

	std::vector<MeshDrawItem> SceneRenderer::State::CollectSelectedStaticItems(const Detail::SceneRendererFrame& frame) const
	{
		std::vector<MeshDrawItem> selected;
		const RenderSnapshot& snapshot = *frame.Snapshot;
		std::vector<uint32_t> items;
		for (const Detail::SceneRendererStaticCellRef& reference : frame.StaticCells)
		{
			const Detail::SceneRendererStaticCell& cell = *reference.Prepared;
			items.clear();
			for (const UUID& entity : snapshot.SelectedEntities)
			{
				const auto found = std::ranges::lower_bound(cell.Entities, std::pair(entity, uint32_t{ 0 }));
				for (auto match = found; match != cell.Entities.end() && match->first == entity; ++match)
					items.push_back(match->second);
			}
			std::ranges::sort(items);
			const auto unique = std::ranges::unique(items);
			items.erase(unique.begin(), unique.end());
			for (const uint32_t item : items)
				selected.push_back(cell.Cell->Items[item]);
		}
		return selected;
	}

	Status SceneRenderer::State::EnsureInstanceIndexBuffer(Detail::SceneRendererFrame& frame)
	{
		// At least Utils::MinInstanceIndexCapacity entries, so every set binds a buffer of InstanceIndex; grown geometrically and
		// never shrunk, as the Instances buffer.
		const uint64_t needed = std::max<uint64_t>(InstanceIndices.size(), 1);
		const uint64_t capacity = InstanceIndexBuffer != nullptr ? InstanceIndexBuffer->getDesc().byteSize / sizeof(InstanceIndex) : 0;
		if (needed <= capacity)
			return {};
		nvrhi::BufferDesc desc;
		desc.byteSize = std::max({ needed, capacity * 2, Utils::MinInstanceIndexCapacity }) * sizeof(InstanceIndex);
		desc.structStride = sizeof(InstanceIndex);
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.InstanceIndices";
		Result<nvrhi::BufferHandle> created = Device->CreateBuffer(desc);
		if (created.has_value())
		{
			InstanceIndexBuffer = std::move(*created);
			return {};
		}
		if (InstanceIndexBuffer == nullptr)
			return std::unexpected(std::move(created).error());
		// The earlier buffer stays bound, and the render draws nothing static.
		frame.KeepFirstError(std::unexpected(std::move(created).error().WithContext("the render's static items are not drawn")));
		DropStaticDraws(frame);
		return {};
	}

	void SceneRenderer::State::DropStaticDraws(Detail::SceneRendererFrame& frame)
	{
		const auto drop = [](std::vector<Detail::SceneRendererDraw>& draws)
		{
			// A static draw batches with static draws only (StaticVertexStream), so the dynamic draws' batches stay whole.
			std::erase_if(draws, [](const Detail::SceneRendererDraw& draw)
			{
				return draw.StaticInstances != 0;
			});
		};
		drop(OpaqueDraws);
		drop(TransparentDraws);
		for (uint32_t slot = 0; slot < frame.PlanarReflectionCount; ++slot)
		{
			drop(ReflectionSlots[slot].OpaqueDraws);
			drop(ReflectionSlots[slot].TransparentDraws);
		}
		frame.StaticShadows = {};
		frame.StaticCellsVisible = 0;
		frame.StaticVisibleItems = 0;
		InstanceIndices.clear();
	}

	nvrhi::IBuffer* SceneRenderer::State::GetStaticInstancesBinding(const Detail::SceneRendererFrame& frame) const
	{
		return frame.StaticInstances != nullptr ? frame.StaticInstances.Get() : InstanceBuffer.Get();
	}

}
