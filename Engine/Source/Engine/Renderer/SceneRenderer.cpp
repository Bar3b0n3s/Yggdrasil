#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/BloomPass.h"
#include "Engine/Renderer/BrdfLut.h"
#include "Engine/Renderer/DebugRenderer.h"
#include "Engine/Renderer/FxaaPass.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/Private/ForwardPipelines.h"
#include "Engine/Renderer/Private/LightingInputs.h"
#include "Engine/Renderer/Private/SceneRendererState.h"
#include "Engine/Renderer/EditorOverlay.h"
#include "Shared/ShadowConstants.h"
#include "Shared/Private/SceneDataViewConstants.h"
#include "Engine/Renderer/Private/MaterialBindingCache.h"
#include "Engine/Renderer/RenderPrepare.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Engine/Renderer/SkyboxPass.h"
#include "Engine/Renderer/TextRenderer.h"
#include "Engine/Renderer/TonemapPass.h"
#include "Shared/DrawConstants.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/ShaderLight.h"
#include "Shared/ViewConstants.h"

#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <limits>
#include <numbers>
#include <set>
#include <cstddef>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// The pipelines (SceneRendererPipelines): the Scene program's mesh variants (Renderer/Private/ForwardPipelines.h), with
// the shared binding layouts every variant binds its view and material sets through, the samplers of set 0, the black
// cube a view without an environment map binds as EnvSpecular, and the passes the set owns (SceneRenderer.h).
//
// Per renderer (one view): the targets (SceneDepth, SceneNormals, SceneColor, the two LDR targets, the bloom chain) and
// their framebuffers, rebuilt by Resize; the ViewConstants and EnvironmentConstants buffers and the Lights structured
// buffer, written by every Render through the command list (keepInitialState buffers, so NVRHI orders each write after
// the reads of earlier submissions); one PassBindingCache for the view's own set-0 sets and one per shared pass, and the
// material sets (Private/MaterialBindingCache.h), each keeping exactly what the last Render used.
//
// Render follows SceneRenderer.h's pass list. Pass 1 resolves every draw before recording: its mesh and material mirrors
// (GpuResourceCache), its pipeline variant (the material's alpha mode and double-sidedness, the world matrix's mirroring)
// and its material set, then sorts the opaque draws (Opaque and Mask) by (forward variant, material, mesh, snapshot order)
// and the transparent ones (Blend) back to front; the prepass draws the opaque ones in the same order with its own variants.

namespace Engine {

	// The startup count is the sum of the pipelines each part of the set declares (SceneRenderer.h).
	static_assert(SceneRendererPipelines::StartupPipelineCount
		== SceneRendererPipelines::MeshPipelineCount + SkyboxPass::PipelineCount + BloomPass::PipelineCount + TonemapPass::PipelineCount
			+ FxaaPass::PipelineCount + DebugRenderer::PipelineCount + TextRenderer::PipelineCount + BrdfLut::PipelineCount
			+ ShadowPass::PipelineCount + DepthPyramidPass::PipelineCount + GtaoPass::PipelineCount + SelectionPass::PipelineCount + 1);
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

		// A buffer of `size` bytes kept in `state` between command lists (keepInitialState): a constant buffer, or a
		// structured buffer of `structStride`-byte elements.
		static Result<nvrhi::BufferHandle> CreateViewBuffer(GraphicsDevice& device, size_t size, uint32_t structStride, std::string debugName)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = size;
			desc.isConstantBuffer = structStride == 0;
			desc.structStride = structStride;
			desc.initialState = structStride == 0 ? nvrhi::ResourceStates::ConstantBuffer : nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = std::move(debugName);
			return device.CreateBuffer(desc);
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
			commandList->open();
			commandList->clearTextureFloat(texture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
			commandList->close();
			device.ExecuteCommandList(*commandList);
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
			list->open();
			if (format == nvrhi::Format::D32)
				list->clearDepthStencilTexture(texture, nvrhi::AllSubresources, true, value, false, 0);
			else
				list->clearTextureFloat(texture, nvrhi::AllSubresources, nvrhi::Color(value));
			list->close();
			device.ExecuteCommandList(*list);
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
		ENGINE_TRY_ASSIGN(state.Layouts, Utils::CreateMeshBindingLayouts(pipelines));
		for (uint32_t variant = 0; variant < Utils::PrepassVariantCount; ++variant)
		{
			ENGINE_TRY_ASSIGN(state.Prepass[variant], Utils::CreatePrepassPipeline(pipelines, state.Layouts, variant));
			ENGINE_TRY_ASSIGN(state.Picking[variant], Utils::CreatePrepassPipeline(pipelines, state.Layouts, variant, true));
		}
		const auto lit = static_cast<size_t>(RenderDebugView::Lit);
		for (uint32_t variant = 0; variant < Utils::ForwardVariantCount; ++variant)
		{
			ENGINE_TRY_ASSIGN(state.Forward[lit][variant], Utils::CreateForwardPipeline(pipelines, state.Layouts, variant, RenderDebugView::Lit));
		}
		state.HasView[lit] = true;
		for (uint32_t variant = 0; variant < Utils::MeshCullModeCount; ++variant)
		{
			ENGINE_TRY_ASSIGN(state.Overdraw[variant], Utils::CreateOverdrawPipeline(pipelines, state.Layouts, variant));
		}
		ENGINE_TRY_ASSIGN(state.DataView, pipelines.CreateComputePipeline({ .Layout = Utils::GetSceneDataViewLayout() }));
		for (size_t index = static_cast<size_t>(RenderDebugView::AO); index < RenderDebugViewCount; ++index)
			state.HasView[index] = true;

		ENGINE_TRY_ASSIGN(state.LinearClamp,
			device.CreateSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp)));
		ENGINE_TRY_ASSIGN(state.AnisoWrap, device.CreateSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap).setMaxAnisotropy(Utils::MaxSamplerAnisotropy)));
		ENGINE_TRY_ASSIGN(state.BlackCube, Utils::CreateBlackCube(device));
		ENGINE_TRY_ASSIGN(state.EmptyCascades, Utils::CreateSceneNeutralTexture(device, nvrhi::Format::D32, true, 0.0f));
		ENGINE_TRY_ASSIGN(state.EmptyAtlas, Utils::CreateSceneNeutralTexture(device, nvrhi::Format::D32, false, 0.0f));
		ENGINE_TRY_ASSIGN(state.WhiteAo, Utils::CreateSceneNeutralTexture(device, nvrhi::Format::R8_UNORM, false, 1.0f));
		ENGINE_TRY_ASSIGN(state.FarDepth, Utils::CreateSceneNeutralTexture(device, nvrhi::Format::R16_FLOAT, false, 65504.0f));
		ENGINE_TRY_ASSIGN(state.ShadowCompare, device.CreateSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp).setReductionType(nvrhi::SamplerReductionType::Comparison)));
		ENGINE_TRY_ASSIGN(state.Shadows, ShadowPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(state.DepthPyramid, DepthPyramidPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(state.Gtao, GtaoPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(state.Selection, SelectionPass::Create(device, pipelines));

		ENGINE_TRY_ASSIGN(state.Skybox, SkyboxPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(state.Bloom, BloomPass::Create(device, pipelines, device.GetInfo().BloomFormat));
		ENGINE_TRY_ASSIGN(state.Tonemap, TonemapPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(state.Fxaa, FxaaPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(state.Debug, DebugRenderer::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(state.Text, TextRenderer::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(state.DfgLut, BrdfLut::Create(device, pipelines));
		state.PipelineCount = MeshPipelineCount + state.Skybox->GetPipelineCount() + state.Bloom->GetPipelineCount() + state.Tonemap->GetPipelineCount()
			+ state.Fxaa->GetPipelineCount() + state.Debug->GetPipelineCount() + state.Text->GetPipelineCount() + state.DfgLut->GetPipelineCount()
			+ ShadowPass::PipelineCount + DepthPyramidPass::PipelineCount + GtaoPass::PipelineCount + SelectionPass::PipelineCount + 1;
		ENGINE_CORE_INFO("Created the scene renderer's {} pipelines", state.PipelineCount);
		return set;
	}

	Status SceneRendererPipelines::EnsureDebugView(RenderDebugView view)
	{
		State& state = *m_State;
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
			ENGINE_TRY_ASSIGN(Targets.EntityId, Utils::CreateTarget(*TargetPool, width, height, nvrhi::Format::R32_UINT, Utils::TargetUsage::Attachment, "EntityId"));
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
			ENGINE_TRY_ASSIGN(Targets.Cascades, TargetPool->Acquire(ShadowPass::GetCascadeTargetDesc(shadowSize)));
		}
		if (spots && Targets.SpotAtlas == nullptr)
		{
			ENGINE_TRY_ASSIGN(Targets.SpotAtlas, TargetPool->Acquire(ShadowPass::GetAtlasTargetDesc()));
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
				if (IsOutsideView(submesh.Bounds, item.World, camera))
				{
					++Stats.CulledSubmeshes;
					continue;
				}
				const AssetHandle materialHandle = Utils::SelectMaterial(item, defaults, submesh.MaterialSlot);
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

		ENGINE_TRY_ASSIGN(state.ViewConstantsBuffer, Utils::CreateViewBuffer(device, sizeof(ViewConstants), 0, "SceneRenderer.ViewConstants"));
		ENGINE_TRY_ASSIGN(state.EnvironmentConstantsBuffer,
			Utils::CreateViewBuffer(device, sizeof(EnvironmentConstants), 0, "SceneRenderer.EnvironmentConstants"));
		ENGINE_TRY_ASSIGN(state.LightBuffer,
			Utils::CreateViewBuffer(device, sizeof(ShaderLight) * MaxVisibleLights, sizeof(ShaderLight), "SceneRenderer.Lights"));
		state.Lights.reserve(MaxVisibleLights);
		state.TargetPool = CreateScope<RenderTargetPool>(device);
		state.Picker = CreateScope<AsyncPicker>(device);
		state.Profiler = CreateScope<GpuProfiler>(device, device.GetFramesInFlight());
		state.TimingSubmissions.resize(device.GetFramesInFlight(), 0);
		ENGINE_TRY_ASSIGN(state.ShadowConstantsBuffer, Utils::CreateViewBuffer(device, sizeof(ShadowConstants), 0, "SceneRenderer.ShadowConstants"));
		ENGINE_TRY_ASSIGN(state.Targets, state.CreateTargets(specification.Width, specification.Height));
		return renderer;
	}

	Status SceneRenderer::Resize(uint32_t width, uint32_t height)
	{
		ENGINE_CORE_ASSERT(width > 0 && height > 0, "SceneRenderer::Resize needs a size of at least 1x1, got {}x{}", width, height);
		State& state = *m_State;
		if (width == state.Targets.Width && height == state.Targets.Height)
			return {};
		ENGINE_CORE_ASSERT(!state.PendingSubmission, "Resize requires OnSubmitted for the prior Render");
		if (state.PendingSubmission)
			return MakeError(ErrorCode::InvalidState, "Resize requires OnSubmitted for the prior Render");
		// Built aside first, so a failure keeps the current targets. The previous ones stay alive while a submitted command
		// list still references them (their framebuffers and binding sets are referenced by every list that used them).
		ENGINE_TRY_ASSIGN(Detail::SceneRendererTargets targets, state.CreateTargets(width, height));
		state.Targets = std::move(targets);
		state.FinalTarget = 0;
		state.ClearTargetBindings();
		state.TargetPool->ReleaseFree();
		CancelPicks();
		state.RecordedPicking = false;
		return {};
	}

	Status SceneRenderer::Render(nvrhi::ICommandList& commandList, const RenderSnapshot& snapshot)
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(!state.PendingSubmission, "Render requires OnSubmitted for the prior Render");
		if (state.PendingSubmission)
			return MakeError(ErrorCode::InvalidState, "Render requires OnSubmitted for the prior Render");
		const double started = Utils::SceneRendererNowSeconds();
		const RenderDebugView debugView = snapshot.DebugView;
		ENGINE_TRY(state.PipelineSet->EnsureDebugView(debugView));
		if (snapshot.Quality.ShadowMapSize < 256 || snapshot.Quality.ShadowMapSize > 8192
			|| (snapshot.Quality.ShadowMapSize & (snapshot.Quality.ShadowMapSize - 1)) != 0)
			return MakeError(ErrorCode::InvalidArgument, "shadow map size must be a power of two in [256,8192]");
		SceneRendererPipelines::State& pipelines = *state.Pipelines;
		state.Stats = {};
		state.RecordedPicking = false;
		state.SubmittedPicking = false;
		const bool isLit = debugView == RenderDebugView::Lit;
		const bool dataView = debugView >= RenderDebugView::AO;
		const bool overdraw = debugView == RenderDebugView::Overdraw;
		const bool wireframe = isLit && HasFlag(snapshot.Flags, RenderViewFlags::Wireframe);
		const bool picking = HasFlag(snapshot.Flags, RenderViewFlags::Picking);
		const bool selection = !dataView && snapshot.HasCamera && HasFlag(snapshot.Flags, RenderViewFlags::EditorOverlays)
			&& HasFlag(snapshot.Flags, RenderViewFlags::Selection) && !snapshot.SelectedEntities.empty();
		Status status;
		const auto keepFirstError = [&status](Status result)
		{
			if (!result && status)
				status = std::move(result);
		};
		bool hasCamera = snapshot.HasCamera;
		if (hasCamera && (!Utils::IsFinite(snapshot.Camera.View) || !Utils::IsFinite(snapshot.Camera.Projection) || !Utils::IsFinite(snapshot.Camera.Position)))
		{
			status = MakeError(ErrorCode::InvalidArgument, "the camera of the render snapshot (entity {}) has a matrix that is not finite; the view is cleared", snapshot.Camera.Entity);
			hasCamera = false;
		}
		CameraData camera = hasCamera ? snapshot.Camera : CameraData{};
		camera.ViewportWidth = state.Targets.Width;
		camera.ViewportHeight = state.Targets.Height;
		const PostProcessSettings& post = snapshot.Post;
		const bool ao = hasCamera && ((isLit && !wireframe) || debugView == RenderDebugView::AO) && post.SsaoEnabled;
		const bool halfAo = snapshot.Quality.SsaoHalfResolution || post.SsaoQuality == RenderSsaoQuality::Low;
		const bool shadows = hasCamera && ((isLit && !wireframe) || debugView == RenderDebugView::ShadowCascades);

		RenderStats stats{ .Available = true, .FrameIndex = snapshot.FrameIndex, .Width = state.Targets.Width, .Height = state.Targets.Height };
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
		recording.BeginPass("Prepare");
		ViewConstants view = Utils::MakeViewConstants(camera, stats.Width, stats.Height, isLit ? Utils::GetExposure(post) : 1.0f);
		LightCullResult culled;
		state.Lights.clear();
		if (hasCamera)
		{
			culled = CullLights(snapshot.Lights, camera);
			for (const uint32_t index : culled.Visible)
				state.Lights.push_back(Utils::MakeShaderLight(snapshot.Lights[index]));
			state.Stats.Lights = static_cast<uint32_t>(culled.Visible.size());
			state.Stats.CulledLights = culled.Culled;
			state.Stats.DroppedLights = culled.Dropped;
			if (culled.Dropped > 0 && !state.LightLimitLogged)
			{
				state.LightLimitLogged = true;
				ENGINE_CORE_WARN("{}: {} lights exceed the view's budget of {} (logged once per view)", RenderLightLimitExceededCode, culled.Dropped, MaxVisibleLights);
			}
		}
		view.LightCount = static_cast<uint32_t>(state.Lights.size());
		ShadowCascadeSet cascades;
		SpotShadowAtlas spots;
		ShadowConstants shadowConstants{};
		if (shadows)
		{
			// Choose in snapshot order, independently of CullLights' importance ordering. Uploaded indices are remapped.
			for (uint32_t index = 0; index < snapshot.Lights.size(); ++index)
			{
				const LightData& light = snapshot.Lights[index];
				const auto visible = std::find(culled.Visible.begin(), culled.Visible.end(), index);
				if (light.Type != RenderLightType::Directional || !light.CastShadows || visible == culled.Visible.end())
					continue;
				auto planned = BuildShadowCascades(camera, light, index, snapshot.Quality.ShadowMapSize);
				if (!planned)
					keepFirstError(std::unexpected(std::move(planned).error()));
				else
				{
					cascades = *planned;
					shadowConstants.Counts.x = cascades.Count;
					shadowConstants.Counts.z = static_cast<uint32_t>(visible - culled.Visible.begin());
					shadowConstants.Directional = glm::vec4(light.LightAngle * std::numbers::pi_v<float> / 180.0f, light.DepthBias, light.NormalBias, light.ShadowDistance);
					for (uint32_t cascade = 0; cascade < cascades.Count; ++cascade)
					{
						const ShadowCascade& plan = cascades.Cascades[cascade];
						shadowConstants.CascadeViewProjection[cascade] = plan.ViewProjection;
						shadowConstants.CascadeSplits[cascade] = plan.SplitFar;
						shadowConstants.CascadeBlendStarts[cascade] = plan.BlendStart;
						shadowConstants.CascadeTexelWorldSizes[cascade] = plan.TexelWorldSize;
						shadowConstants.CascadeDepthRanges[cascade] = glm::vec4(plan.LightNear, plan.LightFar, plan.PenumbraUvPerMetre, 0.05f);
					}
				}
				break;
			}
			auto allocated = AllocateSpotShadowAtlas(snapshot.Lights, culled.Visible, camera);
			if (!allocated)
				keepFirstError(std::unexpected(std::move(allocated).error()));
			else
				spots = std::move(*allocated);
			shadowConstants.Counts.y = static_cast<uint32_t>(spots.Tiles.size());
			for (size_t index = 0; index < spots.Tiles.size(); ++index)
			{
				const SpotShadowTile& tile = spots.Tiles[index];
				const LightData& light = snapshot.Lights[tile.LightIndex];
				SpotShadowConstants& constants = shadowConstants.Spots[index];
				constants.ViewProjection = tile.ViewProjection;
				constants.UvScaleBias = tile.UvScaleBias;
				constants.DepthSoftness = glm::vec4(tile.NearClip, tile.FarClip, light.SourceRadius, 0.05f * tile.UvScaleBias.x);
				const float projectionScale = glm::length(glm::vec3(tile.ViewProjection[0][0], tile.ViewProjection[1][0], tile.ViewProjection[2][0]));
				constants.Bias = glm::vec4(light.DepthBias, light.NormalBias, 2.0f / (projectionScale * static_cast<float>(SpotShadowTileSize - 2 * SpotShadowGuardTexels)), 0.0f);
				constants.Indices.x = static_cast<uint32_t>(std::find(culled.Visible.begin(), culled.Visible.end(), tile.LightIndex) - culled.Visible.begin());
			}
			if (spots.Dropped > 0 && !state.SpotLimitLogged)
			{
				state.SpotLimitLogged = true;
				ENGINE_CORE_WARN("{}: {} visible spot shadows exceed the budget of {} (logged once per view)", RenderSpotShadowBudgetCode, spots.Dropped, MaxSpotShadowLights);
			}
		}
		state.OpaqueDraws.clear();
		state.TransparentDraws.clear();
		if (hasCamera)
			keepFirstError(state.PrepareDraws(snapshot, camera));
		const GpuEnvironment* environment = hasCamera && isLit && !wireframe ? state.Cache->GetEnvironment(snapshot.Environment.Environment) : nullptr;
		const EnvironmentConstants environmentConstants = Utils::MakeEnvironmentConstants(snapshot.Environment, environment);
		const nvrhi::TextureHandle environmentSpecular = environment != nullptr ? environment->Specular : pipelines.BlackCube;
		const nvrhi::TextureHandle environmentSkybox = environment != nullptr ? environment->Skybox : nvrhi::TextureHandle();
		nvrhi::TextureHandle blueNoise;
		if (isLit && state.Assets->GetAssetType(BuiltinAssetHandles::BlueNoiseTexture) == AssetType::Texture)
		{
			const GpuTexture& noise = state.Cache->GetTexture(BuiltinAssetHandles::BlueNoiseTexture);
			if (!noise.IsPlaceholder)
				blueNoise = noise.Texture;
		}
		recording.EndPass({});
		ENGINE_TRY(state.EnsureFrameTargets(picking, ao, halfAo, cascades.Count != 0 ? snapshot.Quality.ShadowMapSize : 0, !spots.Tiles.empty(), selection, overdraw));
		const Detail::SceneRendererTargets& targets = state.Targets;
		state.PendingSubmission = true;
		state.ImageFrame = snapshot.FrameIndex;
		state.ImageRevision = snapshot.SceneRevision;
		state.ImageGeneration = state.Generation;
		state.ImagePickTable = picking ? snapshot.PickTable : std::vector<UUID>();
		state.FinalTarget = 0;
		commandList.beginMarker("SceneRenderer");
		commandList.writeBuffer(state.ViewConstantsBuffer, &view, sizeof(view));
		commandList.writeBuffer(state.EnvironmentConstantsBuffer, &environmentConstants, sizeof(environmentConstants));
		commandList.writeBuffer(state.ShadowConstantsBuffer, &shadowConstants, sizeof(shadowConstants));
		if (!state.Lights.empty())
			commandList.writeBuffer(state.LightBuffer, state.Lights.data(), state.Lights.size() * sizeof(ShaderLight));
		const glm::vec3 clearColor = camera.ClearColor;
		commandList.clearTextureFloat(targets.SceneColor, nvrhi::AllSubresources, nvrhi::Color(clearColor.r, clearColor.g, clearColor.b, 1.0f));
		commandList.clearDepthStencilTexture(targets.SceneDepth, nvrhi::AllSubresources, true, 0.0f, false, 0);
		commandList.clearTextureFloat(targets.SceneNormals, nvrhi::AllSubresources, nvrhi::Color(0.0f));
		if (picking)
		{
			commandList.clearTextureUInt(targets.EntityId, nvrhi::AllSubresources, 0);
			state.RecordedPicking = true;
		}
		if (cascades.Count != 0 || !spots.Tiles.empty())
		{
			std::vector<MeshDrawItem> finiteCasters;
			for (const MeshDrawItem& item : snapshot.Meshes)
				if (Utils::IsFinite(item.World))
					finiteCasters.push_back(item);
			const ShadowRenderInputs inputs{
				.Casters = finiteCasters,
				.Cascades = cascades.Count != 0 ? &cascades : nullptr,
				.Spots = !spots.Tiles.empty() ? &spots : nullptr,
				.CascadeTarget = cascades.Count != 0 ? targets.Cascades.Get() : nullptr,
				.AtlasTarget = !spots.Tiles.empty() ? targets.SpotAtlas.Get() : nullptr,
				.DepthBias = cascades.Count != 0 ? snapshot.Lights[cascades.LightIndex].DepthBias : 1.0f,
				.NormalBias = cascades.Count != 0 ? snapshot.Lights[cascades.LightIndex].NormalBias : 1.0f,
			};
			auto result = pipelines.Shadows->Record(commandList, recording, state.ShadowBindings, *state.Cache, *state.Assets, inputs);
			if (!result)
				keepFirstError(std::unexpected(std::move(result).error()));
		}
		nvrhi::IBindingSet* prepassSet = nullptr;
		if (hasCamera)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(Utils::ViewConstantsRegister, state.ViewConstantsBuffer),
				nvrhi::BindingSetItem::Sampler(Utils::AnisoWrapRegister, pipelines.AnisoWrap),
				nvrhi::BindingSetItem::PushConstants(Utils::DrawConstantsSlot, sizeof(DrawConstants)),
			};
			auto created = state.ViewBindings.GetOrCreate(*state.Device, desc, *pipelines.Layouts.PrepassView);
			if (created)
				prepassSet = *created;
			else
				keepFirstError(std::unexpected(std::move(created).error()));
		}
		if (prepassSet != nullptr && (!overdraw || picking))
		{
			recording.BeginPass("DepthNormal", &commandList);
			RenderPassCounters counters;
			state.RecordMeshPass(commandList, picking ? Detail::SceneRendererMeshPass::Picking : Detail::SceneRendererMeshPass::Prepass,
				state.OpaqueDraws, *prepassSet, debugView, counters);
			recording.EndPass(counters);
		}
		nvrhi::ITexture* occlusion = pipelines.WhiteAo;
		nvrhi::ITexture* linearDepth = pipelines.FarDepth;
		if (ao)
		{
			const Status depth = pipelines.DepthPyramid->Record(commandList, recording, state.DepthBindings, { .SceneDepth = targets.SceneDepth, .ViewDepth = targets.ViewDepth, .Camera = camera });
			keepFirstError(depth);
			if (depth)
			{
				auto result = pipelines.Gtao->Record(commandList, recording, state.GtaoBindings, { .Camera = camera, .Post = post, .HalfResolution = halfAo, .ViewDepth = targets.ViewDepth, .SceneNormals = targets.SceneNormals, .Occlusion = targets.Occlusion, .Scratch = targets.AoScratch });
				if (result)
				{
					occlusion = *result;
					linearDepth = targets.ViewDepth;
				}
				else
					keepFirstError(std::unexpected(std::move(result).error()));
			}
		}
		if (hasCamera && !dataView && !wireframe)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(Utils::ViewConstantsRegister, state.ViewConstantsBuffer),
				nvrhi::BindingSetItem::ConstantBuffer(Utils::ShadowConstantsRegister, state.ShadowConstantsBuffer),
				nvrhi::BindingSetItem::ConstantBuffer(Utils::EnvironmentConstantsRegister, state.EnvironmentConstantsBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(Utils::LightsRegister, state.LightBuffer),
				nvrhi::BindingSetItem::Texture_SRV(Utils::ShadowCascadesRegister, cascades.Count != 0 ? targets.Cascades : pipelines.EmptyCascades),
				nvrhi::BindingSetItem::Texture_SRV(Utils::ShadowAtlasRegister, !spots.Tiles.empty() ? targets.SpotAtlas : pipelines.EmptyAtlas),
				nvrhi::BindingSetItem::Texture_SRV(Utils::EnvSpecularRegister, environmentSpecular),
				nvrhi::BindingSetItem::Texture_SRV(Utils::BrdfLutRegister, pipelines.DfgLut->GetTexture()),
				nvrhi::BindingSetItem::Texture_SRV(Utils::AmbientOcclusionRegister, occlusion),
				nvrhi::BindingSetItem::Texture_SRV(Utils::ViewDepthRegister, linearDepth),
				nvrhi::BindingSetItem::Texture_SRV(Utils::SceneNormalsRegister, targets.SceneNormals),
				nvrhi::BindingSetItem::Sampler(Utils::LinearClampRegister, pipelines.LinearClamp),
				nvrhi::BindingSetItem::Sampler(Utils::ShadowCompareRegister, pipelines.ShadowCompare),
				nvrhi::BindingSetItem::Sampler(Utils::AnisoWrapRegister, pipelines.AnisoWrap),
				nvrhi::BindingSetItem::PushConstants(Utils::DrawConstantsSlot, sizeof(DrawConstants)),
			};
			auto forwardSet = state.ViewBindings.GetOrCreate(*state.Device, desc, *pipelines.Layouts.ForwardView);
			if (!forwardSet)
				keepFirstError(std::unexpected(std::move(forwardSet).error()));
			else
			{
				if (!state.OpaqueDraws.empty())
				{
					recording.BeginPass("ForwardOpaque", &commandList);
					RenderPassCounters counters;
					state.RecordMeshPass(commandList, Detail::SceneRendererMeshPass::ForwardOpaque, state.OpaqueDraws, **forwardSet, debugView, counters);
					recording.EndPass(counters);
				}
				if (isLit && camera.ClearToSkybox && environmentSkybox != nullptr && snapshot.Environment.ShowSkybox)
				{
					recording.BeginPass("Skybox", &commandList);
					const Status drawn = pipelines.Skybox->Record(commandList, state.SkyboxBindings, { .Framebuffer = targets.ForwardFramebuffer, .ViewConstants = state.ViewConstantsBuffer, .EnvironmentConstants = state.EnvironmentConstantsBuffer, .Skybox = environmentSkybox });
					recording.EndPass({ .DrawCalls = drawn ? 1U : 0U, .Triangles = drawn ? 1U : 0U });
					keepFirstError(drawn);
				}
				if (!state.TransparentDraws.empty())
				{
					recording.BeginPass("ForwardTransparent", &commandList);
					RenderPassCounters counters;
					state.RecordMeshPass(commandList, Detail::SceneRendererMeshPass::ForwardTransparent, state.TransparentDraws, **forwardSet, debugView, counters);
					recording.EndPass(counters);
				}
			}
		}
		if (overdraw)
		{
			recording.BeginPass("Overdraw", &commandList);
			RenderPassCounters counters;
			commandList.clearTextureFloat(targets.Overdraw, nvrhi::AllSubresources, nvrhi::Color(0.0f));
			if (prepassSet != nullptr)
			{
				state.RecordMeshPass(commandList, Detail::SceneRendererMeshPass::Overdraw, state.OpaqueDraws, *prepassSet, debugView, counters);
				state.RecordMeshPass(commandList, Detail::SceneRendererMeshPass::Overdraw, state.TransparentDraws, *prepassSet, debugView, counters);
			}
			recording.EndPass(counters);
		}
		nvrhi::ITexture* bloom = nullptr;
		const float bloomIntensity = std::isfinite(post.BloomIntensity) ? std::clamp(post.BloomIntensity, 0.0f, 1.0f) : 0.0f;
		if (isLit && !wireframe && post.BloomEnabled && bloomIntensity > 0.0f)
		{
			recording.BeginPass("Bloom", &commandList);
			RenderPassCounters counters;
			auto result = pipelines.Bloom->RecordCounted(commandList, state.BloomBindings, { .SceneColor = targets.SceneColor, .Chain = targets.Bloom }, counters);
			if (result)
				bloom = *result;
			else
				keepFirstError(std::unexpected(std::move(result).error()));
			recording.EndPass(counters);
		}
		// Dedicated data views bypass all tone/color transforms; Tonemap names the final output stage in every view.
		recording.BeginPass("Tonemap", &commandList);
		Status encoded;
		if (dataView)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, state.ViewConstantsBuffer),
				nvrhi::BindingSetItem::ConstantBuffer(1, state.ShadowConstantsBuffer),
				nvrhi::BindingSetItem::Texture_SRV(0, targets.SceneDepth),
				nvrhi::BindingSetItem::Texture_SRV(1, occlusion),
				nvrhi::BindingSetItem::Texture_SRV(2, linearDepth),
				nvrhi::BindingSetItem::Texture_SRV(3, targets.SceneNormals),
				nvrhi::BindingSetItem::Texture_SRV(4, overdraw ? targets.Overdraw : targets.SceneColor),
				nvrhi::BindingSetItem::Texture_UAV(0, targets.Ldr[0]),
				nvrhi::BindingSetItem::PushConstants(3, sizeof(SceneDataViewConstants)),
			};
			auto set = state.DataViewBindings.GetOrCreate(*state.Device, desc, *pipelines.DataView.BindingLayouts[0]);
			if (!set)
				encoded = std::unexpected(std::move(set).error());
			else
			{
				nvrhi::ComputeState compute;
				compute.pipeline = pipelines.DataView.Pipeline;
				compute.bindings = { *set };
				commandList.setComputeState(compute);
				const SceneDataViewConstants constants{ .Options = glm::uvec4(static_cast<uint32_t>(debugView), 0, 0, 0) };
				commandList.setPushConstants(&constants, sizeof(constants));
				commandList.dispatch((targets.Width + 7) / 8, (targets.Height + 7) / 8);
			}
		}
		else
		{
			encoded = pipelines.Tonemap->Record(commandList, state.TonemapBindings, { .ViewConstants = state.ViewConstantsBuffer, .SceneColor = targets.SceneColor, .Bloom = bloom, .BloomIntensity = bloom != nullptr ? bloomIntensity : 0.0f, .BlueNoise = blueNoise, .LdrColor = targets.Ldr[0], .Settings = { .Tonemapper = isLit ? post.Tonemap : RenderTonemapper::Linear, .Dither = isLit && blueNoise != nullptr, .EncodeSrgb = isLit || debugView == RenderDebugView::Albedo || debugView == RenderDebugView::Emissive } });
		}
		recording.EndPass({ .Dispatches = encoded ? 1U : 0U });
		keepFirstError(encoded);
		if (isLit && !wireframe && post.FxaaEnabled)
		{
			recording.BeginPass("FXAA", &commandList);
			auto result = pipelines.Fxaa->Record(commandList, state.FxaaBindings, { .Source = targets.Ldr[0], .Destination = targets.Ldr[1] });
			if (result)
				state.FinalTarget = 1;
			else
				keepFirstError(std::unexpected(std::move(result).error()));
			recording.EndPass({ .Dispatches = result ? 1U : 0U });
		}
		nvrhi::IFramebuffer* overlay = targets.OverlayFramebuffers[state.FinalTarget];
		const auto drawLines = [&state, &pipelines, &commandList, &recording, &keepFirstError, overlay](std::string_view name, const DebugDrawList& lines)
		{
			if (std::none_of(lines.GetCommands().begin(), lines.GetCommands().end(), [](const DebugDrawCommand& command)
			{
				return !std::holds_alternative<DebugText>(command.Shape);
			}))
				return;
			recording.BeginPass(name, &commandList);
			RenderPassCounters counters;
			auto result = pipelines.Debug->RecordCounted(commandList, state.DebugBindings, { .DebugDraw = &lines, .Framebuffer = overlay, .ViewConstants = state.ViewConstantsBuffer }, counters);
			if (result)
				state.Stats.DebugLineVertices += *result;
			else
				keepFirstError(std::unexpected(std::move(result).error()));
			recording.EndPass(counters);
		};
		std::optional<RenderSnapshot> resizedOverlaySnapshot;
		if (hasCamera && !dataView && (snapshot.Camera.ViewportWidth != targets.Width || snapshot.Camera.ViewportHeight != targets.Height))
		{
			resizedOverlaySnapshot = snapshot;
			resizedOverlaySnapshot->Camera = camera;
		}
		const RenderSnapshot& overlaySnapshot = resizedOverlaySnapshot ? *resizedOverlaySnapshot : snapshot;
		if (hasCamera && wireframe)
		{
			DebugDrawList edges;
			keepFirstError(AppendWireframeOverlay(overlaySnapshot, *state.Assets, edges));
			drawLines("Wireframe", edges);
		}
		if (hasCamera && selection)
			keepFirstError(pipelines.Selection->Record(commandList, recording, state.SelectionBindings, *state.Cache, *state.Assets, { .Snapshot = &snapshot, .SceneDepth = targets.SceneDepth, .Mask = targets.SelectionMask, .Scratch = targets.SelectionScratch, .LdrColor = targets.Ldr[state.FinalTarget] }));
		const bool annotations = snapshot.Annotations.Labels != RenderAnnotationLabels::None || snapshot.Annotations.Bounds || snapshot.Annotations.Axes
			|| HasFlag(snapshot.Flags, RenderViewFlags::Colliders);
		DebugDrawList overlays;
		if (!dataView || annotations)
			overlays.Append(snapshot.DebugDraw);
		if (hasCamera && !dataView)
			keepFirstError(AppendEditorOverlay(overlaySnapshot, overlays));
		if (hasCamera)
			drawLines("Overlays", overlays);
		const std::span<const TextItem> texts = dataView ? std::span<const TextItem>() : std::span<const TextItem>(snapshot.Texts);
		const bool hasLabels = std::any_of(overlays.GetCommands().begin(), overlays.GetCommands().end(), [](const DebugDrawCommand& command)
		{
			return std::holds_alternative<DebugText>(command.Shape);
		});
		if (!texts.empty() || (hasCamera && hasLabels))
		{
			recording.BeginPass("Text", &commandList);
			RenderPassCounters counters;
			auto result = pipelines.Text->RecordCounted(commandList, state.TextBindings, { .Texts = texts, .DebugDraw = &overlays, .Assets = state.Assets, .Framebuffer = overlay, .ViewConstants = state.ViewConstantsBuffer, .HasCamera = hasCamera, .CameraView = hasCamera ? std::optional<glm::mat4>(camera.View) : std::nullopt }, counters);
			if (result)
				state.Stats.TextDraws = *result;
			else
				keepFirstError(std::unexpected(std::move(result).error()));
			recording.EndPass(counters);
		}
		commandList.endMarker();
		state.ReleaseUnusedBindings();
		std::set<size_t> visibleMeshes;
		for (const auto& draw : state.OpaqueDraws)
			visibleMeshes.insert(draw.SnapshotIndex);
		for (const auto& draw : state.TransparentDraws)
			visibleMeshes.insert(draw.SnapshotIndex);
		stats.VisibleMeshes = static_cast<uint32_t>(visibleMeshes.size());
		stats.CulledSubmeshes = state.Stats.CulledSubmeshes;
		stats.ShadowedSpotLights = static_cast<uint32_t>(spots.Tiles.size());
		stats.DroppedSpotShadows = spots.Dropped;
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
		constexpr std::array<std::string_view, 22> Order = { "Prepare", "DirectionalShadows", "SpotShadows", "DepthNormal", "DepthPyramid", "GTAO", "GTAODenoiseHorizontal", "GTAODenoiseVertical", "ForwardOpaque", "Skybox", "ForwardTransparent", "Overdraw", "Bloom", "Tonemap", "FXAA", "SelectionMask", "SelectionDilateHorizontal", "SelectionDilateVertical", "SelectionComposite", "Wireframe", "Overlays", "Text" };
		std::stable_sort(stats.Passes.begin(), stats.Passes.end(), [&Order](const RenderPassStats& left, const RenderPassStats& right)
		{
			return std::find(Order.begin(), Order.end(), left.Name) < std::find(Order.begin(), Order.end(), right.Name);
		});
		state.History.PublishCpu(std::move(stats));
		state.History.PublishGpu(completedTiming);
		return status;
	}

	nvrhi::ITexture* SceneRenderer::GetFinalTexture() const
	{
		return m_State->Targets.Ldr[m_State->FinalTarget].Get();
	}

	uint32_t SceneRenderer::GetWidth() const
	{
		return m_State->Targets.Width;
	}

	uint32_t SceneRenderer::GetHeight() const
	{
		return m_State->Targets.Height;
	}

	const SceneRenderStats& SceneRenderer::GetLastStats() const
	{
		return m_State->Stats;
	}

}
