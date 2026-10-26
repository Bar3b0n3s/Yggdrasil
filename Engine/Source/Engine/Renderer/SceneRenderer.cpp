#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/CommandListRecording.h"
#include "Engine/Graphics/GpuOutOfMemory.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/BloomPass.h"
#include "Engine/Renderer/BrdfLut.h"
#include "Engine/Renderer/DebugRenderer.h"
#include "Engine/Renderer/EditorOverlay.h"
#include "Engine/Renderer/FxaaPass.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/Private/ForwardPipelines.h"
#include "Engine/Renderer/Private/LightingInputs.h"
#include "Engine/Renderer/Private/MaterialBindingCache.h"
#include "Engine/Renderer/Private/SceneRendererState.h"
#include "Engine/Renderer/RenderPassDeclaration.h"
#include "Engine/Renderer/RenderPrepare.h"
#include "Engine/Renderer/RenderStage.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Engine/Renderer/SkyboxPass.h"
#include "Engine/Renderer/TextRenderer.h"
#include "Engine/Renderer/TonemapPass.h"
#include "Shared/DrawConstants.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/Private/SceneDataViewConstants.h"
#include "Shared/ShaderLight.h"
#include "Shared/ShadowConstants.h"
#include "Shared/ViewConstants.h"

#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The pipelines (SceneRendererPipelines): the Scene program's mesh variants (Renderer/Private/ForwardPipelines.h), with
// the shared binding layouts every variant binds its view and material sets through, the samplers of set 0, the black
// cube a view without an environment map binds as EnvSpecular, and the passes the set owns (SceneRenderer.h), all created
// by CreatePipelines into a state of their own that replaces the current one only when every creation succeeded.
//
// Per renderer (one view): the targets (SceneDepth, SceneNormals, SceneColor, the two LDR targets, the bloom chain) and
// their framebuffers, rebuilt by Resize; the ViewConstants and EnvironmentConstants buffers and the Lights structured
// buffer, written by every Render through the command list (keepInitialState buffers, so NVRHI orders each write after
// the reads of earlier submissions); one PassBindingCache for the view's own set-0 sets and one per shared pass, each
// keeping exactly what the last Render used, and the material sets (Private/MaterialBindingCache.h), which also keep the
// sets of the materials the last Render culled (R-MATSET). A Render with a new generation of the pipeline set clears them
// all first.
//
// Render follows SceneRenderer.h's pass list as stage functions (Docs/Decisions/0029-m18-contract.md decision 1), one per
// RenderStage, handing over through Detail::SceneRendererFrame. The Prepare stage resolves every draw before recording:
// its mesh and material mirrors (GpuResourceCache), its pipeline variant (the material's alpha mode and double-sidedness,
// the world matrix's mirroring) and its material set, then sorts the opaque draws (Opaque and Mask) by (forward variant,
// material, mesh, snapshot order) and the transparent ones (Blend) back to front; the prepass draws the opaque ones in the
// same order with its own variants. Render records the per-view constants and the target clears between the Prepare and
// Deform stages, then the GPU stages.

namespace Engine {

	// The startup count is the sum of the pipelines each part of the set declares (SceneRenderer.h).
	static_assert(SceneRendererPipelines::StartupPipelineCount
		== SceneRendererPipelines::MeshPipelineCount + SkyboxPass::PipelineCount + BloomPass::PipelineCount + TonemapPass::PipelineCount
			+ FxaaPass::PipelineCount + DebugRenderer::PipelineCount + TextRenderer::PipelineCount + BrdfLut::PipelineCount
			+ ShadowPass::PipelineCount + DepthPyramidPass::PipelineCount + GtaoPass::PipelineCount + SelectionPass::PipelineCount + 1
			+ QuadBatcher::PipelineCount + UpscalePass::PipelineCount);
	static_assert(SceneRendererPipelines::MeshPipelineCount == 2 * Utils::PrepassVariantCount + Utils::ForwardVariantCount + Utils::MeshCullModeCount);
	static_assert(SceneRendererPipelines::DebugViewPipelineCount == Utils::ForwardVariantCount);
	// The Scene program's specialization constant takes RenderDebugView's values (Shared/DrawConstants.h).
	static_assert(static_cast<uint32_t>(RenderDebugView::Lit) == SceneDebugViewLit && static_cast<uint32_t>(RenderDebugView::Albedo) == SceneDebugViewAlbedo
		&& static_cast<uint32_t>(RenderDebugView::Normals) == SceneDebugViewNormals
		&& static_cast<uint32_t>(RenderDebugView::Roughness) == SceneDebugViewRoughness
		&& static_cast<uint32_t>(RenderDebugView::Metallic) == SceneDebugViewMetallic
		&& static_cast<uint32_t>(RenderDebugView::Emissive) == SceneDebugViewEmissive);

	namespace Utils {

		// The largest anisotropy of AnisoWrap (Vulkan guarantees at least 16 where samplerAnisotropy exists, §8.1).
		constexpr float MaxSamplerAnisotropy = 16.0f;

		static bool IsFinite(const glm::mat4& matrix)
		{
			for (glm::length_t column = 0; column < 4; ++column)
			{
				for (glm::length_t row = 0; row < 4; ++row)
				{
					if (!std::isfinite(matrix[column][row]))
						return false;
				}
			}
			return true;
		}

		static bool IsFinite(const glm::vec3& vector)
		{
			return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
		}

		// How a target is written.
		enum class TargetUsage : uint8_t
		{
			Attachment, // a colour or depth attachment
			LdrColor    // a storage image of the compute post passes, and the overlays' colour attachment
		};

		// One of the renderer's targets, sampled by later passes and copied by Readback, kept between command lists in its
		// writing state (RenderTarget, DepthWrite) or, for an LDR target, in ShaderResource, the state BlitPass samples it in.
		static Result<nvrhi::TextureHandle> CreateTarget(RenderTargetPool& pool, uint32_t width, uint32_t height, nvrhi::Format format,
			TargetUsage usage, std::string_view name)
		{
			const bool isDepth = format == SceneDepthFormat;
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = format;
			desc.dimension = nvrhi::TextureDimension::Texture2D;
			desc.isShaderResource = true;
			desc.isRenderTarget = true;
			desc.isUAV = usage == TargetUsage::LdrColor;
			if (usage == TargetUsage::LdrColor)
				desc.initialState = nvrhi::ResourceStates::ShaderResource;
			else
				desc.initialState = isDepth ? nvrhi::ResourceStates::DepthWrite : nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			desc.debugName = std::format("SceneRenderer.{}", name);
			return pool.Acquire(desc);
		}

		// The ViewConstants of `camera` for a `width` x `height` target (§8.3, Shared/ViewConstants.h).
		static ViewConstants MakeViewConstants(const CameraData& camera, uint32_t width, uint32_t height, float exposure)
		{
			const glm::vec2 viewportSize(static_cast<float>(width), static_cast<float>(height));
			ViewConstants view{};
			view.View = camera.View;
			view.Projection = camera.Projection;
			view.ViewProjection = camera.Projection * camera.View;
			view.InverseProjection = glm::inverse(camera.Projection);
			view.InverseViewProjection = glm::inverse(view.ViewProjection);
			view.CameraPosition = camera.Position;
			view.Near = camera.NearClip;
			view.ViewportSize = viewportSize;
			view.InverseViewportSize = 1.0f / viewportSize;
			view.AspectRatio = viewportSize.x / viewportSize.y;
			view.Exposure = exposure;
			if (camera.ProjectionKind == RenderProjection::Orthographic)
			{
				view.ProjectionKind = ProjectionKindOrthographic;
				view.OrthoHalfExtents = glm::vec2(1.0f / camera.Projection[0][0], 1.0f / camera.Projection[1][1]);
				view.Far = camera.FarClip;
				view.TanHalfFovY = 0.0f;
			}
			else
			{
				view.ProjectionKind = ProjectionKindPerspective;
				view.OrthoHalfExtents = glm::vec2(0.0f);
				view.Far = 0.0f;
				view.TanHalfFovY = 1.0f / camera.Projection[1][1];
			}
			return view;
		}

		// 2^ExposureEV (§8.9); 1 for a value that is not finite.
		static float GetExposure(const PostProcessSettings& post)
		{
			if (!std::isfinite(post.ExposureEV))
				return 1.0f;
			const float exposure = std::exp2(post.ExposureEV);
			return std::isfinite(exposure) ? exposure : 1.0f;
		}

		// The material slot `slot` of `item` drawing a submesh of a mesh with `defaults`: the item's slot when it is set,
		// else the mesh's default material, else the built-in Default material.
		static AssetHandle SelectMaterial(const MeshDrawItem& item, std::span<const AssetHandle> defaults, uint32_t slot)
		{
			if (slot < item.Materials.size() && item.Materials[slot].IsValid())
				return item.Materials[slot];
			if (slot < defaults.size() && defaults[slot].IsValid())
				return defaults[slot];
			return BuiltinAssetHandles::DefaultMaterial;
		}

		// A 1x1 RGBA16_FLOAT cube of zeros: EnvSpecular of a view without an environment map, so the forward set always binds a
		// cube (the shader reads it only when EnvironmentConstants::HasEnvironment is set). A startup object of the pass list,
		// not an asset upload: a render target cleared once by one command list executed at once, kept in ShaderResource.
		static Result<nvrhi::TextureHandle> CreateBlackCube(GraphicsDevice& device)
		{
			nvrhi::TextureDesc desc;
			desc.width = 1;
			desc.height = 1;
			desc.arraySize = 6;
			desc.mipLevels = 1;
			desc.format = nvrhi::Format::RGBA16_FLOAT;
			desc.dimension = nvrhi::TextureDimension::TextureCube;
			desc.isShaderResource = true;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = "SceneRenderer.BlackCube";
			ENGINE_TRY_ASSIGN(nvrhi::TextureHandle texture, device.CreateTexture(desc));
			// Not an immediate command list: a caller may have the frame's immediate list open (NVRHI's validation allows one).
			ENGINE_TRY_ASSIGN(const nvrhi::CommandListHandle commandList,
				device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
			CommandListRecording recording(device, *commandList);
			commandList->clearTextureFloat(texture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
			recording.Execute();
			return texture;
		}

		// Shared neutral resources keep the forward descriptor set complete when a pass is disabled.
		static Result<nvrhi::TextureHandle> CreateSceneNeutralTexture(GraphicsDevice& device, nvrhi::Format format, bool array, float value)
		{
			nvrhi::TextureDesc desc;
			desc.width = 1;
			desc.height = 1;
			desc.arraySize = array ? 4 : 1;
			desc.dimension = array ? nvrhi::TextureDimension::Texture2DArray : nvrhi::TextureDimension::Texture2D;
			desc.format = format;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = "SceneRenderer.Neutral";
			ENGINE_TRY_ASSIGN(nvrhi::TextureHandle texture, device.CreateTexture(desc));
			ENGINE_TRY_ASSIGN(const nvrhi::CommandListHandle list, device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
			CommandListRecording recording(device, *list);
			if (format == ShadowDepthFormat)
				list->clearDepthStencilTexture(texture, nvrhi::AllSubresources, true, value, false, 0);
			else
				list->clearTextureFloat(texture, nvrhi::AllSubresources, nvrhi::Color(value));
			recording.Execute();
			return texture;
		}

		static double SceneRendererNowSeconds()
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		// Appends `descriptions` to `all`.
		static void AppendDescriptions(std::vector<PipelineLayoutDescription>& all, std::vector<PipelineLayoutDescription> descriptions)
		{
			all.insert(all.end(), std::make_move_iterator(descriptions.begin()), std::make_move_iterator(descriptions.end()));
		}

		// The position of stats row `name` in RenderStatsOrder (RenderPassDeclaration.h); past the end for a row no pass
		// declares.
		static size_t GetStatsPosition(std::string_view name)
		{
			return static_cast<size_t>(std::find(RenderStatsOrder.begin(), RenderStatsOrder.end(), name) - RenderStatsOrder.begin());
		}

	}

	SceneRendererPipelines::SceneRendererPipelines(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	SceneRendererPipelines::~SceneRendererPipelines() = default;

	Result<Scope<SceneRendererPipelines>> SceneRendererPipelines::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<SceneRendererPipelines> set = CreateScope<SceneRendererPipelines>(ConstructionKey());
		State& state = *set->m_State;
		state.Device = &device;
		state.Factory = &pipelines;
		ENGINE_TRY(set->CreatePipelines());
		// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decisions 26 and 27): released with its passes, created again
		// through the one creation entry.
		SceneRendererPipelines* owner = set.get();
		GpuRecreationHooks hooks;
		hooks.Name = "SceneRendererPipelines";
		hooks.Release = [owner]()
		{
			owner->ReleaseGpuObjects();
		};
		hooks.Recreate = [owner]()
		{
			return owner->CreatePipelines();
		};
		state.Recreation = device.GetRecreation().Register(std::move(hooks));
		return set;
	}

	Status SceneRendererPipelines::EnsureDebugView(RenderDebugView view)
	{
		State& state = *m_State;
		// Between the set's Release and its CreatePipelines (a device loss's recovery) the set has no binding layouts to
		// create a view's pipelines with (SceneRenderer.h, ReleaseGpuObjects).
		ENGINE_CORE_ASSERT(state.PipelineCount > 0, "EnsureDebugView on a pipeline set whose GPU objects were released by a device loss");
		if (state.PipelineCount == 0)
			return MakeError(ErrorCode::InvalidState, "the pipeline set's GPU objects were released by a device loss and not created again");
		const auto index = static_cast<size_t>(view);
		if (index >= RenderDebugViewCount)
			return MakeError(ErrorCode::InvalidArgument, "debug view {} has no pipelines", index);
		if (state.HasView[index])
			return {};
		std::array<GraphicsPipeline, Utils::ForwardVariantCount> created{};
		for (uint32_t variant = 0; variant < Utils::ForwardVariantCount; ++variant)
		{
			ENGINE_TRY_ASSIGN(created[variant], Utils::CreateForwardPipeline(*state.Factory, state.Layouts, variant, view));
		}
		state.Forward[index] = std::move(created);
		state.HasView[index] = true;
		state.PipelineCount += DebugViewPipelineCount;
		ENGINE_CORE_INFO("Created the {} debug view's {} pipelines ({} scene renderer pipelines in all)", RenderDebugViewToString(view),
			DebugViewPipelineCount, state.PipelineCount);
		return {};
	}

	uint32_t SceneRendererPipelines::GetPipelineCount() const
	{
		return m_State->PipelineCount;
	}

	Status SceneRendererPipelines::CreatePipelines()
	{
		State& state = *m_State;
		GraphicsDevice& device = *state.Device;
		PipelineFactory& pipelines = *state.Factory;
		// Every pass is created aside, into a state of its own, so an error leaves the current passes in use.
		State created;
		created.Device = state.Device;
		created.Factory = state.Factory;
		ENGINE_TRY_ASSIGN(created.Layouts, Utils::CreateMeshBindingLayouts(pipelines));
		for (uint32_t variant = 0; variant < Utils::PrepassVariantCount; ++variant)
		{
			ENGINE_TRY_ASSIGN(created.Prepass[variant], Utils::CreatePrepassPipeline(pipelines, created.Layouts, variant));
			ENGINE_TRY_ASSIGN(created.Picking[variant], Utils::CreatePrepassPipeline(pipelines, created.Layouts, variant, true));
		}
		const auto lit = static_cast<size_t>(RenderDebugView::Lit);
		for (uint32_t variant = 0; variant < Utils::ForwardVariantCount; ++variant)
		{
			ENGINE_TRY_ASSIGN(created.Forward[lit][variant], Utils::CreateForwardPipeline(pipelines, created.Layouts, variant, RenderDebugView::Lit));
		}
		created.HasView[lit] = true;
		for (uint32_t variant = 0; variant < Utils::MeshCullModeCount; ++variant)
		{
			ENGINE_TRY_ASSIGN(created.Overdraw[variant], Utils::CreateOverdrawPipeline(pipelines, created.Layouts, variant));
		}
		ENGINE_TRY_ASSIGN(created.DataView, pipelines.CreateComputePipeline({ .Layout = Utils::GetSceneDataViewLayout() }));
		for (size_t index = static_cast<size_t>(RenderDebugView::AO); index < RenderDebugViewCount; ++index)
			created.HasView[index] = true;

		ENGINE_TRY_ASSIGN(created.LinearClamp,
			device.CreateSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp)));
		ENGINE_TRY_ASSIGN(created.AnisoWrap, device.CreateSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap).setMaxAnisotropy(Utils::MaxSamplerAnisotropy)));
		ENGINE_TRY_ASSIGN(created.BlackCube, Utils::CreateBlackCube(device));
		ENGINE_TRY_ASSIGN(created.EmptyCascades, Utils::CreateSceneNeutralTexture(device, ShadowDepthFormat, true, 0.0f));
		ENGINE_TRY_ASSIGN(created.EmptyAtlas, Utils::CreateSceneNeutralTexture(device, ShadowDepthFormat, false, 0.0f));
		ENGINE_TRY_ASSIGN(created.WhiteAo, Utils::CreateSceneNeutralTexture(device, AmbientOcclusionFormat, false, 1.0f));
		ENGINE_TRY_ASSIGN(created.FarDepth, Utils::CreateSceneNeutralTexture(device, ViewDepthFormat, false, 65504.0f));
		ENGINE_TRY_ASSIGN(created.ShadowCompare, device.CreateSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp).setReductionType(nvrhi::SamplerReductionType::Comparison)));
		ENGINE_TRY_ASSIGN(created.Shadows, ShadowPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(created.DepthPyramid, DepthPyramidPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(created.Gtao, GtaoPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(created.Selection, SelectionPass::Create(device, pipelines));

		ENGINE_TRY_ASSIGN(created.Skybox, SkyboxPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(created.Bloom, BloomPass::Create(device, pipelines, device.GetInfo().BloomFormat));
		ENGINE_TRY_ASSIGN(created.Tonemap, TonemapPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(created.Fxaa, FxaaPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(created.Debug, DebugRenderer::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(created.Text, TextRenderer::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(created.DfgLut, BrdfLut::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(created.Quads, QuadBatcher::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(created.Upscale, UpscalePass::Create(device, pipelines));
		created.PipelineCount = MeshPipelineCount + created.Skybox->GetPipelineCount() + created.Bloom->GetPipelineCount()
			+ created.Tonemap->GetPipelineCount() + created.Fxaa->GetPipelineCount() + created.Debug->GetPipelineCount()
			+ created.Text->GetPipelineCount() + created.DfgLut->GetPipelineCount() + ShadowPass::PipelineCount + DepthPyramidPass::PipelineCount
			+ GtaoPass::PipelineCount + SelectionPass::PipelineCount + 1 + created.Quads->GetPipelineCount() + created.Upscale->GetPipelineCount();

		// Every creation succeeded: the new passes replace the current ones, and the set keeps its back-references and its
		// registration (moving an empty registration over another calls nothing on the registry, so this also runs inside a
		// Recreate hook). The lazily created debug-view pipelines are dropped; EnsureDebugView creates them again on first use.
		// Command lists that recorded the replaced pipelines keep them alive until they retire (§8.14 item 2).
		created.Generation = state.Generation + 1;
		created.Recreation = std::move(state.Recreation);
		state = std::move(created);
		ENGINE_CORE_INFO("Created the scene renderer's {} pipelines", state.PipelineCount);
		return {};
	}

	uint64_t SceneRendererPipelines::GetGeneration() const
	{
		return m_State->Generation;
	}

	std::vector<PipelineLayoutDescription> SceneRendererPipelines::GetLayoutDescriptions(nvrhi::Format bloomFormat)
	{
		std::vector<PipelineLayoutDescription> descriptions = Utils::GetMeshLayoutDescriptions();
		Utils::AppendDescriptions(descriptions, SkyboxPass::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, BloomPass::GetLayoutDescriptions(bloomFormat));
		Utils::AppendDescriptions(descriptions, TonemapPass::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, FxaaPass::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, DebugRenderer::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, TextRenderer::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, BrdfLut::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, ShadowPass::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, DepthPyramidPass::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, GtaoPass::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, SelectionPass::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, QuadBatcher::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, UpscalePass::GetLayoutDescriptions());
		descriptions.push_back(Utils::GetSceneDataViewLayout());
		return descriptions;
	}

	void SceneRendererPipelines::CollectStale(const AssetManager& assets, bool releaseUnused)
	{
		m_State->Text->CollectStale(assets, releaseUnused);
	}

	Result<Detail::SceneRendererTargets> SceneRenderer::State::CreateTargets(uint32_t width, uint32_t height) const
	{
		GraphicsDevice& device = *Device;
		Detail::SceneRendererTargets targets;
		targets.Width = width;
		targets.Height = height;
		ENGINE_TRY_ASSIGN(targets.SceneDepth,
			Utils::CreateTarget(*TargetPool, width, height, SceneDepthFormat, Utils::TargetUsage::Attachment, "SceneDepth"));
		ENGINE_TRY_ASSIGN(targets.SceneNormals,
			Utils::CreateTarget(*TargetPool, width, height, SceneNormalsFormat, Utils::TargetUsage::Attachment, "SceneNormals"));
		ENGINE_TRY_ASSIGN(targets.SceneColor,
			Utils::CreateTarget(*TargetPool, width, height, SceneColorFormat, Utils::TargetUsage::Attachment, "SceneColor"));
		ENGINE_TRY_ASSIGN(targets.Ldr[0], Utils::CreateTarget(*TargetPool, width, height, LdrColorFormat, Utils::TargetUsage::LdrColor, "LdrColor"));
		ENGINE_TRY_ASSIGN(targets.Ldr[1], Utils::CreateTarget(*TargetPool, width, height, LdrColorFormat, Utils::TargetUsage::LdrColor, "LdrColorFxaa"));
		ENGINE_TRY_ASSIGN(targets.Bloom, TargetPool->Acquire(BloomPass::GetChainDesc(width, height, Pipelines->Bloom->GetFormat())));

		ENGINE_TRY_ASSIGN(targets.PrepassFramebuffer,
			device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(targets.SceneNormals).setDepthAttachment(targets.SceneDepth)));
		ENGINE_TRY_ASSIGN(targets.ForwardFramebuffer,
			device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(targets.SceneColor).setDepthAttachment(targets.SceneDepth)));
		// The overlays test against SceneDepth without writing it (their pipelines disable depth writes). The attachment is
		// not marked read-only: NVRHI stores a read-only depth attachment with STORE_OP_STORE, a depth write the read-only
		// state's barriers do not cover (synchronization validation reports the next layout transition), and keeping
		// SceneDepth in DepthWrite from the prepass to the overlays needs no transition at all.
		for (size_t index = 0; index < targets.Ldr.size(); ++index)
		{
			ENGINE_TRY_ASSIGN(targets.OverlayFramebuffers[index],
				device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(targets.Ldr[index]).setDepthAttachment(targets.SceneDepth)));
		}
		return targets;
	}

	Status SceneRenderer::State::EnsureFrameTargets(bool picking, bool ao, bool halfAo, uint32_t shadowSize, bool spots, bool selection, bool overdraw)
	{
		const uint32_t width = Targets.Width;
		const uint32_t height = Targets.Height;
		if (picking && Targets.EntityId == nullptr)
		{
			ENGINE_TRY_ASSIGN(Targets.EntityId, Utils::CreateTarget(*TargetPool, width, height, EntityIdFormat, Utils::TargetUsage::Attachment, "EntityId"));
		}
		if (picking && Targets.PickingFramebuffer == nullptr)
		{
			ENGINE_TRY_ASSIGN(Targets.PickingFramebuffer, Device->CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(Targets.SceneNormals).addColorAttachment(Targets.EntityId).setDepthAttachment(Targets.SceneDepth)));
		}
		if (ao)
		{
			if (Targets.ViewDepth == nullptr)
			{
				ENGINE_TRY_ASSIGN(Targets.ViewDepth, TargetPool->Acquire(DepthPyramidPass::GetTargetDesc(width, height)));
			}
			const auto desc = GtaoPass::GetTargetDesc(width, height, halfAo);
			if (Targets.Occlusion == nullptr || Targets.Occlusion->getDesc().width != desc.width || Targets.Occlusion->getDesc().height != desc.height)
			{
				ClearTargetBindings();
				ENGINE_TRY_ASSIGN(nvrhi::TextureHandle occlusion, TargetPool->Acquire(desc));
				ENGINE_TRY_ASSIGN(nvrhi::TextureHandle scratch, TargetPool->Acquire(desc));
				Targets.Occlusion = std::move(occlusion);
				Targets.AoScratch = std::move(scratch);
			}
		}
		if (shadowSize != 0 && (Targets.Cascades == nullptr || Targets.Cascades->getDesc().width != shadowSize))
		{
			ClearTargetBindings();
			ENGINE_TRY_ASSIGN(nvrhi::TextureHandle cascades, TargetPool->Acquire(ShadowPass::GetCascadeTargetDesc(shadowSize)));
			ENGINE_TRY_ASSIGN(std::vector<nvrhi::FramebufferHandle> framebuffers, ShadowPass::CreateFramebuffers(*Device, *cascades));
			Targets.Cascades = std::move(cascades);
			Targets.CascadeFramebuffers = std::move(framebuffers);
		}
		if (spots && Targets.SpotAtlas == nullptr)
		{
			ENGINE_TRY_ASSIGN(nvrhi::TextureHandle atlas, TargetPool->Acquire(ShadowPass::GetAtlasTargetDesc()));
			ENGINE_TRY_ASSIGN(std::vector<nvrhi::FramebufferHandle> framebuffers, ShadowPass::CreateFramebuffers(*Device, *atlas));
			Targets.SpotAtlas = std::move(atlas);
			Targets.SpotAtlasFramebuffer = std::move(framebuffers.front());
		}
		if (selection && Targets.SelectionMask == nullptr)
		{
			ENGINE_TRY_ASSIGN(nvrhi::TextureHandle mask, TargetPool->Acquire(SelectionPass::GetMaskDesc(width, height)));
			ENGINE_TRY_ASSIGN(nvrhi::TextureHandle scratch, TargetPool->Acquire(SelectionPass::GetMaskDesc(width, height)));
			Targets.SelectionMask = std::move(mask);
			Targets.SelectionScratch = std::move(scratch);
		}
		if (overdraw && Targets.Overdraw == nullptr)
		{
			ENGINE_TRY_ASSIGN(Targets.Overdraw, Utils::CreateTarget(*TargetPool, width, height, nvrhi::Format::RGBA16_FLOAT, Utils::TargetUsage::Attachment, "Overdraw"));
		}
		if (overdraw && Targets.OverdrawFramebuffer == nullptr)
		{
			ENGINE_TRY_ASSIGN(Targets.OverdrawFramebuffer, Device->CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(Targets.Overdraw)));
		}
		return {};
	}

	void SceneRenderer::State::ClearTargetBindings()
	{
		ViewBindings.Clear();
		SkyboxBindings.Clear();
		BloomBindings.Clear();
		TonemapBindings.Clear();
		FxaaBindings.Clear();
		DebugBindings.Clear();
		TextBindings.Clear();
		ShadowBindings.Clear();
		DepthBindings.Clear();
		GtaoBindings.Clear();
		SelectionBindings.Clear();
		DataViewBindings.Clear();
	}

	void SceneRenderer::State::ClearBindings()
	{
		ClearTargetBindings();
		UpscaleBindings.Clear();
		Materials.Clear();
	}

	void SceneRenderer::State::ReleaseUnusedBindings()
	{
		ViewBindings.ReleaseUnused();
		SkyboxBindings.ReleaseUnused();
		BloomBindings.ReleaseUnused();
		TonemapBindings.ReleaseUnused();
		FxaaBindings.ReleaseUnused();
		DebugBindings.ReleaseUnused();
		TextBindings.ReleaseUnused();
		ShadowBindings.ReleaseUnused();
		DepthBindings.ReleaseUnused();
		GtaoBindings.ReleaseUnused();
		SelectionBindings.ReleaseUnused();
		DataViewBindings.ReleaseUnused();
		UpscaleBindings.ReleaseUnused();

		Materials.ReleaseUnused();
	}

	Result<Detail::SceneRendererMaterial> SceneRenderer::State::ResolveMaterial(AssetHandle handle)
	{
		// The mirror is valid until the next upload: everything needed is taken now.
		const GpuMaterial& material = Cache->GetMaterial(handle);
		if (material.Constants == nullptr || material.BaseColorMap == nullptr || material.MetallicRoughnessMap == nullptr || material.NormalMap == nullptr
			|| material.OcclusionMap == nullptr || material.EmissiveMap == nullptr)
			return Detail::SceneRendererMaterial{}; // not even a placeholder could be created (reported by the cache)
		Result<nvrhi::IBindingSet*> set = Materials.GetOrCreate(*Device, handle, material, *Pipelines->Layouts.Material);
		if (!set.has_value())
			return std::unexpected(std::move(set).error().WithContext(std::format("while binding material {}", handle)));
		return Detail::SceneRendererMaterial{ .Set = *set, .Mode = material.AlphaMode, .DoubleSided = material.DoubleSided };
	}

	Status SceneRenderer::State::PrepareDraws(const RenderSnapshot& snapshot, const CameraData& camera)
	{
		OpaqueDraws.clear();
		TransparentDraws.clear();
		Status status;
		std::map<AssetHandle, Detail::SceneRendererMaterial> materials;
		std::set<AssetHandle> culledMaterials; // the materials of culled submeshes this render looked at
		std::vector<GpuSubmesh> submeshes;
		std::vector<AssetHandle> defaults;
		for (size_t index = 0; index < snapshot.Meshes.size(); ++index)
		{
			const MeshDrawItem& item = snapshot.Meshes[index];
			if (!item.Mesh.IsValid())
				continue; // a null handle draws nothing
			if (!Utils::IsFinite(item.World))
			{
				if (status.has_value())
				{
					status = MakeError(ErrorCode::InvalidArgument, "the world matrix of entity {} in the render snapshot is not finite; its draw is skipped",
						item.Entity);
				}
				continue;
			}

			// The mirror is valid until the next upload (resolving materials uploads textures): what is needed is copied now.
			const GpuMesh& mesh = Cache->GetMesh(item.Mesh);
			if (mesh.VertexBuffer == nullptr || mesh.IndexBuffer == nullptr)
				continue; // not even the placeholder could be created (reported by the cache)
			nvrhi::IBuffer* vertexBuffer = mesh.VertexBuffer.Get();
			nvrhi::IBuffer* indexBuffer = mesh.IndexBuffer.Get();
			submeshes.assign(mesh.Submeshes.begin(), mesh.Submeshes.end());
			defaults.assign(mesh.DefaultMaterials.begin(), mesh.DefaultMaterials.end());

			const bool mirrored = glm::determinant(glm::mat3(item.World)) < 0.0f;
			const glm::mat4 worldView = camera.View * item.World;
			for (uint32_t submeshIndex = 0; submeshIndex < submeshes.size(); ++submeshIndex)
			{
				const GpuSubmesh& submesh = submeshes[submeshIndex];
				if (submesh.IndexCount == 0)
					continue;
				const AssetHandle materialHandle = Utils::SelectMaterial(item, defaults, submesh.MaterialSlot);
				if (IsOutsideView(submesh.Bounds, item.World, camera))
				{
					++Stats.CulledSubmeshes;
					// R-MATSET (M18): a material whose draws left the view keeps the set it has while it stays in the snapshot,
					// so it makes no new set when it returns. Only an existing set is kept, and only for the mirror's current
					// generation (its lookup rebuilds a replaced version, as the material's return would): a material that no
					// render drew gets no mirror or set here.
					if (!materials.contains(materialHandle) && Materials.Contains(materialHandle) && culledMaterials.insert(materialHandle).second)
						Materials.Keep(materialHandle, Cache->GetMaterial(materialHandle).Generation);
					continue;
				}
				auto found = materials.find(materialHandle);
				if (found == materials.end())
				{
					// A material whose set cannot be created skips its draws; the first such error is the render's.
					Result<Detail::SceneRendererMaterial> resolved = ResolveMaterial(materialHandle);
					if (!resolved.has_value() && status.has_value())
						status = std::unexpected(std::move(resolved).error());
					found = materials.emplace(materialHandle, resolved.value_or(Detail::SceneRendererMaterial{})).first;
				}
				const Detail::SceneRendererMaterial& material = found->second;
				if (material.Set == nullptr)
					continue; // its GPU objects are missing (reported)

				const Utils::MeshCullMode cullMode = Utils::SelectCullMode(mirrored, material.DoubleSided);
				Detail::SceneRendererDraw draw;
				draw.VertexBuffer = vertexBuffer;
				draw.IndexBuffer = indexBuffer;
				draw.IndexOffset = submesh.IndexOffset;
				draw.IndexCount = submesh.IndexCount;
				draw.Material = material.Set;
				draw.ForwardVariant = Utils::GetForwardVariant(material.Mode, cullMode);
				draw.Constants.World = item.World;
				draw.Constants.EntityId = item.PickId;
				draw.Constants.Flags = (mirrored ? DrawFlagMirrored : 0U) | (item.ReceiveShadows ? DrawFlagReceiveShadows : 0U);
				draw.MaterialHandle = materialHandle;
				draw.Mesh = item.Mesh;
				draw.Entity = item.Entity;
				draw.Submesh = submeshIndex;
				draw.SnapshotIndex = index;
				if (material.Mode == AlphaMode::Blend)
				{
					const glm::vec3 centre = submesh.Bounds.IsEmpty() ? glm::vec3(0.0f) : (submesh.Bounds.Min + submesh.Bounds.Max) * 0.5f;
					draw.ViewDepth = -(worldView * glm::vec4(centre, 1.0f)).z;
					TransparentDraws.push_back(draw);
				}
				else
				{
					draw.PrepassVariant = Utils::GetPrepassVariant(material.Mode, cullMode);
					OpaqueDraws.push_back(draw);
				}
			}
		}

		// §8.3 pass 1: opaque draws by pipeline, then material, then mesh, ties in snapshot order; transparent draws back to
		// front, ties by entity UUID (RenderPrepare.h).
		std::vector<OpaqueSortKey> opaqueKeys;
		opaqueKeys.reserve(OpaqueDraws.size());
		for (uint32_t index = 0; index < OpaqueDraws.size(); ++index)
		{
			const Detail::SceneRendererDraw& draw = OpaqueDraws[index];
			opaqueKeys.push_back({ .Pipeline = draw.ForwardVariant, .Material = draw.MaterialHandle, .Mesh = draw.Mesh, .DrawIndex = index });
		}
		SortOpaqueDraws(opaqueKeys);
		std::vector<TransparentSortKey> transparentKeys;
		transparentKeys.reserve(TransparentDraws.size());
		for (uint32_t index = 0; index < TransparentDraws.size(); ++index)
		{
			const Detail::SceneRendererDraw& draw = TransparentDraws[index];
			transparentKeys.push_back({ .ViewDepth = draw.ViewDepth, .Entity = draw.Entity, .Submesh = draw.Submesh, .DrawIndex = index });
		}
		SortTransparentDraws(transparentKeys);

		const auto reorder = []<typename Key>(std::vector<Detail::SceneRendererDraw>& draws, const std::vector<Key>& keys)
		{
			std::vector<Detail::SceneRendererDraw> sorted;
			sorted.reserve(draws.size());
			for (const Key& key : keys)
				sorted.push_back(draws[key.DrawIndex]);
			draws = std::move(sorted);
		};
		reorder(OpaqueDraws, opaqueKeys);
		reorder(TransparentDraws, transparentKeys);
		return status;
	}

	void SceneRenderer::State::RecordMeshPass(nvrhi::ICommandList& commandList, Detail::SceneRendererMeshPass pass, std::span<const Detail::SceneRendererDraw> draws, nvrhi::IBindingSet& viewSet,
		RenderDebugView view, RenderPassCounters& counters)
	{
		const bool prepass = pass == Detail::SceneRendererMeshPass::Prepass || pass == Detail::SceneRendererMeshPass::Picking;
		nvrhi::IFramebuffer* framebuffer = pass == Detail::SceneRendererMeshPass::Prepass ? Targets.PrepassFramebuffer.Get() : Targets.ForwardFramebuffer.Get();
		if (pass == Detail::SceneRendererMeshPass::Picking)
			framebuffer = Targets.PickingFramebuffer;
		if (pass == Detail::SceneRendererMeshPass::Overdraw)
			framebuffer = Targets.OverdrawFramebuffer;
		const nvrhi::Viewport viewport(static_cast<float>(Targets.Width), static_cast<float>(Targets.Height));
		const auto& forward = Pipelines->Forward[static_cast<size_t>(view)];

		// The state of the last draw: setGraphicsState only when it changes (it also invalidates the push constants, which
		// every draw sets).
		const nvrhi::IGraphicsPipeline* lastPipeline = nullptr;
		const nvrhi::IBindingSet* lastMaterial = nullptr;
		const nvrhi::IBuffer* lastVertexBuffer = nullptr;
		for (const Detail::SceneRendererDraw& draw : draws)
		{
			const GraphicsPipeline& pipeline = prepass
				? (pass == Detail::SceneRendererMeshPass::Picking ? Pipelines->Picking[draw.PrepassVariant] : Pipelines->Prepass[draw.PrepassVariant])
				: (pass == Detail::SceneRendererMeshPass::Overdraw ? Pipelines->Overdraw[draw.ForwardVariant % Utils::MeshCullModeCount] : forward[draw.ForwardVariant]);
			if (pipeline.Pipeline.Get() != lastPipeline || draw.Material != lastMaterial || draw.VertexBuffer != lastVertexBuffer)
			{
				nvrhi::GraphicsState state;
				state.pipeline = pipeline.Pipeline;
				state.framebuffer = framebuffer;
				state.viewport.addViewportAndScissorRect(viewport);
				state.bindings = { &viewSet, draw.Material };
				state.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(draw.VertexBuffer).setSlot(0).setOffset(0));
				state.setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(draw.IndexBuffer).setFormat(nvrhi::Format::R32_UINT).setOffset(0));
				commandList.setGraphicsState(state);
				lastPipeline = pipeline.Pipeline.Get();
				lastMaterial = draw.Material;
				lastVertexBuffer = draw.VertexBuffer;
			}
			commandList.setPushConstants(&draw.Constants, sizeof(draw.Constants));
			commandList.drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.IndexCount).setStartIndexLocation(draw.IndexOffset));
			++counters.DrawCalls;
			counters.Triangles += draw.IndexCount / 3;
		}
		if (pass == Detail::SceneRendererMeshPass::ForwardOpaque || pass == Detail::SceneRendererMeshPass::ForwardTransparent)
			Stats.MeshDraws += static_cast<uint32_t>(draws.size());
		if (pass == Detail::SceneRendererMeshPass::ForwardTransparent)
			Stats.TransparentDraws = static_cast<uint32_t>(draws.size());
	}

	SceneRenderer::SceneRenderer(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	SceneRenderer::~SceneRenderer() = default;

	Result<Scope<SceneRenderer>> SceneRenderer::Create(GraphicsDevice& device, SceneRendererPipelines& pipelines, GpuResourceCache& cache,
		AssetManager& assets, const SceneRendererSpecification& specification)
	{
		ENGINE_CORE_ASSERT(specification.Width > 0 && specification.Height > 0, "SceneRenderer::Create needs a size of at least 1x1, got {}x{}",
			specification.Width, specification.Height);
		Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(ConstructionKey());
		State& state = *renderer->m_State;
		state.Device = &device;
		state.PipelineSet = &pipelines;
		state.Pipelines = pipelines.m_State.get();
		state.Cache = &cache;
		state.Assets = &assets;

		state.TargetPool = CreateScope<RenderTargetPool>(device);
		state.Picker = CreateScope<AsyncPicker>(device);
		state.Profiler = CreateScope<GpuProfiler>(device, device.GetFramesInFlight());
		state.TimingSubmissions.resize(device.GetFramesInFlight(), 0);
		state.ViewWidth = specification.Width;
		state.ViewHeight = specification.Height;
		// The per-view buffers and the targets of the size, which the device's recovery creates again the same way.
		ENGINE_TRY(renderer->RecreateGpuObjects());
		// M18 (C18.3, decisions 26 and 27): released with its per-view objects, which it creates again.
		SceneRenderer* owner = renderer.get();
		GpuRecreationHooks hooks;
		hooks.Name = "SceneRenderer";
		hooks.Release = [owner]()
		{
			owner->ReleaseGpuObjects();
		};
		hooks.Recreate = [owner]()
		{
			return owner->RecreateGpuObjects();
		};
		// M18 (C18.4, decision 43): its pool's free targets, when a request ran out of GPU memory. Through the renderer's
		// current state, which a device loss's Release replaces.
		hooks.TrimUnused = [owner]()
		{
			owner->m_State->TargetPool->TrimUnused();
		};
		state.Recreation = device.GetRecreation().Register(std::move(hooks));
		return renderer;
	}

	Status SceneRenderer::Resize(uint32_t width, uint32_t height)
	{
		ENGINE_CORE_ASSERT(width > 0 && height > 0, "SceneRenderer::Resize needs a size of at least 1x1, got {}x{}", width, height);
		State& state = *m_State;
		if (width == state.ViewWidth && height == state.ViewHeight)
			return {};
		ENGINE_CORE_ASSERT(!state.PendingSubmission, "Resize requires OnSubmitted for the prior Render");
		if (state.PendingSubmission)
			return MakeError(ErrorCode::InvalidState, "Resize requires OnSubmitted for the prior Render");
		// Built aside first, so a failure keeps the current targets. The previous ones stay alive while a submitted command
		// list still references them (their framebuffers and binding sets are referenced by every list that used them). M18
		// (C18.4, decisions 42 and 44): a new size after startup is a request; an out-of-memory creation is trimmed for and
		// retried once (the targets a failed attempt acquired are free in the pool again, and its retry reuses them), and a
		// second one returns OutOfMemory with the renderer as it was. Nothing records meanwhile: no Render awaits OnSubmitted.
		Detail::SceneRendererTargets targets;
		const Status created = RetryAfterTrim(*state.Device, std::format("a scene view's resize to {}x{}", width, height), [&state, &targets, width, height]() -> Status
		{
			ENGINE_TRY_ASSIGN(targets, state.CreateTargets(width, height));
			return {};
		});
		ENGINE_TRY(created);
		state.Targets = std::move(targets);
		state.ViewWidth = width;
		state.ViewHeight = height;
		state.OutputTargets = {};
		state.UpscaleBindings.Clear();
		state.FinalTarget = 0;
		state.ClearTargetBindings();
		state.TargetPool->ReleaseFree();
		CancelPicks();
		state.RecordedPicking = false;
		return {};
	}

	void SceneRenderer::ReleaseTargets()
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(!state.PendingSubmission, "ReleaseTargets requires OnSubmitted for the prior Render");
		if (state.PendingSubmission)
			return;
		// The size-dependent targets go, with every binding set that references them; the size, the pipelines, the per-view
		// buffers and the material sets stay. A zero-sized Targets makes the next Render's Prepare stage create the targets of
		// its scene resolution again (RecordPrepareStage), and the output stage creates its own on demand.
		state.Targets = {};
		state.OutputTargets = {};
		state.UpscaleBindings.Clear();
		state.FinalTarget = 0;
		state.ClearTargetBindings();
		// What the pool holds that nothing pending uses goes at once: the released targets are free now, and those an
		// EndFrame stamped with a completed submission are released (no wait).
		state.TargetPool->ReleaseFree();
		CancelPicks();
		state.RecordedPicking = false;
	}

	Status SceneRenderer::Render(nvrhi::ICommandList& commandList, const RenderSnapshot& snapshot)
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(!state.PendingSubmission, "Render requires OnSubmitted for the prior Render");
		if (state.PendingSubmission)
			return MakeError(ErrorCode::InvalidState, "Render requires OnSubmitted for the prior Render");
		// M18 (C18.3, decision 27): a render between the set's Release and its CreatePipelines is a programmer error: the
		// device's recovery runs both (SceneRendererPipelines::ReleaseGpuObjects).
		ENGINE_CORE_ASSERT(state.Pipelines->PipelineCount > 0, "Render with a pipeline set whose GPU objects were released by a device loss");
		if (state.Pipelines->PipelineCount == 0)
			return MakeError(ErrorCode::InvalidState, "the renderer's pipeline set was released by a device loss and not created again");
		const double started = Utils::SceneRendererNowSeconds();
		RenderDebugView debugView = snapshot.DebugView;
		if (static_cast<size_t>(debugView) >= RenderDebugViewCount)
			return MakeError(ErrorCode::InvalidArgument, "debug view {} has no pipelines", std::to_underlying(debugView));
		// A debug view's pipelines are created the first time it is asked for, before anything is recorded or uploaded. M18
		// (C18.4, decisions 42 and 44): after startup that is a debug-view switch, a request: an out-of-memory creation is
		// trimmed for and retried once, and when it runs out of memory again the view is recorded in the debug view this
		// renderer last rendered (Lit when that one's pipelines are gone too), and Render returns OutOfMemory as a partial
		// error naming the view that stayed, never FatalError(OutOfMemory). On a lost device a Gpu error is the loss (C18.3,
		// decision 28), returned before recording like any other error (an invalid shader), and the device's recovery at the
		// frame boundary leaves the view to be created on the next render.
		Status debugViewError{};
		if (!state.Pipelines->HasView[static_cast<size_t>(debugView)])
		{
			const Status ensured = RetryAfterTrim(*state.Device, std::format("the {} debug view's pipelines", RenderDebugViewToString(debugView)),
				[&state, debugView]()
			{
				return state.PipelineSet->EnsureDebugView(debugView);
			});
			if (!ensured.has_value())
			{
				if (ensured.error().GetCode() != ErrorCode::OutOfMemory)
					return std::unexpected(ensured.error());
				const RenderDebugView requested = debugView;
				debugView = state.Pipelines->HasView[static_cast<size_t>(state.RenderedDebugView)] ? state.RenderedDebugView : RenderDebugView::Lit;
				bool& logged = state.DebugViewFailureLogged[static_cast<size_t>(requested)];
				if (!logged)
				{
					logged = true;
					ENGINE_CORE_WARN("The {} debug view could not be created and the view stays on the {} debug view (logged once per view): {}",
						RenderDebugViewToString(requested), RenderDebugViewToString(debugView), ensured.error());
				}
				Error kept = Error(ensured.error()).WithContext(std::format("the view was rendered in the {} debug view instead", RenderDebugViewToString(debugView)));
				debugViewError = std::unexpected(std::move(kept));
			}
		}
		if (snapshot.Quality.ShadowMapSize < 256 || snapshot.Quality.ShadowMapSize > 8192
			|| (snapshot.Quality.ShadowMapSize & (snapshot.Quality.ShadowMapSize - 1)) != 0)
			return MakeError(ErrorCode::InvalidArgument, "shadow map size must be a power of two in [256,8192]");
		// CreatePipelines replaced the binding layouts every binding set of this renderer was made for (decision 2).
		if (state.PipelineGeneration != state.Pipelines->Generation)
		{
			state.ClearBindings();
			state.PipelineGeneration = state.Pipelines->Generation;
		}
		state.Stats = {};
		state.RecordedPicking = false;
		state.SubmittedPicking = false;

		Detail::SceneRendererFrame frame;
		frame.Snapshot = &snapshot;
		frame.CommandList = &commandList;
		frame.Output = ComputeRenderOutputMapping(state.ViewWidth, state.ViewHeight, snapshot.Output);
		frame.DebugView = debugView;
		frame.IsLit = debugView == RenderDebugView::Lit;
		frame.IsDataView = debugView >= RenderDebugView::AO;
		frame.IsOverdraw = debugView == RenderDebugView::Overdraw;
		frame.Wireframe = frame.IsLit && HasFlag(snapshot.Flags, RenderViewFlags::Wireframe);
		frame.Picking = HasFlag(snapshot.Flags, RenderViewFlags::Picking);
		frame.Selection = !frame.IsDataView && snapshot.HasCamera && HasFlag(snapshot.Flags, RenderViewFlags::EditorOverlays)
			&& HasFlag(snapshot.Flags, RenderViewFlags::Selection) && !snapshot.SelectedEntities.empty();
		frame.PixelArt = snapshot.Output.PixelArt;
		// A debug-view switch that failed alone is the render's first partial error (decision 44).
		frame.FirstError = std::move(debugViewError);

		RenderStats stats{ .Available = true, .FrameIndex = snapshot.FrameIndex, .Width = state.ViewWidth, .Height = state.ViewHeight, .ShadowMapSize = snapshot.Quality.ShadowMapSize, .SsaoHalfResolution = snapshot.Quality.SsaoHalfResolution };
		GpuProfiler* profiler = nullptr;
		const uint32_t slot = state.NextTimingSlot;
		state.NextTimingSlot = (slot + 1) % static_cast<uint32_t>(state.TimingSubmissions.size());
		state.PendingTimingSlot.reset();
		GpuTimingFrame completedTiming;
		if (state.TimingSubmissions[slot] <= state.Device->GetCompletedSubmissionID())
		{
			state.Profiler->BeginFrame(slot, snapshot.FrameIndex);
			completedTiming = state.Profiler->GetLastFrameResult();
			profiler = state.Profiler.get();
			state.PendingTimingSlot = slot;
		}
		RenderRecordingContext recording(stats, Utils::SceneRendererNowSeconds, profiler);
		frame.Recording = &recording;

		// The stages (decision 1), each between the GPU-zone hook's BeginStage and EndStage (decision 4). The Prepare stage's
		// error is one of Render's errors before recording: nothing is recorded.
		recording.BeginStage(RenderStage::Prepare, nullptr);
		const Status prepared = state.RecordPrepareStage(frame);
		recording.EndStage();
		if (!prepared.has_value())
			return prepared;
		state.PendingSubmission = true;
		state.RenderedDebugView = debugView;
		state.ImageFrame = snapshot.FrameIndex;
		state.ImageRevision = snapshot.SceneRevision;
		state.ImageGeneration = state.Generation;
		state.ImagePickTable = frame.Picking ? std::vector<UUID>(snapshot.PickTable.begin(), snapshot.PickTable.end()) : std::vector<UUID>();
		state.FinalTarget = 0;
		commandList.beginMarker("SceneRenderer");
		state.RecordViewSetup(frame);
		const auto recordStage = [&state, &frame, &recording, &commandList](RenderStage stage, void (State::*record)(Detail::SceneRendererFrame&))
		{
			recording.BeginStage(stage, &commandList);
			(state.*record)(frame);
			recording.EndStage();
		};
		recordStage(RenderStage::Deform, &State::RecordDeformStage);
		recordStage(RenderStage::Shadows, &State::RecordShadowsStage);
		recordStage(RenderStage::DepthNormal, &State::RecordDepthNormalStage);
		recordStage(RenderStage::Ambient, &State::RecordAmbientStage);
		recordStage(RenderStage::Forward, &State::RecordForwardStage);
		recordStage(RenderStage::Sprites, &State::RecordSpritesStage);
		recordStage(RenderStage::HdrPost, &State::RecordHdrPostStage);
		recordStage(RenderStage::Tonemap, &State::RecordTonemapStage);
		recordStage(RenderStage::LdrPost, &State::RecordLdrPostStage);
		state.FinalTarget = frame.SceneTarget;
		recordStage(RenderStage::SceneOverlay, &State::RecordSceneOverlayStage);
		recordStage(RenderStage::Output, &State::RecordOutputStage);
		recordStage(RenderStage::EditorOverlay, &State::RecordEditorOverlayStage);
		commandList.endMarker();
		state.ReleaseUnusedBindings();
		std::set<size_t> visibleMeshes;
		for (const auto& draw : state.OpaqueDraws)
			visibleMeshes.insert(draw.SnapshotIndex);
		for (const auto& draw : state.TransparentDraws)
			visibleMeshes.insert(draw.SnapshotIndex);
		stats.VisibleMeshes = static_cast<uint32_t>(visibleMeshes.size());
		stats.CulledSubmeshes = state.Stats.CulledSubmeshes;
		stats.ShadowedSpotLights = static_cast<uint32_t>(frame.Spots.Tiles.size());
		stats.DroppedSpotShadows = frame.Spots.Dropped;
		for (const RenderPassStats& pass : stats.Passes)
			if (pass.Name == "DirectionalShadows" || pass.Name == "SpotShadows")
				stats.ShadowDraws += pass.DrawCalls;
		stats.MemoryAllocationCount = state.Device->GetMemoryAllocationCount();
		stats.MaxMemoryAllocationCount = state.Device->GetInfo().MaxMemoryAllocationCount;
		if (stats.MemoryAllocationCount > 2000 && !state.AllocationLimitLogged)
		{
			state.AllocationLimitLogged = true;
			ENGINE_CORE_WARN("Renderer device uses {} native memory allocations (device limit {}); more than 2000 warrants inspection (logged once per view)",
				stats.MemoryAllocationCount, stats.MaxMemoryAllocationCount);
		}
		stats.CpuMilliseconds = (Utils::SceneRendererNowSeconds() - started) * 1000.0;
		// The stats order of the pass declarations (RenderPassDeclaration.h), whatever the recording order.
		std::stable_sort(stats.Passes.begin(), stats.Passes.end(), [](const RenderPassStats& left, const RenderPassStats& right)
		{
			return Utils::GetStatsPosition(left.Name) < Utils::GetStatsPosition(right.Name);
		});
		state.History.PublishCpu(std::move(stats));
		state.History.PublishGpu(completedTiming);
		return std::move(frame.FirstError);
	}

	nvrhi::ITexture* SceneRenderer::GetFinalTexture() const
	{
		const State& state = *m_State;
		if (!state.OutputMapping.IsIdentity() && state.OutputMapping.ViewWidth == state.ViewWidth && state.OutputMapping.ViewHeight == state.ViewHeight
			&& state.OutputTargets.Color != nullptr)
			return state.OutputTargets.Color.Get();
		return state.Targets.Ldr[state.FinalTarget].Get();
	}

	uint32_t SceneRenderer::GetWidth() const
	{
		return m_State->ViewWidth;
	}

	uint32_t SceneRenderer::GetHeight() const
	{
		return m_State->ViewHeight;
	}

	const SceneRenderStats& SceneRenderer::GetLastStats() const
	{
		return m_State->Stats;
	}

	RenderDebugView SceneRenderer::GetRenderedDebugView() const
	{
		return m_State->RenderedDebugView;
	}

	void SceneRenderer::State::RecordViewSetup(Detail::SceneRendererFrame& frame)
	{
		nvrhi::ICommandList& commandList = *frame.CommandList;
		commandList.writeBuffer(ViewConstantsBuffer, &frame.View, sizeof(frame.View));
		commandList.writeBuffer(EnvironmentConstantsBuffer, &frame.Environment, sizeof(frame.Environment));
		commandList.writeBuffer(ShadowConstantsBuffer, &frame.Shadow, sizeof(frame.Shadow));
		if (!Lights.empty())
			commandList.writeBuffer(LightBuffer, Lights.data(), Lights.size() * sizeof(ShaderLight));
		const glm::vec3 clearColor = frame.Camera.ClearColor;
		commandList.clearTextureFloat(Targets.SceneColor, nvrhi::AllSubresources, nvrhi::Color(clearColor.r, clearColor.g, clearColor.b, 1.0f));
		commandList.clearDepthStencilTexture(Targets.SceneDepth, nvrhi::AllSubresources, true, 0.0f, false, 0);
		commandList.clearTextureFloat(Targets.SceneNormals, nvrhi::AllSubresources, nvrhi::Color(0.0f));
		if (frame.Picking)
		{
			commandList.clearTextureUInt(Targets.EntityId, nvrhi::AllSubresources, 0);
			RecordedPicking = true;
		}
	}

	Status SceneRenderer::State::RecordPrepareStage(Detail::SceneRendererFrame& frame)
	{
		// The scene-resolution targets at the output mapping's scene size.
		if (Targets.Width != frame.Output.SceneWidth || Targets.Height != frame.Output.SceneHeight)
		{
			ENGINE_TRY_ASSIGN(Detail::SceneRendererTargets targets, CreateTargets(frame.Output.SceneWidth, frame.Output.SceneHeight));
			Targets = std::move(targets);
			FinalTarget = 0;
			ClearTargetBindings();
			TargetPool->ReleaseFree();
		}
		const RenderSnapshot& snapshot = *frame.Snapshot;
		RenderRecordingContext& recording = *frame.Recording;
		frame.HasCamera = snapshot.HasCamera;
		if (frame.HasCamera && (!Utils::IsFinite(snapshot.Camera.View) || !Utils::IsFinite(snapshot.Camera.Projection) || !Utils::IsFinite(snapshot.Camera.Position)))
		{
			frame.KeepFirstError(MakeError(ErrorCode::InvalidArgument, "the camera of the render snapshot (entity {}) has a matrix that is not finite; the view is cleared",
				snapshot.Camera.Entity));
			frame.HasCamera = false;
		}
		const bool hasCamera = frame.HasCamera;
		frame.Camera = hasCamera ? snapshot.Camera : CameraData{};
		frame.Camera.ViewportWidth = Targets.Width;
		frame.Camera.ViewportHeight = Targets.Height;
		const CameraData& camera = frame.Camera;
		const PostProcessSettings& post = snapshot.Post;
		frame.Ao = hasCamera && ((frame.IsLit && !frame.Wireframe) || frame.DebugView == RenderDebugView::AO) && post.SsaoEnabled;
		frame.HalfAo = snapshot.Quality.SsaoHalfResolution || post.SsaoQuality == RenderSsaoQuality::Low;
		const bool shadows = hasCamera && ((frame.IsLit && !frame.Wireframe) || frame.DebugView == RenderDebugView::ShadowCascades);
		// The Lit view follows Post; the debug views and the 2D colour path (decision 7) fix exposure 1 and no dither.
		const bool tonemapped = frame.IsLit && !frame.PixelArt;

		recording.BeginPass("Prepare");
		frame.View = Utils::MakeViewConstants(camera, Targets.Width, Targets.Height, tonemapped ? Utils::GetExposure(post) : 1.0f);
		LightCullResult culled;
		Lights.clear();
		if (hasCamera)
		{
			culled = CullLights(snapshot.Lights, camera);
			for (const uint32_t index : culled.Visible)
				Lights.push_back(Utils::MakeShaderLight(snapshot.Lights[index]));
			Stats.Lights = static_cast<uint32_t>(culled.Visible.size());
			Stats.CulledLights = culled.Culled;
			Stats.DroppedLights = culled.Dropped;
			if (culled.Dropped > 0 && !LightLimitLogged)
			{
				LightLimitLogged = true;
				ENGINE_CORE_WARN("{}: {} lights exceed the view's budget of {} (logged once per view)", RenderLightLimitExceededCode, culled.Dropped, MaxVisibleLights);
			}
		}
		frame.View.LightCount = static_cast<uint32_t>(Lights.size());
		if (shadows)
		{
			// Choose in snapshot order, independently of CullLights' importance ordering. Uploaded indices are remapped. A light
			// the planner rejects (data the schemas accept that cannot be shadowed, such as a ShadowDistance within the camera's
			// near plane) stays lit without shadows and the next eligible light is tried; it is not a render error.
			uint32_t rejectedLights = 0;
			std::string rejection;
			for (uint32_t index = 0; index < snapshot.Lights.size(); ++index)
			{
				const LightData& light = snapshot.Lights[index];
				const auto visible = std::find(culled.Visible.begin(), culled.Visible.end(), index);
				if (light.Type != RenderLightType::Directional || !light.CastShadows || visible == culled.Visible.end())
					continue;
				auto planned = BuildShadowCascades(camera, light, index, snapshot.Quality.ShadowMapSize);
				if (!planned)
				{
					if (rejection.empty())
						rejection = std::format("directional light {}: {}", light.Entity, planned.error().GetMessageText());
					++rejectedLights;
					continue;
				}
				frame.Cascades = *planned;
				const ShadowCascadeSet& cascades = frame.Cascades;
				frame.Shadow.Counts.x = cascades.Count;
				frame.Shadow.Counts.z = static_cast<uint32_t>(visible - culled.Visible.begin());
				frame.Shadow.Directional = glm::vec4(light.LightAngle * std::numbers::pi_v<float> / 180.0f, light.DepthBias, light.NormalBias, light.ShadowDistance);
				for (uint32_t cascade = 0; cascade < cascades.Count; ++cascade)
				{
					const ShadowCascade& plan = cascades.Cascades[cascade];
					frame.Shadow.CascadeViewProjection[cascade] = plan.ViewProjection;
					frame.Shadow.CascadeSplits[cascade] = plan.SplitFar;
					frame.Shadow.CascadeBlendStarts[cascade] = plan.BlendStart;
					frame.Shadow.CascadeTexelWorldSizes[cascade] = plan.TexelWorldSize;
					frame.Shadow.CascadeDepthRanges[cascade] = glm::vec4(plan.LightNear, plan.LightFar, plan.PenumbraUvPerMetre, 0.05f);
				}
				break;
			}
			// Spots are validated one at a time: one that cannot be shadowed is rejected alone (SpotShadowAtlas.h).
			auto allocated = AllocateSpotShadowAtlas(snapshot.Lights, culled.Visible, camera);
			if (!allocated)
				frame.KeepFirstError(std::unexpected(std::move(allocated).error()));
			else
			{
				frame.Spots = std::move(*allocated);
				if (frame.Spots.Rejected > 0 && rejection.empty())
					rejection = "a spot light's cone, range or values cannot be projected";
				rejectedLights += frame.Spots.Rejected;
			}
			if (rejectedLights > 0 && !ShadowRejectionLogged)
			{
				ShadowRejectionLogged = true;
				ENGINE_CORE_WARN("{} shadow-casting light(s) cannot be shadowed and are lit without shadows (first: {}; logged once per view)",
					rejectedLights, rejection);
			}
			frame.Shadow.Counts.y = static_cast<uint32_t>(frame.Spots.Tiles.size());
			for (size_t index = 0; index < frame.Spots.Tiles.size(); ++index)
			{
				const SpotShadowTile& tile = frame.Spots.Tiles[index];
				const LightData& light = snapshot.Lights[tile.LightIndex];
				SpotShadowConstants& constants = frame.Shadow.Spots[index];
				constants.ViewProjection = tile.ViewProjection;
				constants.UvScaleBias = tile.UvScaleBias;
				constants.DepthSoftness = glm::vec4(tile.NearClip, tile.FarClip, light.SourceRadius, 0.05f * tile.UvScaleBias.x);
				const float projectionScale = glm::length(glm::vec3(tile.ViewProjection[0][0], tile.ViewProjection[1][0], tile.ViewProjection[2][0]));
				constants.Bias = glm::vec4(light.DepthBias, light.NormalBias, 2.0f / (projectionScale * static_cast<float>(SpotShadowTileSize - 2 * SpotShadowGuardTexels)), 0.0f);
				constants.Indices.x = static_cast<uint32_t>(std::find(culled.Visible.begin(), culled.Visible.end(), tile.LightIndex) - culled.Visible.begin());
			}
			if (frame.Spots.Dropped > 0 && !SpotLimitLogged)
			{
				SpotLimitLogged = true;
				ENGINE_CORE_WARN("{}: {} visible spot shadows exceed the budget of {} (logged once per view)", RenderSpotShadowBudgetCode, frame.Spots.Dropped, MaxSpotShadowLights);
			}
		}
		OpaqueDraws.clear();
		TransparentDraws.clear();
		if (hasCamera)
			frame.KeepFirstError(PrepareDraws(snapshot, camera));
		const GpuEnvironment* environment = hasCamera && frame.IsLit && !frame.Wireframe ? Cache->GetEnvironment(snapshot.Environment.Environment) : nullptr;
		frame.Environment = Utils::MakeEnvironmentConstants(snapshot.Environment, environment);
		frame.EnvironmentSpecular = environment != nullptr ? environment->Specular : Pipelines->BlackCube;
		frame.EnvironmentSkybox = environment != nullptr ? environment->Skybox : nvrhi::TextureHandle();
		if (tonemapped && Assets->GetAssetType(BuiltinAssetHandles::BlueNoiseTexture) == AssetType::Texture)
		{
			const GpuTexture& noise = Cache->GetTexture(BuiltinAssetHandles::BlueNoiseTexture);
			if (!noise.IsPlaceholder)
				frame.BlueNoise = noise.Texture;
		}
		recording.EndPass({});
		return EnsureFrameTargets(frame.Picking, frame.Ao, frame.HalfAo, frame.Cascades.Count != 0 ? snapshot.Quality.ShadowMapSize : 0, !frame.Spots.Tiles.empty(),
			frame.Selection, frame.IsOverdraw);
	}

	// Reserved for M23's Skinning pass (RenderStage::Deform), the first GPU work of a frame: no mesh is deformed in M18, so
	// the shadow casters, the prepass and the forward passes draw the mirrors' own vertex buffers.
	void SceneRenderer::State::RecordDeformStage(Detail::SceneRendererFrame& /*frame*/)
	{
	}

	void SceneRenderer::State::RecordShadowsStage(Detail::SceneRendererFrame& frame)
	{
		const ShadowCascadeSet& cascades = frame.Cascades;
		const SpotShadowAtlas& spots = frame.Spots;
		if (cascades.Count == 0 && spots.Tiles.empty())
			return;
		const RenderSnapshot& snapshot = *frame.Snapshot;
		std::vector<MeshDrawItem> finiteCasters;
		for (const MeshDrawItem& item : snapshot.Meshes)
			if (Utils::IsFinite(item.World))
				finiteCasters.push_back(item);
		const ShadowRenderInputs inputs{
			.Casters = finiteCasters,
			.Cascades = cascades.Count != 0 ? &cascades : nullptr,
			.Spots = !spots.Tiles.empty() ? &spots : nullptr,
			.CascadeTarget = cascades.Count != 0 ? Targets.Cascades.Get() : nullptr,
			.AtlasTarget = !spots.Tiles.empty() ? Targets.SpotAtlas.Get() : nullptr,
			.CascadeFramebuffers = Targets.CascadeFramebuffers,
			.AtlasFramebuffer = !spots.Tiles.empty() ? Targets.SpotAtlasFramebuffer.Get() : nullptr,
			.CasterConstants = ShadowCasterConstantsBuffer,
			.DepthBias = cascades.Count != 0 ? snapshot.Lights[cascades.LightIndex].DepthBias : 1.0f,
			.NormalBias = cascades.Count != 0 ? snapshot.Lights[cascades.LightIndex].NormalBias : 1.0f,
		};
		auto result = Pipelines->Shadows->Record(*frame.CommandList, *frame.Recording, ShadowBindings, *Cache, *Assets, inputs);
		if (!result)
			frame.KeepFirstError(std::unexpected(std::move(result).error()));
	}

	void SceneRenderer::State::RecordDepthNormalStage(Detail::SceneRendererFrame& frame)
	{
		nvrhi::ICommandList& commandList = *frame.CommandList;
		frame.PrepassSet = nullptr;
		if (frame.HasCamera)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(Utils::ViewConstantsRegister, ViewConstantsBuffer),
				nvrhi::BindingSetItem::Sampler(Utils::AnisoWrapRegister, Pipelines->AnisoWrap),
				nvrhi::BindingSetItem::PushConstants(Utils::DrawConstantsSlot, sizeof(DrawConstants)),
			};
			auto created = ViewBindings.GetOrCreate(*Device, desc, *Pipelines->Layouts.PrepassView);
			if (created)
				frame.PrepassSet = *created;
			else
				frame.KeepFirstError(std::unexpected(std::move(created).error()));
		}
		// The Overdraw data view draws its own pass in the Forward stage; with picking it still needs EntityId.
		if (frame.PrepassSet == nullptr || (frame.IsOverdraw && !frame.Picking))
			return;
		frame.Recording->BeginPass("DepthNormal", &commandList);
		RenderPassCounters counters;
		RecordMeshPass(commandList, frame.Picking ? Detail::SceneRendererMeshPass::Picking : Detail::SceneRendererMeshPass::Prepass, OpaqueDraws,
			*frame.PrepassSet, frame.DebugView, counters);
		frame.Recording->EndPass(counters);
	}

	void SceneRenderer::State::RecordAmbientStage(Detail::SceneRendererFrame& frame)
	{
		frame.Occlusion = Pipelines->WhiteAo;
		frame.LinearDepth = Pipelines->FarDepth;
		if (!frame.Ao)
			return;
		nvrhi::ICommandList& commandList = *frame.CommandList;
		RenderRecordingContext& recording = *frame.Recording;
		const Status depth = Pipelines->DepthPyramid->Record(commandList, recording, DepthBindings, { .SceneDepth = Targets.SceneDepth, .ViewDepth = Targets.ViewDepth, .Camera = frame.Camera });
		frame.KeepFirstError(depth);
		if (!depth)
			return;
		auto result = Pipelines->Gtao->Record(commandList, recording, GtaoBindings, { .Camera = frame.Camera, .Post = frame.Snapshot->Post, .HalfResolution = frame.HalfAo, .ViewDepth = Targets.ViewDepth, .SceneNormals = Targets.SceneNormals, .Occlusion = Targets.Occlusion, .Scratch = Targets.AoScratch });
		if (result)
		{
			frame.Occlusion = *result;
			frame.LinearDepth = Targets.ViewDepth;
		}
		else
			frame.KeepFirstError(std::unexpected(std::move(result).error()));
	}

	void SceneRenderer::State::RecordForwardStage(Detail::SceneRendererFrame& frame)
	{
		const RenderSnapshot& snapshot = *frame.Snapshot;
		nvrhi::ICommandList& commandList = *frame.CommandList;
		RenderRecordingContext& recording = *frame.Recording;
		if (frame.HasCamera && !frame.IsDataView && !frame.Wireframe)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(Utils::ViewConstantsRegister, ViewConstantsBuffer),
				nvrhi::BindingSetItem::ConstantBuffer(Utils::ShadowConstantsRegister, ShadowConstantsBuffer),
				nvrhi::BindingSetItem::ConstantBuffer(Utils::EnvironmentConstantsRegister, EnvironmentConstantsBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(Utils::LightsRegister, LightBuffer),
				nvrhi::BindingSetItem::Texture_SRV(Utils::ShadowCascadesRegister, frame.Cascades.Count != 0 ? Targets.Cascades : Pipelines->EmptyCascades),
				nvrhi::BindingSetItem::Texture_SRV(Utils::ShadowAtlasRegister, !frame.Spots.Tiles.empty() ? Targets.SpotAtlas : Pipelines->EmptyAtlas),
				nvrhi::BindingSetItem::Texture_SRV(Utils::EnvSpecularRegister, frame.EnvironmentSpecular),
				nvrhi::BindingSetItem::Texture_SRV(Utils::BrdfLutRegister, Pipelines->DfgLut->GetTexture()),
				nvrhi::BindingSetItem::Texture_SRV(Utils::AmbientOcclusionRegister, frame.Occlusion),
				nvrhi::BindingSetItem::Texture_SRV(Utils::ViewDepthRegister, frame.LinearDepth),
				nvrhi::BindingSetItem::Texture_SRV(Utils::SceneNormalsRegister, Targets.SceneNormals),
				nvrhi::BindingSetItem::Sampler(Utils::LinearClampRegister, Pipelines->LinearClamp),
				nvrhi::BindingSetItem::Sampler(Utils::ShadowCompareRegister, Pipelines->ShadowCompare),
				nvrhi::BindingSetItem::Sampler(Utils::AnisoWrapRegister, Pipelines->AnisoWrap),
				nvrhi::BindingSetItem::PushConstants(Utils::DrawConstantsSlot, sizeof(DrawConstants)),
			};
			auto forwardSet = ViewBindings.GetOrCreate(*Device, desc, *Pipelines->Layouts.ForwardView);
			if (!forwardSet)
				frame.KeepFirstError(std::unexpected(std::move(forwardSet).error()));
			else
			{
				if (!OpaqueDraws.empty())
				{
					recording.BeginPass("ForwardOpaque", &commandList);
					RenderPassCounters counters;
					RecordMeshPass(commandList, Detail::SceneRendererMeshPass::ForwardOpaque, OpaqueDraws, **forwardSet, frame.DebugView, counters);
					recording.EndPass(counters);
				}
				if (frame.IsLit && frame.Camera.ClearToSkybox && frame.EnvironmentSkybox != nullptr && snapshot.Environment.ShowSkybox)
				{
					recording.BeginPass("Skybox", &commandList);
					const Status drawn = Pipelines->Skybox->Record(commandList, SkyboxBindings, { .Framebuffer = Targets.ForwardFramebuffer, .ViewConstants = ViewConstantsBuffer, .EnvironmentConstants = EnvironmentConstantsBuffer, .Skybox = frame.EnvironmentSkybox });
					recording.EndPass({ .DrawCalls = drawn ? 1U : 0U, .Triangles = drawn ? 1U : 0U });
					frame.KeepFirstError(drawn);
				}
				if (!TransparentDraws.empty())
				{
					// GTAO and its depth-aware upsample describe the prepass (Opaque and Mask) surface at each pixel, not the
					// Blend surface in front of it: the transparent pass binds the neutral white AO and far depth instead, so
					// a transparent surface's indirect light uses its material AO only.
					nvrhi::BindingSetDesc transparentDesc = desc;
					for (nvrhi::BindingSetItem& item : transparentDesc.bindings)
					{
						if (item.type == nvrhi::ResourceType::Texture_SRV && item.slot == Utils::AmbientOcclusionRegister)
							item.resourceHandle = Pipelines->WhiteAo;
						else if (item.type == nvrhi::ResourceType::Texture_SRV && item.slot == Utils::ViewDepthRegister)
							item.resourceHandle = Pipelines->FarDepth;
					}
					auto transparentSet = ViewBindings.GetOrCreate(*Device, transparentDesc, *Pipelines->Layouts.ForwardView);
					if (!transparentSet)
						frame.KeepFirstError(std::unexpected(std::move(transparentSet).error()));
					else
					{
						recording.BeginPass("ForwardTransparent", &commandList);
						RenderPassCounters counters;
						RecordMeshPass(commandList, Detail::SceneRendererMeshPass::ForwardTransparent, TransparentDraws, **transparentSet, frame.DebugView, counters);
						recording.EndPass(counters);
					}
				}
			}
		}
		if (frame.IsOverdraw)
		{
			recording.BeginPass("Overdraw", &commandList);
			RenderPassCounters counters;
			commandList.clearTextureFloat(Targets.Overdraw, nvrhi::AllSubresources, nvrhi::Color(0.0f));
			if (frame.PrepassSet != nullptr)
			{
				RecordMeshPass(commandList, Detail::SceneRendererMeshPass::Overdraw, OpaqueDraws, *frame.PrepassSet, frame.DebugView, counters);
				RecordMeshPass(commandList, Detail::SceneRendererMeshPass::Overdraw, TransparentDraws, *frame.PrepassSet, frame.DebugView, counters);
			}
			recording.EndPass(counters);
		}
	}

	// Reserved for M21's Sprites and Lights2D passes (RenderStage::Sprites): no sprite is drawn in M18.
	void SceneRenderer::State::RecordSpritesStage(Detail::SceneRendererFrame& /*frame*/)
	{
	}

	void SceneRenderer::State::RecordHdrPostStage(Detail::SceneRendererFrame& frame)
	{
		const PostProcessSettings& post = frame.Snapshot->Post;
		frame.Bloom = nullptr;
		frame.BloomIntensity = std::isfinite(post.BloomIntensity) ? std::clamp(post.BloomIntensity, 0.0f, 1.0f) : 0.0f;
		// The debug views and the 2D colour path (decision 7: a bloom of any intensity changes texels) draw no bloom.
		if (!frame.IsLit || frame.Wireframe || frame.PixelArt || !post.BloomEnabled || frame.BloomIntensity <= 0.0f)
			return;
		nvrhi::ICommandList& commandList = *frame.CommandList;
		frame.Recording->BeginPass("Bloom", &commandList);
		RenderPassCounters counters;
		auto result = Pipelines->Bloom->RecordCounted(commandList, BloomBindings, { .SceneColor = Targets.SceneColor, .Chain = Targets.Bloom }, counters);
		if (result)
			frame.Bloom = *result;
		else
			frame.KeepFirstError(std::unexpected(std::move(result).error()));
		frame.Recording->EndPass(counters);
	}

	void SceneRenderer::State::RecordTonemapStage(Detail::SceneRendererFrame& frame)
	{
		const PostProcessSettings& post = frame.Snapshot->Post;
		nvrhi::ICommandList& commandList = *frame.CommandList;
		RenderRecordingContext& recording = *frame.Recording;
		// Dedicated data views bypass all tone/color transforms; Tonemap names the final output stage in every view.
		recording.BeginPass("Tonemap", &commandList);
		Status encoded;
		if (frame.IsDataView)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, ViewConstantsBuffer),
				nvrhi::BindingSetItem::ConstantBuffer(1, ShadowConstantsBuffer),
				nvrhi::BindingSetItem::Texture_SRV(0, Targets.SceneDepth),
				nvrhi::BindingSetItem::Texture_SRV(1, frame.Occlusion),
				nvrhi::BindingSetItem::Texture_SRV(2, frame.LinearDepth),
				nvrhi::BindingSetItem::Texture_SRV(3, Targets.SceneNormals),
				nvrhi::BindingSetItem::Texture_SRV(4, frame.IsOverdraw ? Targets.Overdraw : Targets.SceneColor),
				nvrhi::BindingSetItem::Texture_UAV(0, Targets.Ldr[0]),
				nvrhi::BindingSetItem::PushConstants(3, sizeof(SceneDataViewConstants)),
			};
			auto set = DataViewBindings.GetOrCreate(*Device, desc, *Pipelines->DataView.BindingLayouts[0]);
			if (!set)
				encoded = std::unexpected(std::move(set).error());
			else
			{
				nvrhi::ComputeState compute;
				compute.pipeline = Pipelines->DataView.Pipeline;
				compute.bindings = { *set };
				commandList.setComputeState(compute);
				const SceneDataViewConstants constants{ .Options = glm::uvec4(static_cast<uint32_t>(frame.DebugView), 0, 0, 0) };
				commandList.setPushConstants(&constants, sizeof(constants));
				commandList.dispatch((Targets.Width + 7) / 8, (Targets.Height + 7) / 8);
			}
		}
		else
		{
			// The Lit view's tonemapper and dither follow Post; the material debug views and the 2D colour path (decision 7) use
			// Linear without dither, and every view but Normals, Roughness and Metallic encodes with the sRGB OETF.
			const bool tonemapped = frame.IsLit && !frame.PixelArt;
			const TonemapSettings settings{ .Tonemapper = tonemapped ? post.Tonemap : RenderTonemapper::Linear,
				.Dither = tonemapped && frame.BlueNoise != nullptr,
				.EncodeSrgb = frame.IsLit || frame.DebugView == RenderDebugView::Albedo || frame.DebugView == RenderDebugView::Emissive };
			encoded = Pipelines->Tonemap->Record(commandList, TonemapBindings, { .ViewConstants = ViewConstantsBuffer, .SceneColor = Targets.SceneColor, .Bloom = frame.Bloom, .BloomIntensity = frame.Bloom != nullptr ? frame.BloomIntensity : 0.0f, .BlueNoise = frame.BlueNoise, .LdrColor = Targets.Ldr[0], .Settings = settings });
		}
		recording.EndPass({ .Dispatches = encoded ? 1U : 0U });
		frame.KeepFirstError(std::move(encoded));
		frame.SceneTarget = 0;
	}

	void SceneRenderer::State::RecordLdrPostStage(Detail::SceneRendererFrame& frame)
	{
		const PostProcessSettings& post = frame.Snapshot->Post;
		// FXAA blends texels across edges: never in a debug view, the wireframe or the 2D colour path (decision 7).
		if (!frame.IsLit || frame.Wireframe || frame.PixelArt || !post.FxaaEnabled)
			return;
		nvrhi::ICommandList& commandList = *frame.CommandList;
		frame.Recording->BeginPass("FXAA", &commandList);
		auto result = Pipelines->Fxaa->Record(commandList, FxaaBindings, { .Source = Targets.Ldr[0], .Destination = Targets.Ldr[1] });
		if (result)
			frame.SceneTarget = 1;
		else
			frame.KeepFirstError(std::unexpected(std::move(result).error()));
		frame.Recording->EndPass({ .Dispatches = result ? 1U : 0U });
	}

}
