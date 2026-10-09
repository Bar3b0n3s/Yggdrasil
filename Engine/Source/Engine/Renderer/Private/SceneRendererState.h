#pragma once

#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/BloomPass.h"
#include "Engine/Renderer/BrdfLut.h"
#include "Engine/Renderer/DebugRenderer.h"
#include "Engine/Renderer/DepthPyramidPass.h"
#include "Engine/Renderer/FxaaPass.h"
#include "Engine/Renderer/GtaoPass.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/SelectionPass.h"
#include "Engine/Renderer/ShadowPass.h"
#include "Engine/Renderer/SkyboxPass.h"
#include "Engine/Renderer/TextRenderer.h"
#include "Engine/Renderer/TonemapPass.h"
#include "Engine/Renderer/Private/ForwardPipelines.h"
#include "Engine/Renderer/Private/MaterialBindingCache.h"
#include "Shared/DrawConstants.h"
#include "Shared/ShaderLight.h"

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace Engine {

	namespace Detail {

		// One submesh draw of the frame, resolved by pass 1.
		struct SceneRendererDraw
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
			size_t SnapshotIndex = 0;
			float ViewDepth = 0.0f; // transparent draws: the view depth of the submesh bounds' centre
		};

		// A material of the frame, resolved once per render: its set, or null when its draws are skipped.
		struct SceneRendererMaterial
		{
			nvrhi::IBindingSet* Set = nullptr;
			Engine::AlphaMode Mode = Engine::AlphaMode::Opaque; // the type is spelled Engine:: (GCC's "changes meaning")
			bool DoubleSided = false;
		};

		// Which mesh pass RecordMeshPass records.
		enum class SceneRendererMeshPass : uint8_t
		{
			Prepass,
			ForwardOpaque,
			ForwardTransparent,
			Picking,
			Overdraw
		};

		// The size-dependent targets of a renderer.
		struct SceneRendererTargets
		{
			uint32_t Width = 0;
			uint32_t Height = 0;
			nvrhi::TextureHandle SceneDepth{};
			nvrhi::TextureHandle SceneNormals{};
			nvrhi::TextureHandle SceneColor{};
			std::array<nvrhi::TextureHandle, 2> Ldr{}; // LdrColor and its FXAA ping-pong partner
			nvrhi::TextureHandle EntityId{};
			nvrhi::TextureHandle ViewDepth{};
			nvrhi::TextureHandle Occlusion{};
			nvrhi::TextureHandle AoScratch{};
			nvrhi::TextureHandle Cascades{};
			nvrhi::TextureHandle SpotAtlas{};
			nvrhi::TextureHandle SelectionMask{};
			nvrhi::TextureHandle SelectionScratch{};
			nvrhi::TextureHandle Overdraw{};
			nvrhi::FramebufferHandle PickingFramebuffer{};
			nvrhi::FramebufferHandle OverdrawFramebuffer{};
			nvrhi::TextureHandle Bloom{}; // BloomPass::GetChainDesc
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
		std::array<GraphicsPipeline, Utils::PrepassVariantCount> Picking{};
		std::array<GraphicsPipeline, Utils::MeshCullModeCount> Overdraw{};
		ComputePipeline DataView{};
		// The forward variants of each debug view, indexed by RenderDebugView (Lit's at startup, the others by
		// EnsureDebugView).
		std::array<std::array<GraphicsPipeline, Utils::ForwardVariantCount>, RenderDebugViewCount> Forward{};
		std::array<bool, RenderDebugViewCount> HasView{};
		nvrhi::SamplerHandle LinearClamp{};
		nvrhi::SamplerHandle AnisoWrap{};
		nvrhi::TextureHandle BlackCube{};
		nvrhi::TextureHandle EmptyCascades{};
		nvrhi::TextureHandle EmptyAtlas{};
		nvrhi::TextureHandle WhiteAo{};
		nvrhi::TextureHandle FarDepth{};
		nvrhi::SamplerHandle ShadowCompare{};
		Scope<ShadowPass> Shadows{};
		Scope<DepthPyramidPass> DepthPyramid{};
		Scope<GtaoPass> Gtao{};
		Scope<SelectionPass> Selection{};
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
		PassBindingCache ShadowBindings{};
		PassBindingCache DepthBindings{};
		PassBindingCache GtaoBindings{};
		PassBindingCache SelectionBindings{};
		PassBindingCache DataViewBindings{};
		Scope<RenderTargetPool> TargetPool{};
		Scope<AsyncPicker> Picker{};
		Scope<GpuProfiler> Profiler{};
		RenderStatsHistory History{};
		std::vector<uint64_t> TimingSubmissions{};
		uint32_t NextTimingSlot = 0;
		std::optional<uint32_t> PendingTimingSlot{};
		uint64_t Generation = 1;
		uint64_t ImageFrame = 0;
		uint64_t ImageRevision = 0;
		uint64_t ImageGeneration = 0;
		std::vector<UUID> ImagePickTable{};
		bool PendingSubmission = false;
		bool RecordedPicking = false;
		bool SubmittedPicking = false;
		bool SpotLimitLogged = false;
		bool AllocationLimitLogged = false;
		nvrhi::BufferHandle ShadowConstantsBuffer{};
		Utils::MaterialBindingCache Materials{};

		Detail::SceneRendererTargets Targets{};
		size_t FinalTarget = 0; // the index of the LDR target holding the last image
		nvrhi::BufferHandle ViewConstantsBuffer{};
		nvrhi::BufferHandle EnvironmentConstantsBuffer{};
		nvrhi::BufferHandle LightBuffer{};
		// Reused by every Render.
		std::vector<Detail::SceneRendererDraw> OpaqueDraws{};
		std::vector<Detail::SceneRendererDraw> TransparentDraws{};
		std::vector<ShaderLight> Lights{};
		bool LightLimitLogged = false; // RENDER_LIGHT_LIMIT_EXCEEDED is logged once per renderer
		SceneRenderStats Stats{};

		// The targets of `width` x `height` with their framebuffers.
		[[nodiscard]] Result<Detail::SceneRendererTargets> CreateTargets(uint32_t width, uint32_t height) const;
		[[nodiscard]] Status EnsureFrameTargets(bool picking, bool ao, bool halfAo, uint32_t shadowSize, bool spots, bool selection, bool overdraw);
		// Clears every binding-set cache that references the targets.
		void ClearTargetBindings();
		// The material `handle` of this render: its mirror's set, created or found in the material cache; an empty
		// Detail::SceneRendererMaterial (no set: its draws are skipped) when the mirror lacks GPU objects, which the cache reported.
		// Errors: those of the material cache's GetOrCreate (Gpu), with the material named.
		[[nodiscard]] Result<Detail::SceneRendererMaterial> ResolveMaterial(AssetHandle handle);
		// Pass 1: the draws of `snapshot` seen by `camera` into OpaqueDraws and TransparentDraws, culled, resolved and sorted.
		// Returns the first non-finite world matrix's error, after skipping that draw, or a material set's creation error.
		[[nodiscard]] Status PrepareDraws(const RenderSnapshot& snapshot, const CameraData& camera);
		// Passes 4, 7 and 9: `draws` with the pipelines of `pass` (the forward ones of `view`) and the view set `viewSet`.
		void RecordMeshPass(nvrhi::ICommandList& commandList, Detail::SceneRendererMeshPass pass, std::span<const Detail::SceneRendererDraw> draws, nvrhi::IBindingSet& viewSet,
			RenderDebugView view, RenderPassCounters& counters);
		// Drops what the render did not use from every binding-set cache.
		void ReleaseUnusedBindings();
	};

}
