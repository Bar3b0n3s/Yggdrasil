#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Core/Log.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/Fog.h"
#include "Engine/Renderer/LightClusterPass.h"
#include "Engine/Renderer/LightClusters.h"
#include "Engine/Renderer/ParticlePass.h"
#include "Engine/Renderer/PlanarReflectionPass.h"
#include "Engine/Renderer/PlanarReflections.h"
#include "Engine/Renderer/Private/ForwardPipelines.h"
#include "Engine/Renderer/Private/LightingInputs.h"
#include "Engine/Renderer/Private/SceneRendererState.h"
#include "Engine/Renderer/RenderPassDeclaration.h"
#include "Engine/Renderer/RenderPrepare.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Engine/Renderer/SkyAtmosphere.h"
#include "Engine/Renderer/SkyboxPass.h"
#include "Engine/Renderer/TerrainPass.h"
#include "Shared/DrawConstants.h"
#include "Shared/FogConstants.h"
#include "Shared/InstanceRecord.h"
#include "Shared/LightClusterConstants.h"
#include "Shared/ShaderLight.h"
#include "Shared/TerrainConstants.h"
#include "Shared/ViewConstants.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// The planar reflections' stage functions (M42, Docs/Decisions/0059-m42-planar-reflections.md decision 3;
// Renderer/PlanarReflections.h for the CPU plan, SceneRenderer.h for the pass list).
//
// Prepare (PreparePlanarReflections, at the end of the Prepare stage): in a Lit view with a camera outside the wireframe,
// a snapshot reflection is a candidate when its plane is finite, the camera faces its reflecting side and a draw of its
// entity survived the main view's culling; its glass's rectangle on screen is the union of those draws' bounds'
// rectangles, whose pixel area ranks it (RankPlanarReflections: the largest first, ties by entity, at most
// MaxPlanarReflectionsPerView). Each ranked reflection takes the next slot: its mirror camera (the camera mirrored in its
// plane, S * View * R, at ComputePlanarReflectionSize of the scene resolution, with ComputePlanarReflectionMipCount's chain
// for the view's SsrMaxRoughness), its draws (PrepareViewDraws with the mirror camera cropped to the glass's rectangle
// widened by the chain's blur footprint, ComputePlanarReflectionBlurMargin, and the clip plane; their instance records after
// the main view's, the shadow casters' and the selection's), its light list (the main view's at their indices, then the
// lights only it sees, unshadowed), its ViewConstants (ReflectionView 1, ClipPlane, CascadeDepthPlane) and fog. A reflection
// whose objects cannot be created, and every reflection of a view whose probe constants could not be created (the forward
// set binds them), falls back for the render (counted as dropped). Then the main view's records of each drawn reflection's
// glass carry DrawFlagPlanarReflection and its slot.
//
// Forward (RecordPlanarReflections, first in the Forward stage): each slot's mirror view into its reflection under its one
// row PlanarReflection<k>, which times the whole view, scissored to the rectangle its draws were culled to: the clears, the
// prepass and TerrainDepthNormal, its light clusters (LightClusterPass::RecordInRow, inside the row; the lead's decision on
// W42.1's review), then ForwardOpaque, TerrainForward, the skybox, ForwardTransparent, the particles (play scenes) and the
// mip chain. A mirror view binds the main view's shadow maps and constants, environment, probes and instance buffer, the
// neutral white occlusion and far depth (no GTAO), and the neutral black reflections (no recursion).
// SSR, TAA, exposure, bloom, the tonemap and every 2D, text, UI and overlay pass stay out: the reflection is the main
// view's pre-exposure radiance, which the main view's post chain exposes.

namespace Engine {

	namespace Utils {

		// The rectangle `rect` (the mirror camera's normalized device coordinates, +y up) in the pixels of a `width` x `height`
		// target (row 0 at the top, the viewport's Y flip; Shared/ViewConstants.h), its bounds rounded outwards and clamped to
		// the target.
		[[nodiscard]] static nvrhi::Rect ToReflectionScissor(const PlanarReflectionScreenRect& rect, uint32_t width, uint32_t height)
		{
			const auto toPixel = [](double value, uint32_t limit)
			{
				return static_cast<int>(std::clamp(value, 0.0, static_cast<double>(limit)));
			};
			const double minX = std::floor((static_cast<double>(rect.Min.x) * 0.5 + 0.5) * width);
			const double maxX = std::ceil((static_cast<double>(rect.Max.x) * 0.5 + 0.5) * width);
			const double minY = std::floor((0.5 - static_cast<double>(rect.Max.y) * 0.5) * height);
			const double maxY = std::ceil((0.5 - static_cast<double>(rect.Min.y) * 0.5) * height);
			return nvrhi::Rect(toPixel(minX, width), toPixel(maxX, width), toPixel(minY, height), toPixel(maxY, height));
		}

		// `slot`'s reflection level 0 alone, or with `depth`.
		[[nodiscard]] static nvrhi::FramebufferDesc MakeReflectionFramebufferDesc(nvrhi::ITexture* color, nvrhi::ITexture* depth)
		{
			nvrhi::FramebufferDesc desc;
			desc.addColorAttachment(nvrhi::FramebufferAttachment().setTexture(color).setSubresources(nvrhi::TextureSubresourceSet(0, 1, 0, 1)));
			if (depth != nullptr)
				desc.setDepthAttachment(depth);
			return desc;
		}

	}

	void SceneRenderer::State::PreparePlanarReflections(Detail::SceneRendererFrame& frame)
	{
		const RenderSnapshot& snapshot = *frame.Snapshot;
		for (Detail::SceneRendererReflectionSlot& slot : ReflectionSlots)
			slot.Drawn = false;
		for (Detail::SceneRendererReflectionScratch& scratch : ReflectionScratch)
			scratch.Used = false;
		frame.PlanarReflections = PlanarReflectionConstants{};
		frame.PlanarReflections.SsrMaxRoughness = std::isfinite(snapshot.Post.SsrMaxRoughness) ? snapshot.Post.SsrMaxRoughness : 0.0f;
		if (snapshot.PlanarReflections.empty())
			return;

		// The candidates: in a view that draws reflections (SSR's: a Lit view with a camera outside the wireframe), the
		// reflections with a finite plane facing the camera whose glass a draw of the main view shows.
		const bool drawable = frame.HasCamera && frame.IsLit && !frame.Wireframe;
		std::vector<PlanarReflectionCandidate> candidates;
		std::vector<PlanarReflectionPlane> planes(snapshot.PlanarReflections.size());
		std::vector<PlanarReflectionScreenRect> rects(snapshot.PlanarReflections.size());
		for (size_t index = 0; drawable && index < snapshot.PlanarReflections.size(); ++index)
		{
			const PlanarReflectionItem& item = snapshot.PlanarReflections[index];
			const std::optional<PlanarReflectionPlane> plane = ComputePlanarReflectionPlane(item);
			if (!plane || !item.Entity.IsValid() || !IsPlanarReflectionFacing(frame.Camera, *plane))
				continue;
			std::optional<PlanarReflectionScreenRect> rect;
			const auto unite = [&snapshot, &frame, &item, &rect](const std::vector<Detail::SceneRendererDraw>& draws)
			{
				for (const Detail::SceneRendererDraw& draw : draws)
				{
					if (draw.Entity != item.Entity)
						continue;
					const std::optional<PlanarReflectionScreenRect> drawn = ComputePlanarReflectionScreenRect(draw.Bounds, snapshot.Meshes[draw.SnapshotIndex].World,
						frame.Camera);
					if (!drawn)
						continue;
					rect = rect ? PlanarReflectionScreenRect{ .Min = glm::min(rect->Min, drawn->Min), .Max = glm::max(rect->Max, drawn->Max) } : *drawn;
				}
			};
			unite(OpaqueDraws);
			unite(TransparentDraws);
			if (!rect)
				continue;
			planes[index] = *plane;
			rects[index] = *rect;
			candidates.push_back({ .Item = index, .Entity = item.Entity, .ScreenArea = rect->GetPixelArea(Targets.Width, Targets.Height) });
		}
		const PlanarReflectionSelection selection = RankPlanarReflections(candidates);
		frame.CulledPlanarReflections = static_cast<uint32_t>(snapshot.PlanarReflections.size() - candidates.size());
		frame.DroppedPlanarReflections = selection.Dropped;
		if (selection.Dropped > 0 && !PlanarReflectionLimitLogged)
		{
			PlanarReflectionLimitLogged = true;
			ENGINE_CORE_WARN("{} planar reflection(s) beyond the view's {} fall back to the reflection probes and screen-space reflections (logged once per view)",
				selection.Dropped, MaxPlanarReflectionsPerView);
		}

		// Each ranked reflection in the next slot; one whose objects cannot be created falls back for this render, and so does
		// every one of a view whose probe constants could not be created (EnsureProbeViewResources kept that error), since a
		// mirror view's forward set binds them as the main view's does, whose forward passes are skipped without them.
		uint32_t drawn = 0;
		for (const size_t index : selection.Slots)
		{
			if (ProbeResources.Constants == nullptr)
			{
				++frame.DroppedPlanarReflections;
				continue;
			}
			Detail::SceneRendererReflectionSlot& slot = ReflectionSlots[drawn];
			Status prepared = PrepareReflectionSlot(frame, slot, index, planes[index], rects[index]);
			if (!prepared.has_value())
			{
				frame.KeepFirstError(std::unexpected(std::move(prepared).error().WithContext(std::format("while preparing the planar reflection of entity {}",
					snapshot.PlanarReflections[index].Entity))));
				++frame.DroppedPlanarReflections;
				continue;
			}
			slot.Drawn = true;
			const nvrhi::TextureDesc& color = slot.Color->getDesc();
			frame.PlanarReflections.ViewProjections[drawn] = slot.Camera.Projection * slot.Camera.View;
			frame.PlanarReflections.MipCounts[static_cast<glm::length_t>(drawn)] = static_cast<float>(color.mipLevels);
			frame.PlanarReflections.Heights[static_cast<glm::length_t>(drawn)] = static_cast<float>(color.height);
			frame.PlanarReflectionEntities.push_back(slot.Entity);
			++drawn;
		}
		frame.PlanarReflectionCount = drawn;
		frame.PlanarReflections.Count = drawn;

		// The main view's records of a drawn reflection's glass carry its slot (Shared/DrawConstants.h).
		const auto flag = [this, drawn](const std::vector<Detail::SceneRendererDraw>& draws)
		{
			for (const Detail::SceneRendererDraw& draw : draws)
			{
				for (uint32_t slot = 0; slot < drawn; ++slot)
				{
					if (draw.Entity != ReflectionSlots[slot].Entity)
						continue;
					InstanceRecord& record = Instances[draw.Instance];
					record.Flags |= DrawFlagPlanarReflection;
					record.PlanarReflectionSlot = slot;
					break;
				}
			}
		};
		flag(OpaqueDraws);
		flag(TransparentDraws);
	}

	Status SceneRenderer::State::PrepareReflectionSlot(Detail::SceneRendererFrame& frame, Detail::SceneRendererReflectionSlot& slot, size_t item,
		const PlanarReflectionPlane& plane, const PlanarReflectionScreenRect& rect)
	{
		const RenderSnapshot& snapshot = *frame.Snapshot;
		const PlanarReflectionItem& reflection = snapshot.PlanarReflections[item];
		const glm::uvec2 size = ComputePlanarReflectionSize(Targets.Width, Targets.Height, reflection.ResolutionScale);
		const uint32_t mipCount = ComputePlanarReflectionMipCount(size.x, size.y, frame.PlanarReflections.SsrMaxRoughness);
		slot.Entity = reflection.Entity;
		slot.Camera = MakePlanarReflectionCamera(frame.Camera, plane, size.x, size.y);
		slot.Clusters = ComputeLightClusterGrid(slot.Camera, size.x, size.y, snapshot.Quality.MaxShadedLights);
		ENGINE_TRY(EnsureReflectionSlot(slot, size.x, size.y, mipCount, slot.Clusters));
		// The glass's rectangle widened by the blur's footprint at the chain's last level, so every texel a sample on the glass
		// averages was rendered with the scene in it: the mirror view culls to it and rasterizes in it.
		const PlanarReflectionScreenRect widened = WidenPlanarReflectionScreenRect(rect, size.x, size.y, ComputePlanarReflectionBlurMargin(size.x, size.y, mipCount));
		slot.Scissor = Utils::ToReflectionScissor(MirrorPlanarReflectionScreenRect(widened), size.x, size.y);

		// The draws: every mesh item but the glass's own, culled by the mirror camera cropped to the widened rectangle and by
		// the plane.
		const glm::vec4 clipPlane = ComputePlanarReflectionClipPlane(plane);
		const CameraData cull = MakePlanarReflectionCullCamera(slot.Camera, widened);
		const Detail::SceneRendererMirrorCull mirror{ .Excluded = reflection.Entity, .ClipPlane = clipPlane };
		frame.KeepFirstError(PrepareViewDraws(frame, snapshot, cull, slot.OpaqueDraws, slot.TransparentDraws, &mirror));

		// The light list: the main view's, whose shadow slots and directional index stay valid, then the lights only the mirror
		// view sees, unshadowed, while the snapshot's cap allows.
		slot.Lights = Lights;
		std::vector<uint32_t> listed = frame.VisibleLights;
		std::ranges::sort(listed);
		const LightCullResult seen = CullLights(snapshot.Lights, cull, snapshot.Quality.MaxShadedLights);
		for (const uint32_t index : seen.Visible)
		{
			if (slot.Lights.size() >= snapshot.Quality.MaxShadedLights)
				break;
			if (!std::ranges::binary_search(listed, index))
				slot.Lights.push_back(Utils::MakeShaderLight(snapshot.Lights[index]));
		}

		// The mirror view's constants: unjittered, with the main view's exposure; clipped at the glass; the main view's cascades.
		slot.View = Detail::MakeSceneViewConstants(slot.Camera, size.x, size.y, frame.View.Exposure);
		slot.View.LightCount = static_cast<uint32_t>(slot.Lights.size());
		slot.View.ReflectionView = 1;
		slot.View.ClipPlane = clipPlane;
		slot.View.CascadeDepthPlane = ComputeCascadeDepthPlane(frame.Camera);
		slot.Fog = MakeFogConstants(snapshot.Fog, slot.Camera, snapshot.Lights, GetSkyGlowLight(snapshot), frame.Environment, frame.Fog.Enabled != 0);
		slot.Fog.SkyIsClearColor = frame.Fog.SkyIsClearColor;
		return {};
	}

	Status SceneRenderer::State::EnsureReflectionSlot(Detail::SceneRendererReflectionSlot& slot, uint32_t width, uint32_t height, uint32_t mipCount,
		const LightClusterGrid& clusters)
	{
		if (slot.Color == nullptr || slot.Color->getDesc().width != width || slot.Color->getDesc().height != height || slot.Color->getDesc().mipLevels != mipCount)
		{
			slot.ForwardFramebuffer = nullptr;
			slot.ParticleFramebuffer = nullptr;
			slot.Color = nullptr;
			ENGINE_TRY_ASSIGN(slot.Color, TargetPool->Acquire(PlanarReflectionPass::GetColorDesc(width, height, mipCount)));
		}
		// The prepass scratch of this size, which the slots of the size share.
		auto scratch = std::ranges::find_if(ReflectionScratch, [width, height](const Detail::SceneRendererReflectionScratch& candidate)
		{
			return candidate.Width == width && candidate.Height == height;
		});
		if (scratch == ReflectionScratch.end())
		{
			Detail::SceneRendererReflectionScratch created;
			created.Width = width;
			created.Height = height;
			ENGINE_TRY_ASSIGN(created.Depth, Detail::CreateSceneAttachment(*TargetPool, width, height, SceneDepthFormat, "PlanarReflectionDepth"));
			ENGINE_TRY_ASSIGN(created.Normals, Detail::CreateSceneAttachment(*TargetPool, width, height, SceneNormalsFormat, "PlanarReflectionNormals"));
			ENGINE_TRY_ASSIGN(created.Velocity, Detail::CreateSceneAttachment(*TargetPool, width, height, SceneVelocityFormat, "PlanarReflectionVelocity"));
			ENGINE_TRY_ASSIGN(created.Roughness, Detail::CreateSceneAttachment(*TargetPool, width, height, SceneRoughnessFormat, "PlanarReflectionRoughness"));
			ENGINE_TRY_ASSIGN(created.PrepassFramebuffer,
				Device->CreateFramebuffer(nvrhi::FramebufferDesc()
						.addColorAttachment(created.Normals)
						.addColorAttachment(created.Velocity)
						.addColorAttachment(created.Roughness)
						.setDepthAttachment(created.Depth)));
			ReflectionScratch.push_back(std::move(created));
			scratch = std::prev(ReflectionScratch.end());
		}
		scratch->Used = true;
		slot.Scratch = static_cast<size_t>(std::distance(ReflectionScratch.begin(), scratch));
		if (slot.ForwardFramebuffer == nullptr || slot.ForwardFramebuffer->getDesc().depthAttachment.texture != scratch->Depth.Get())
		{
			ENGINE_TRY_ASSIGN(slot.ForwardFramebuffer, Device->CreateFramebuffer(Utils::MakeReflectionFramebufferDesc(slot.Color, scratch->Depth)));
		}
		if (slot.ParticleFramebuffer == nullptr)
		{
			ENGINE_TRY_ASSIGN(slot.ParticleFramebuffer, Device->CreateFramebuffer(Utils::MakeReflectionFramebufferDesc(slot.Color, nullptr)));
		}
		if (slot.ViewConstantsBuffer == nullptr)
		{
			ENGINE_TRY_ASSIGN(slot.ViewConstantsBuffer, Detail::CreateViewBuffer(*Device, sizeof(ViewConstants), 0, "SceneRenderer.PlanarReflectionViewConstants"));
		}
		if (slot.FogConstantsBuffer == nullptr)
		{
			ENGINE_TRY_ASSIGN(slot.FogConstantsBuffer, Detail::CreateViewBuffer(*Device, sizeof(FogConstants), 0, "SceneRenderer.PlanarReflectionFogConstants"));
		}
		if (slot.LightBuffer == nullptr)
		{
			ENGINE_TRY_ASSIGN(slot.LightBuffer,
				Detail::CreateViewBuffer(*Device, sizeof(ShaderLight) * MaxShadedLightsCeiling, sizeof(ShaderLight), "SceneRenderer.PlanarReflectionLights"));
		}
		if (slot.LightClusterConstantsBuffer == nullptr)
		{
			ENGINE_TRY_ASSIGN(slot.LightClusterConstantsBuffer, Device->CreateBuffer(LightClusterPass::GetConstantsDesc()));
		}
		const nvrhi::BufferDesc masks = LightClusterPass::GetMaskBufferDesc(GetLightClusterCount(clusters), clusters.WordCount);
		if (slot.LightClusterBuffer == nullptr || slot.LightClusterBuffer->getDesc().byteSize < masks.byteSize)
		{
			ENGINE_TRY_ASSIGN(slot.LightClusterBuffer, Device->CreateBuffer(masks));
		}
		return {};
	}

	void SceneRenderer::State::RecordPlanarReflections(Detail::SceneRendererFrame& frame)
	{
		if (frame.PlanarReflectionCount == 0)
			return;
		const RenderSnapshot& snapshot = *frame.Snapshot;
		nvrhi::ICommandList& commandList = *frame.CommandList;
		RenderRecordingContext& recording = *frame.Recording;
		// A mirror view samples no reflection (no recursion): its records carry no flag, and its set binds the neutral one.
		std::array<nvrhi::ITexture*, MaxPlanarReflectionsPerView> neutral{};
		neutral.fill(Pipelines->EmptyPlanarReflection.Get());
		// Whether each scratch holds its cleared values: a slot clears its scratch only when an earlier slot's prepass wrote it
		// (two clears of one image with nothing between them would be a write-after-write hazard: NVRHI orders a clear after a
		// pass's writes, not after another clear).
		std::vector<uint8_t> scratchCleared(ReflectionScratch.size(), 0);
		for (uint32_t index = 0; index < frame.PlanarReflectionCount; ++index)
		{
			Detail::SceneRendererReflectionSlot& slot = ReflectionSlots[index];
			const Detail::SceneRendererReflectionScratch& scratch = ReflectionScratch[slot.Scratch];
			const std::string_view row = Detail::PlanarReflectionsRows[index];
			const Detail::SceneRendererMeshTarget prepassTarget{ .Framebuffer = scratch.PrepassFramebuffer,
				.Width = scratch.Width,
				.Height = scratch.Height,
				.Scissor = slot.Scissor };
			const Detail::SceneRendererMeshTarget forwardTarget{ .Framebuffer = slot.ForwardFramebuffer,
				.Width = scratch.Width,
				.Height = scratch.Height,
				.Scissor = slot.Scissor };

			// The clears, the prepass and the terrains' prepass.
			recording.BeginPass(row, &commandList);
			RenderPassCounters counters;
			const glm::vec3 clearColor = slot.Camera.ClearColor;
			commandList.clearTextureFloat(slot.Color, nvrhi::TextureSubresourceSet(0, 1, 0, 1), nvrhi::Color(clearColor.r, clearColor.g, clearColor.b, 1.0f));
			if (scratchCleared[slot.Scratch] == 0)
			{
				commandList.clearDepthStencilTexture(scratch.Depth, nvrhi::AllSubresources, true, 0.0f, false, 0);
				commandList.clearTextureFloat(scratch.Normals, nvrhi::AllSubresources, nvrhi::Color(0.0f));
				commandList.clearTextureFloat(scratch.Velocity, nvrhi::AllSubresources, nvrhi::Color(0.0f));
				commandList.clearTextureUInt(scratch.Roughness, nvrhi::AllSubresources, 0);
				scratchCleared[slot.Scratch] = 1;
			}
			const uint32_t drawsBeforePrepass = counters.DrawCalls;
			nvrhi::BindingSetDesc prepassDesc;
			prepassDesc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(Utils::ViewConstantsRegister, slot.ViewConstantsBuffer),
				nvrhi::BindingSetItem::Texture_SRV(Utils::BrdfLutRegister, Pipelines->DfgLut->GetTexture()),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(Utils::InstancesRegister, InstanceBuffer),
				nvrhi::BindingSetItem::Texture_SRV(Utils::DissolveNoiseRegister, frame.DissolveNoise),
				nvrhi::BindingSetItem::Sampler(Utils::LinearClampRegister, Pipelines->LinearClamp),
				nvrhi::BindingSetItem::Sampler(Utils::AnisoWrapRegister, Pipelines->AnisoWrap),
				nvrhi::BindingSetItem::PushConstants(Utils::DrawConstantsSlot, sizeof(DrawConstants)),
			};
			auto prepassSet = ViewBindings.GetOrCreate(*Device, prepassDesc, *Pipelines->Layouts.PrepassView);
			if (prepassSet)
				RecordMeshPass(commandList, Detail::SceneRendererMeshPass::Prepass, slot.OpaqueDraws, **prepassSet, frame.DebugView, counters, &prepassTarget);
			else
				frame.KeepFirstError(std::unexpected(std::move(prepassSet).error()));
			if (!snapshot.Terrains.empty() && Pipelines->Terrain != nullptr)
			{
				const TerrainPassInputs inputs{ .Snapshot = &snapshot, .Camera = &slot.Camera, .DebugView = frame.DebugView };
				frame.KeepFirstError(Pipelines->Terrain->Prepare(commandList, *Cache, slot.TerrainResources, inputs));
				if (!slot.TerrainResources.Draws.empty())
				{
					nvrhi::BindingSetDesc terrainDesc;
					terrainDesc.bindings = {
						nvrhi::BindingSetItem::ConstantBuffer(Utils::ViewConstantsRegister, slot.ViewConstantsBuffer),
						nvrhi::BindingSetItem::Texture_SRV(Utils::BrdfLutRegister, Pipelines->DfgLut->GetTexture()),
						nvrhi::BindingSetItem::Sampler(Utils::LinearClampRegister, Pipelines->LinearClamp),
						nvrhi::BindingSetItem::Sampler(Utils::AnisoWrapRegister, Pipelines->AnisoWrap),
						nvrhi::BindingSetItem::PushConstants(Utils::DrawConstantsSlot, sizeof(TerrainDrawConstants)),
					};
					auto terrainSet = ViewBindings.GetOrCreate(*Device, terrainDesc, Pipelines->Terrain->GetPrepassViewLayout());
					if (terrainSet)
						Pipelines->Terrain->RecordDepthNormal(commandList, slot.TerrainResources, *scratch.PrepassFramebuffer, false, **terrainSet, counters, slot.Scissor);
					else
						frame.KeepFirstError(std::unexpected(std::move(terrainSet).error()));
				}
			}
			// Only the prepasses write the scratch (the forward passes test its depth without writing it).
			if (counters.DrawCalls != drawsBeforePrepass)
				scratchCleared[slot.Scratch] = 0;

			// The mirror view's light clusters, inside its row (LightClusterPass::RecordInRow), so the row times its whole view; the
			// brute-force loop's constants without them.
			const bool hasLocalLight = std::ranges::any_of(slot.Lights, [](const ShaderLight& light)
			{
				return light.Type != ShaderLightTypeDirectional;
			});
			bool clustered = false;
			if (snapshot.Quality.ClusteredLights && hasLocalLight)
			{
				const Status recorded = Pipelines->LightClusters->RecordInRow(commandList, LightClusterBindings,
					{ .Grid = slot.Clusters,
						.ViewConstants = slot.ViewConstantsBuffer,
						.ClusterConstants = slot.LightClusterConstantsBuffer,
						.Lights = slot.LightBuffer,
						.Masks = slot.LightClusterBuffer },
					counters);
				clustered = recorded.has_value();
				frame.KeepFirstError(recorded);
			}
			if (!clustered)
			{
				const LightClusterConstants constants = MakeLightClusterConstants(slot.Clusters, LightClusterModeBruteForce);
				commandList.writeBuffer(slot.LightClusterConstantsBuffer, &constants, sizeof(constants));
			}

			// The mirror view's forward passes, then its mip chain.
			const nvrhi::BindingSetDesc forwardDesc = MakeForwardSetDesc(frame, slot.ViewConstantsBuffer, slot.FogConstantsBuffer, slot.LightBuffer,
				slot.LightClusterConstantsBuffer, slot.LightClusterBuffer, Pipelines->WhiteAo, Pipelines->FarDepth, scratch.Normals, neutral);
			auto forwardSet = ViewBindings.GetOrCreate(*Device, forwardDesc, *Pipelines->Layouts.ForwardView);
			if (forwardSet)
			{
				RecordMeshPass(commandList, Detail::SceneRendererMeshPass::ForwardOpaque, slot.OpaqueDraws, **forwardSet, frame.DebugView, counters, &forwardTarget);
				if (!slot.TerrainResources.Draws.empty())
					Pipelines->Terrain->RecordForward(commandList, slot.TerrainResources, *slot.ForwardFramebuffer, **forwardSet, counters, slot.Scissor);
				// M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3): the Sky in the skybox's place, with the slot's
				// scissor and fog, sampling the main view's sky-view LUT (world directions).
				const bool fogsClearSky = !frame.DrawsSkybox && !frame.DrawsSky && slot.Fog.Enabled != 0 && slot.Fog.FogSkybox != 0;
				if (frame.DrawsSky)
				{
					const Status sky = RecordSky(commandList, *slot.ForwardFramebuffer, *slot.ViewConstantsBuffer, *slot.FogConstantsBuffer, slot.Scissor);
					if (sky)
					{
						++counters.DrawCalls;
						++counters.Triangles;
					}
					frame.KeepFirstError(sky);
				}
				else if (frame.DrawsSkybox || fogsClearSky)
				{
					const Status sky = Pipelines->Skybox->Record(commandList, SkyboxBindings,
						{ .Framebuffer = slot.ForwardFramebuffer,
							.ViewConstants = slot.ViewConstantsBuffer,
							.EnvironmentConstants = EnvironmentConstantsBuffer,
							.FogConstants = slot.FogConstantsBuffer,
							.Skybox = frame.DrawsSkybox ? frame.EnvironmentSkybox.Get() : Pipelines->BlackCube.Get(),
							.Scissor = slot.Scissor });
					if (sky)
					{
						++counters.DrawCalls;
						++counters.Triangles;
					}
					frame.KeepFirstError(sky);
				}
				// The forward set already binds the neutral occlusion and depth, which the transparent pass needs.
				RecordMeshPass(commandList, Detail::SceneRendererMeshPass::ForwardTransparent, slot.TransparentDraws, **forwardSet, frame.DebugView, counters,
					&forwardTarget);
			}
			else
				frame.KeepFirstError(std::unexpected(std::move(forwardSet).error()));
			if (!snapshot.Particles.empty() && Pipelines->Particles != nullptr)
			{
				ResolveParticleFlipbooks(snapshot.ParticleEmitters);
				frame.KeepFirstError(PlanParticlePass(snapshot.ParticleEmitters, snapshot.Particles, ParticleFlipbooks, slot.Camera.View,
					slot.Camera.Projection * slot.Camera.View, slot.ParticlePlan));
				if (!slot.ParticlePlan.Quads.empty())
				{
					const Result<uint32_t> particles = Pipelines->Particles->Record(commandList, ParticleBindings,
						{ .Plan = &slot.ParticlePlan,
							.Resources = Cache,
							.ColorFramebuffer = slot.ParticleFramebuffer,
							.VelocityFramebuffer = nullptr,
							.ViewConstants = slot.ViewConstantsBuffer,
							.Fog = slot.FogConstantsBuffer,
							.SceneDepth = scratch.Depth,
							.Sampler = frame.PixelArt ? QuadSampler::Point : QuadSampler::Linear,
							.Scissor = slot.Scissor },
						counters);
					if (!particles)
						frame.KeepFirstError(std::unexpected(particles.error()));
				}
			}
			frame.KeepFirstError(Pipelines->PlanarReflections->Record(commandList, ReflectionBindings, *slot.Color, counters));
			recording.EndPass(counters);
		}
	}

	void SceneRenderer::State::ReleaseUnusedReflections()
	{
		for (Detail::SceneRendererReflectionSlot& slot : ReflectionSlots)
		{
			if (!slot.Drawn)
			{
				slot = Detail::SceneRendererReflectionSlot{};
				continue;
			}
			TerrainPass::ReleaseUnused(slot.TerrainResources);
			// An outsized render keeps no memory after it (as the main view's particle plan).
			if (slot.ParticlePlan.Quads.capacity() > ParticlePass::MaxQuadsPerBatch)
				std::vector<ParticlePassQuad>().swap(slot.ParticlePlan.Quads);
		}
		std::erase_if(ReflectionScratch, [](const Detail::SceneRendererReflectionScratch& scratch)
		{
			return !scratch.Used;
		});
	}

}
