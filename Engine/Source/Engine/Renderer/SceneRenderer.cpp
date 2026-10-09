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
			+ FxaaPass::PipelineCount + DebugRenderer::PipelineCount + TextRenderer::PipelineCount + BrdfLut::PipelineCount);
	static_assert(SceneRendererPipelines::MeshPipelineCount == Utils::PrepassVariantCount + Utils::ForwardVariantCount);
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
		static Result<nvrhi::TextureHandle> CreateTarget(GraphicsDevice& device, uint32_t width, uint32_t height, nvrhi::Format format,
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
			return device.CreateTexture(desc);
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

		// Appends `descriptions` to `all`.
		static void AppendDescriptions(std::vector<PipelineLayoutDescription>& all, std::vector<PipelineLayoutDescription> descriptions)
		{
			all.insert(all.end(), std::make_move_iterator(descriptions.begin()), std::make_move_iterator(descriptions.end()));
		}

	}

	namespace {

		// One submesh draw of the frame, resolved by pass 1.
		struct SceneDraw
		{
			nvrhi::IBuffer* VertexBuffer = nullptr; // held by the GpuResourceCache for the render
			nvrhi::IBuffer* IndexBuffer = nullptr;
			uint32_t IndexOffset = 0;
			uint32_t IndexCount = 0;
			nvrhi::IBindingSet* Material = nullptr; // held by the renderer's material cache for the render
			uint32_t ForwardVariant = 0;
			uint32_t PrepassVariant = 0; // opaque draws only
			DrawConstants Constants{};
			// Sort keys (§8.3 pass 1).
			AssetHandle MaterialHandle{};
			AssetHandle Mesh{};
			UUID Entity{};
			uint32_t Submesh = 0;
			float ViewDepth = 0.0f; // transparent draws: the view depth of the submesh bounds' centre
		};

		// A material of the frame, resolved once per render: its set, or null when its draws are skipped.
		struct ResolvedMaterial
		{
			nvrhi::IBindingSet* Set = nullptr;
			Engine::AlphaMode Mode = Engine::AlphaMode::Opaque; // the type is spelled Engine:: (GCC's "changes meaning")
			bool DoubleSided = false;
		};

		// Which mesh pass RecordMeshPass records.
		enum class MeshPass : uint8_t
		{
			Prepass,
			ForwardOpaque,
			ForwardTransparent
		};

		// The size-dependent targets of a renderer.
		struct SceneTargets
		{
			uint32_t Width = 0;
			uint32_t Height = 0;
			nvrhi::TextureHandle SceneDepth{};
			nvrhi::TextureHandle SceneNormals{};
			nvrhi::TextureHandle SceneColor{};
			std::array<nvrhi::TextureHandle, 2> Ldr{}; // LdrColor and its FXAA ping-pong partner
			nvrhi::TextureHandle Bloom{};              // BloomPass::GetChainDesc
			nvrhi::FramebufferHandle PrepassFramebuffer{};
			nvrhi::FramebufferHandle ForwardFramebuffer{};
			std::array<nvrhi::FramebufferHandle, 2> OverlayFramebuffers{}; // each LDR target with SceneDepth (tested, not written)
		};

	}

	struct SceneRendererPipelines::State
	{
		GraphicsDevice* Device = nullptr;   // documented back-reference
		PipelineFactory* Factory = nullptr; // documented back-reference (EnsureDebugView)
		Utils::MeshBindingLayouts Layouts{};
		std::array<GraphicsPipeline, Utils::PrepassVariantCount> Prepass{};
		// The forward variants of each debug view, indexed by RenderDebugView (Lit's at startup, the others by
		// EnsureDebugView).
		std::array<std::array<GraphicsPipeline, Utils::ForwardVariantCount>, RenderDebugViewCount> Forward{};
		std::array<bool, RenderDebugViewCount> HasView{};
		nvrhi::SamplerHandle LinearClamp{};
		nvrhi::SamplerHandle AnisoWrap{};
		nvrhi::TextureHandle BlackCube{};
		// The passes the set owns (SceneRenderer.h).
		Scope<SkyboxPass> Skybox;
		Scope<BloomPass> Bloom;
		Scope<TonemapPass> Tonemap;
		Scope<FxaaPass> Fxaa;
		Scope<DebugRenderer> Debug;
		Scope<TextRenderer> Text;
		Scope<BrdfLut> DfgLut;
		uint32_t PipelineCount = 0;
	};

	struct SceneRenderer::State
	{
		// Documented back-references.
		GraphicsDevice* Device = nullptr;
		SceneRendererPipelines* PipelineSet = nullptr; // EnsureDebugView
		SceneRendererPipelines::State* Pipelines = nullptr;
		GpuResourceCache* Cache = nullptr;
		AssetManager* Assets = nullptr;

		// The view's binding sets (PassBindingCache.h): its own set-0 sets, those of each shared pass, the materials'.
		PassBindingCache ViewBindings{};
		PassBindingCache SkyboxBindings{};
		PassBindingCache BloomBindings{};
		PassBindingCache TonemapBindings{};
		PassBindingCache FxaaBindings{};
		PassBindingCache DebugBindings{};
		PassBindingCache TextBindings{};
		Utils::MaterialBindingCache Materials{};

		SceneTargets Targets{};
		size_t FinalTarget = 0; // the index of the LDR target holding the last image
		nvrhi::BufferHandle ViewConstantsBuffer{};
		nvrhi::BufferHandle EnvironmentConstantsBuffer{};
		nvrhi::BufferHandle LightBuffer{};
		// Reused by every Render.
		std::vector<SceneDraw> OpaqueDraws{};
		std::vector<SceneDraw> TransparentDraws{};
		std::vector<ShaderLight> Lights{};
		bool LightLimitLogged = false; // RENDER_LIGHT_LIMIT_EXCEEDED is logged once per renderer
		SceneRenderStats Stats{};

		// The targets of `width` x `height` with their framebuffers.
		[[nodiscard]] Result<SceneTargets> CreateTargets(uint32_t width, uint32_t height) const;
		// Clears every binding-set cache that references the targets.
		void ClearTargetBindings();
		// The material `handle` of this render: its mirror's set, created or found in the material cache; an empty
		// ResolvedMaterial (no set: its draws are skipped) when the mirror lacks GPU objects, which the cache reported.
		// Errors: those of the material cache's GetOrCreate (Gpu), with the material named.
		[[nodiscard]] Result<ResolvedMaterial> ResolveMaterial(AssetHandle handle);
		// Pass 1: the draws of `snapshot` seen by `camera` into OpaqueDraws and TransparentDraws, culled, resolved and sorted.
		// Returns the first non-finite world matrix's error, after skipping that draw, or a material set's creation error.
		[[nodiscard]] Status PrepareDraws(const RenderSnapshot& snapshot, const CameraData& camera);
		// Passes 4, 7 and 9: `draws` with the pipelines of `pass` (the forward ones of `view`) and the view set `viewSet`.
		void RecordMeshPass(nvrhi::ICommandList& commandList, MeshPass pass, std::span<const SceneDraw> draws, nvrhi::IBindingSet& viewSet,
			RenderDebugView view);
		// Drops what the render did not use from every binding-set cache.
		void ReleaseUnusedBindings();
	};

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
		}
		const auto lit = static_cast<size_t>(RenderDebugView::Lit);
		for (uint32_t variant = 0; variant < Utils::ForwardVariantCount; ++variant)
		{
			ENGINE_TRY_ASSIGN(state.Forward[lit][variant], Utils::CreateForwardPipeline(pipelines, state.Layouts, variant, RenderDebugView::Lit));
		}
		state.HasView[lit] = true;

		ENGINE_TRY_ASSIGN(state.LinearClamp,
			device.CreateSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp)));
		ENGINE_TRY_ASSIGN(state.AnisoWrap, device.CreateSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap).setMaxAnisotropy(Utils::MaxSamplerAnisotropy)));
		ENGINE_TRY_ASSIGN(state.BlackCube, Utils::CreateBlackCube(device));

		ENGINE_TRY_ASSIGN(state.Skybox, SkyboxPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(state.Bloom, BloomPass::Create(device, pipelines, device.GetInfo().BloomFormat));
		ENGINE_TRY_ASSIGN(state.Tonemap, TonemapPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(state.Fxaa, FxaaPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(state.Debug, DebugRenderer::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(state.Text, TextRenderer::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(state.DfgLut, BrdfLut::Create(device, pipelines));
		state.PipelineCount = MeshPipelineCount + state.Skybox->GetPipelineCount() + state.Bloom->GetPipelineCount() + state.Tonemap->GetPipelineCount()
			+ state.Fxaa->GetPipelineCount() + state.Debug->GetPipelineCount() + state.Text->GetPipelineCount() + state.DfgLut->GetPipelineCount();
		ENGINE_CORE_INFO("Created the scene renderer's {} pipelines", state.PipelineCount);
		return set;
	}

	Status SceneRendererPipelines::EnsureDebugView(RenderDebugView view)
	{
		if (view >= RenderDebugView::AO && view <= RenderDebugView::Overdraw)
		{
			ENGINE_CONTRACT_STUB();
			return std::unexpected(Error(ErrorCode::Unsupported, "M9 debug rendering is not implemented"));
		}
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
		return descriptions;
	}

	void SceneRendererPipelines::CollectStale(const AssetManager& assets, bool releaseUnused)
	{
		m_State->Text->CollectStale(assets, releaseUnused);
	}

	Result<SceneTargets> SceneRenderer::State::CreateTargets(uint32_t width, uint32_t height) const
	{
		GraphicsDevice& device = *Device;
		SceneTargets targets;
		targets.Width = width;
		targets.Height = height;
		ENGINE_TRY_ASSIGN(targets.SceneDepth,
			Utils::CreateTarget(device, width, height, SceneDepthFormat, Utils::TargetUsage::Attachment, "SceneDepth"));
		ENGINE_TRY_ASSIGN(targets.SceneNormals,
			Utils::CreateTarget(device, width, height, SceneNormalsFormat, Utils::TargetUsage::Attachment, "SceneNormals"));
		ENGINE_TRY_ASSIGN(targets.SceneColor,
			Utils::CreateTarget(device, width, height, SceneColorFormat, Utils::TargetUsage::Attachment, "SceneColor"));
		ENGINE_TRY_ASSIGN(targets.Ldr[0], Utils::CreateTarget(device, width, height, LdrColorFormat, Utils::TargetUsage::LdrColor, "LdrColor"));
		ENGINE_TRY_ASSIGN(targets.Ldr[1], Utils::CreateTarget(device, width, height, LdrColorFormat, Utils::TargetUsage::LdrColor, "LdrColorFxaa"));
		ENGINE_TRY_ASSIGN(targets.Bloom, device.CreateTexture(BloomPass::GetChainDesc(width, height, Pipelines->Bloom->GetFormat())));

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

	void SceneRenderer::State::ClearTargetBindings()
	{
		ViewBindings.Clear();
		SkyboxBindings.Clear();
		BloomBindings.Clear();
		TonemapBindings.Clear();
		FxaaBindings.Clear();
		DebugBindings.Clear();
		TextBindings.Clear();
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
		Materials.ReleaseUnused();
	}

	Result<ResolvedMaterial> SceneRenderer::State::ResolveMaterial(AssetHandle handle)
	{
		// The mirror is valid until the next upload: everything needed is taken now.
		const GpuMaterial& material = Cache->GetMaterial(handle);
		if (material.Constants == nullptr || material.BaseColorMap == nullptr || material.MetallicRoughnessMap == nullptr || material.NormalMap == nullptr
			|| material.OcclusionMap == nullptr || material.EmissiveMap == nullptr)
			return ResolvedMaterial{}; // not even a placeholder could be created (reported by the cache)
		Result<nvrhi::IBindingSet*> set = Materials.GetOrCreate(*Device, handle, material, *Pipelines->Layouts.Material);
		if (!set.has_value())
			return std::unexpected(std::move(set).error().WithContext(std::format("while binding material {}", handle)));
		return ResolvedMaterial{ .Set = *set, .Mode = material.AlphaMode, .DoubleSided = material.DoubleSided };
	}

	Status SceneRenderer::State::PrepareDraws(const RenderSnapshot& snapshot, const CameraData& camera)
	{
		OpaqueDraws.clear();
		TransparentDraws.clear();
		Status status;
		std::map<AssetHandle, ResolvedMaterial> materials;
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
					Result<ResolvedMaterial> resolved = ResolveMaterial(materialHandle);
					if (!resolved.has_value() && status.has_value())
						status = std::unexpected(std::move(resolved).error());
					found = materials.emplace(materialHandle, resolved.value_or(ResolvedMaterial{})).first;
				}
				const ResolvedMaterial& material = found->second;
				if (material.Set == nullptr)
					continue; // its GPU objects are missing (reported)

				const Utils::MeshCullMode cullMode = Utils::SelectCullMode(mirrored, material.DoubleSided);
				SceneDraw draw;
				draw.VertexBuffer = vertexBuffer;
				draw.IndexBuffer = indexBuffer;
				draw.IndexOffset = submesh.IndexOffset;
				draw.IndexCount = submesh.IndexCount;
				draw.Material = material.Set;
				draw.ForwardVariant = Utils::GetForwardVariant(material.Mode, cullMode);
				draw.Constants.World = item.World;
				draw.Constants.EntityId = static_cast<uint32_t>(index + 1);
				draw.Constants.Flags = mirrored ? DrawFlagMirrored : 0U;
				draw.MaterialHandle = materialHandle;
				draw.Mesh = item.Mesh;
				draw.Entity = item.Entity;
				draw.Submesh = submeshIndex;
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
			const SceneDraw& draw = OpaqueDraws[index];
			opaqueKeys.push_back({ .Pipeline = draw.ForwardVariant, .Material = draw.MaterialHandle, .Mesh = draw.Mesh, .DrawIndex = index });
		}
		SortOpaqueDraws(opaqueKeys);
		std::vector<TransparentSortKey> transparentKeys;
		transparentKeys.reserve(TransparentDraws.size());
		for (uint32_t index = 0; index < TransparentDraws.size(); ++index)
		{
			const SceneDraw& draw = TransparentDraws[index];
			transparentKeys.push_back({ .ViewDepth = draw.ViewDepth, .Entity = draw.Entity, .Submesh = draw.Submesh, .DrawIndex = index });
		}
		SortTransparentDraws(transparentKeys);

		const auto reorder = []<typename Key>(std::vector<SceneDraw>& draws, const std::vector<Key>& keys)
		{
			std::vector<SceneDraw> sorted;
			sorted.reserve(draws.size());
			for (const Key& key : keys)
				sorted.push_back(draws[key.DrawIndex]);
			draws = std::move(sorted);
		};
		reorder(OpaqueDraws, opaqueKeys);
		reorder(TransparentDraws, transparentKeys);
		return status;
	}

	void SceneRenderer::State::RecordMeshPass(nvrhi::ICommandList& commandList, MeshPass pass, std::span<const SceneDraw> draws, nvrhi::IBindingSet& viewSet,
		RenderDebugView view)
	{
		nvrhi::IFramebuffer* framebuffer = pass == MeshPass::Prepass ? Targets.PrepassFramebuffer.Get() : Targets.ForwardFramebuffer.Get();
		const nvrhi::Viewport viewport(static_cast<float>(Targets.Width), static_cast<float>(Targets.Height));
		const auto& forward = Pipelines->Forward[static_cast<size_t>(view)];

		// The state of the last draw: setGraphicsState only when it changes (it also invalidates the push constants, which
		// every draw sets).
		const nvrhi::IGraphicsPipeline* lastPipeline = nullptr;
		const nvrhi::IBindingSet* lastMaterial = nullptr;
		const nvrhi::IBuffer* lastVertexBuffer = nullptr;
		for (const SceneDraw& draw : draws)
		{
			const GraphicsPipeline& pipeline = pass == MeshPass::Prepass ? Pipelines->Prepass[draw.PrepassVariant] : forward[draw.ForwardVariant];
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
		}
		if (pass != MeshPass::Prepass)
			Stats.MeshDraws += static_cast<uint32_t>(draws.size());
		if (pass == MeshPass::ForwardTransparent)
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
		ENGINE_TRY_ASSIGN(state.Targets, state.CreateTargets(specification.Width, specification.Height));
		return renderer;
	}

	Status SceneRenderer::Resize(uint32_t width, uint32_t height)
	{
		ENGINE_CORE_ASSERT(width > 0 && height > 0, "SceneRenderer::Resize needs a size of at least 1x1, got {}x{}", width, height);
		State& state = *m_State;
		if (width == state.Targets.Width && height == state.Targets.Height)
			return {};
		// Built aside first, so a failure keeps the current targets. The previous ones stay alive while a submitted command
		// list still references them (their framebuffers and binding sets are referenced by every list that used them).
		ENGINE_TRY_ASSIGN(SceneTargets targets, state.CreateTargets(width, height));
		state.Targets = std::move(targets);
		state.FinalTarget = 0;
		state.ClearTargetBindings();
		return {};
	}

	Status SceneRenderer::Render(nvrhi::ICommandList& commandList, const RenderSnapshot& snapshot)
	{
		if (snapshot.Flags != RenderViewFlags::None || !snapshot.SelectedEntities.empty() || !snapshot.Icons.empty()
			|| !snapshot.Annotations.LabelEntities.empty() || snapshot.Annotations.Labels != RenderAnnotationLabels::None || snapshot.Annotations.Bounds || snapshot.Annotations.Axes
			|| snapshot.Quality.ShadowMapSize != 2048 || snapshot.Quality.SsaoHalfResolution)
		{
			ENGINE_CONTRACT_STUB();
			return std::unexpected(Error(ErrorCode::Unsupported, "M9 render view options are not implemented"));
		}
		if (snapshot.DebugView >= RenderDebugView::AO && snapshot.DebugView <= RenderDebugView::Overdraw)
		{
			ENGINE_CONTRACT_STUB();
			return std::unexpected(Error(ErrorCode::Unsupported, "M9 debug rendering is not implemented"));
		}
		State& state = *m_State;
		SceneRendererPipelines::State& pipelines = *state.Pipelines;
		state.Stats = {};
		const SceneTargets& targets = state.Targets;
		Status status;
		const auto keepFirstError = [&status](Status result)
		{
			if (!result.has_value() && status.has_value())
				status = std::move(result);
		};

		// A debug view's pipelines are created the first time it is asked for (SceneRendererPipelines::EnsureDebugView).
		const RenderDebugView debugView = snapshot.DebugView;
		ENGINE_CORE_ASSERT(static_cast<size_t>(debugView) < RenderDebugViewCount, "unknown debug view {}", std::to_underlying(debugView));
		const bool isLit = debugView == RenderDebugView::Lit;
		if (!isLit)
		{
			const Status ensured = state.PipelineSet->EnsureDebugView(debugView);
			if (!ensured.has_value())
			{
				FatalError(FatalErrorKind::OutOfMemory,
					std::format("Cannot create the {} debug view's pipelines: {}", RenderDebugViewToString(debugView), ensured.error().ToString()));
			}
		}

		// Pass 1: Prepare. A camera whose matrices are not finite renders like no camera, and is the render's error.
		bool hasCamera = snapshot.HasCamera;
		if (hasCamera && (!Utils::IsFinite(snapshot.Camera.View) || !Utils::IsFinite(snapshot.Camera.Projection) || !Utils::IsFinite(snapshot.Camera.Position)))
		{
			status = MakeError(ErrorCode::InvalidArgument, "the camera of the render snapshot (entity {}) has a matrix that is not finite; the view is cleared",
				snapshot.Camera.Entity);
			hasCamera = false;
		}
		const PostProcessSettings& post = snapshot.Post;
		const CameraData camera = hasCamera ? snapshot.Camera : CameraData{};
		ViewConstants view = Utils::MakeViewConstants(camera, targets.Width, targets.Height, isLit ? Utils::GetExposure(post) : 1.0f);

		// The light list (§8.3 pass 1): at most MaxVisibleLights, RENDER_LIGHT_LIMIT_EXCEEDED once per renderer.
		state.Lights.clear();
		if (hasCamera)
		{
			const LightCullResult culled = CullLights(snapshot.Lights, camera);
			for (const uint32_t index : culled.Visible)
				state.Lights.push_back(Utils::MakeShaderLight(snapshot.Lights[index]));
			state.Stats.Lights = static_cast<uint32_t>(culled.Visible.size());
			state.Stats.CulledLights = culled.Culled;
			state.Stats.DroppedLights = culled.Dropped;
			if (culled.Dropped > 0 && !state.LightLimitLogged)
			{
				state.LightLimitLogged = true;
				ENGINE_CORE_WARN("{}: {} lights are visible in a view that shades at most {}; the {} least important are left out (logged once per view)",
					RenderLightLimitExceededCode, culled.Visible.size() + culled.Dropped, MaxVisibleLights, culled.Dropped);
			}
		}
		view.LightCount = static_cast<uint32_t>(state.Lights.size());

		// The environment (§8.6): its mirror, or the constant ambient. The cube handles are held for the render.
		const GpuEnvironment* environment = hasCamera ? state.Cache->GetEnvironment(snapshot.Environment.Environment) : nullptr;
		const EnvironmentConstants environmentConstants = Utils::MakeEnvironmentConstants(snapshot.Environment, environment);
		const nvrhi::TextureHandle environmentSpecular = environment != nullptr ? environment->Specular : pipelines.BlackCube;
		const nvrhi::TextureHandle environmentSkybox = environment != nullptr ? environment->Skybox : nvrhi::TextureHandle();

		// The blue noise of the dither (Lit only), when the manager has the built-in and it is not a placeholder.
		nvrhi::TextureHandle blueNoise;
		if (isLit && state.Assets->GetAssetType(BuiltinAssetHandles::BlueNoiseTexture) == AssetType::Texture)
		{
			const GpuTexture& noise = state.Cache->GetTexture(BuiltinAssetHandles::BlueNoiseTexture);
			if (!noise.IsPlaceholder)
				blueNoise = noise.Texture;
		}

		// Without a camera nothing is drawn (PrepareDraws clears both lists itself).
		state.OpaqueDraws.clear();
		state.TransparentDraws.clear();
		if (hasCamera)
			keepFirstError(state.PrepareDraws(snapshot, camera));

		commandList.beginMarker("SceneRenderer");
		commandList.writeBuffer(state.ViewConstantsBuffer, &view, sizeof(view));
		commandList.writeBuffer(state.EnvironmentConstantsBuffer, &environmentConstants, sizeof(environmentConstants));
		if (!state.Lights.empty())
			commandList.writeBuffer(state.LightBuffer, state.Lights.data(), state.Lights.size() * sizeof(ShaderLight));

		const glm::vec3 clearColor = camera.ClearColor;
		commandList.clearTextureFloat(targets.SceneColor, nvrhi::AllSubresources, nvrhi::Color(clearColor.r, clearColor.g, clearColor.b, 1.0f));
		commandList.clearDepthStencilTexture(targets.SceneDepth, nvrhi::AllSubresources, true, 0.0f, false, 0);
		commandList.clearTextureFloat(targets.SceneNormals, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
		if (hasCamera)
		{
			nvrhi::BindingSetDesc prepassDesc;
			prepassDesc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(Utils::ViewConstantsRegister, state.ViewConstantsBuffer),
				nvrhi::BindingSetItem::Sampler(Utils::AnisoWrapRegister, pipelines.AnisoWrap),
				nvrhi::BindingSetItem::PushConstants(Utils::DrawConstantsSlot, sizeof(DrawConstants)),
			};
			nvrhi::BindingSetDesc forwardDesc;
			forwardDesc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(Utils::ViewConstantsRegister, state.ViewConstantsBuffer),
				nvrhi::BindingSetItem::ConstantBuffer(Utils::EnvironmentConstantsRegister, state.EnvironmentConstantsBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(Utils::LightsRegister, state.LightBuffer),
				nvrhi::BindingSetItem::Texture_SRV(Utils::EnvSpecularRegister, environmentSpecular),
				nvrhi::BindingSetItem::Texture_SRV(Utils::BrdfLutRegister, pipelines.DfgLut->GetTexture()),
				nvrhi::BindingSetItem::Sampler(Utils::LinearClampRegister, pipelines.LinearClamp),
				nvrhi::BindingSetItem::Sampler(Utils::AnisoWrapRegister, pipelines.AnisoWrap),
				nvrhi::BindingSetItem::PushConstants(Utils::DrawConstantsSlot, sizeof(DrawConstants)),
			};
			const Result<nvrhi::IBindingSet*> prepassSet = state.ViewBindings.GetOrCreate(*state.Device, prepassDesc, *pipelines.Layouts.PrepassView);
			const Result<nvrhi::IBindingSet*> forwardSet = state.ViewBindings.GetOrCreate(*state.Device, forwardDesc, *pipelines.Layouts.ForwardView);
			if (prepassSet.has_value() && forwardSet.has_value())
			{
				// Pass 4: the depth/normal prepass (Opaque and Mask).
				commandList.beginMarker("Prepass");
				state.RecordMeshPass(commandList, MeshPass::Prepass, state.OpaqueDraws, **prepassSet, debugView);
				commandList.endMarker();

				// Pass 7: forward opaque.
				commandList.beginMarker("ForwardOpaque");
				state.RecordMeshPass(commandList, MeshPass::ForwardOpaque, state.OpaqueDraws, **forwardSet, debugView);
				commandList.endMarker();

				// Pass 8: the skybox, over every pixel the scene left at the cleared depth (Lit only).
				if (isLit && camera.ClearToSkybox && environmentSkybox != nullptr && snapshot.Environment.ShowSkybox)
				{
					const SkyboxPassInputs skybox{
						.Framebuffer = targets.ForwardFramebuffer,
						.ViewConstants = state.ViewConstantsBuffer,
						.EnvironmentConstants = state.EnvironmentConstantsBuffer,
						.Skybox = environmentSkybox,
					};
					keepFirstError(pipelines.Skybox->Record(commandList, state.SkyboxBindings, skybox));
				}

				// Pass 9: forward transparent (Blend), back to front.
				commandList.beginMarker("ForwardTransparent");
				state.RecordMeshPass(commandList, MeshPass::ForwardTransparent, state.TransparentDraws, **forwardSet, debugView);
				commandList.endMarker();
			}
			else
			{
				keepFirstError(prepassSet.has_value() ? Status() : Status(std::unexpected(prepassSet.error())));
				keepFirstError(forwardSet.has_value() ? Status() : Status(std::unexpected(forwardSet.error())));
			}
		}

		// Pass 10: bloom (Lit only).
		nvrhi::ITexture* bloom = nullptr;
		const float bloomIntensity = std::isfinite(post.BloomIntensity) ? std::clamp(post.BloomIntensity, 0.0f, 1.0f) : 0.0f;
		if (isLit && post.BloomEnabled && bloomIntensity > 0.0f)
		{
			Result<nvrhi::ITexture*> chain = pipelines.Bloom->Record(commandList, state.BloomBindings, { .SceneColor = targets.SceneColor, .Chain = targets.Bloom });
			if (chain.has_value())
				bloom = *chain;
			else
				keepFirstError(std::unexpected(std::move(chain).error()));
		}

		// Pass 11: tonemap and encode. A debug view: Linear at exposure 1, no dither, the OETF only for the colour views.
		const TonemapPassInputs tonemap{
			.ViewConstants = state.ViewConstantsBuffer,
			.SceneColor = targets.SceneColor,
			.Bloom = bloom,
			.BloomIntensity = bloom != nullptr ? bloomIntensity : 0.0f,
			.BlueNoise = blueNoise,
			.LdrColor = targets.Ldr[0],
			.Settings = {
				.Tonemapper = isLit ? post.Tonemap : RenderTonemapper::Linear,
				.Dither = isLit && blueNoise != nullptr,
				.EncodeSrgb = isLit || debugView == RenderDebugView::Albedo || debugView == RenderDebugView::Emissive,
			},
		};
		keepFirstError(pipelines.Tonemap->Record(commandList, state.TonemapBindings, tonemap));

		// Pass 12: FXAA (Lit only), into the partner target.
		state.FinalTarget = 0;
		if (isLit && post.FxaaEnabled)
		{
			Result<nvrhi::ITexture*> antialiased = pipelines.Fxaa->Record(commandList, state.FxaaBindings, { .Source = targets.Ldr[0], .Destination = targets.Ldr[1] });
			if (antialiased.has_value())
				state.FinalTarget = *antialiased == targets.Ldr[1].Get() ? 1 : 0;
			else
				keepFirstError(std::unexpected(std::move(antialiased).error()));
		}

		// Passes 13 and 14: the debug lines (world space, so with a camera only), then the texts and the debug labels.
		nvrhi::IFramebuffer* overlay = targets.OverlayFramebuffers[state.FinalTarget].Get();
		if (hasCamera && !snapshot.DebugDraw.IsEmpty())
		{
			const DebugRenderInputs lines{ .DebugDraw = &snapshot.DebugDraw, .Framebuffer = overlay, .ViewConstants = state.ViewConstantsBuffer };
			Result<uint32_t> vertices = pipelines.Debug->Record(commandList, state.DebugBindings, lines);
			if (vertices.has_value())
				state.Stats.DebugLineVertices = *vertices;
			else
				keepFirstError(std::unexpected(std::move(vertices).error()));
		}
		if (!snapshot.Texts.empty() || (hasCamera && !snapshot.DebugDraw.IsEmpty()))
		{
			const TextRenderInputs texts{
				.Texts = snapshot.Texts,
				.DebugDraw = &snapshot.DebugDraw,
				.Assets = state.Assets,
				.Framebuffer = overlay,
				.ViewConstants = state.ViewConstantsBuffer,
				.HasCamera = hasCamera,
				.CameraView = hasCamera ? std::optional<glm::mat4>(camera.View) : std::nullopt,
			};
			Result<uint32_t> drawn = pipelines.Text->Record(commandList, state.TextBindings, texts);
			if (drawn.has_value())
				state.Stats.TextDraws = *drawn;
			else
				keepFirstError(std::unexpected(std::move(drawn).error()));
		}
		commandList.endMarker();

		// The view keeps only the sets this render bound (the recorded command list holds its own references).
		state.ReleaseUnusedBindings();
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
