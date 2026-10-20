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
#include "Engine/Renderer/Private/ForwardPipelines.h"
#include "Engine/Renderer/Private/MaterialBindingCache.h"
#include "Engine/Renderer/QuadBatcher.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SelectionPass.h"
#include "Engine/Renderer/ShadowPass.h"
#include "Engine/Renderer/SkyboxPass.h"
#include "Engine/Renderer/TextRenderer.h"
#include "Engine/Renderer/TonemapPass.h"
#include "Engine/Renderer/UpscalePass.h"
#include "Shared/DrawConstants.h"
#include "Shared/ShaderLight.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
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
			std::vector<nvrhi::FramebufferHandle> CascadeFramebuffers{}; // ShadowPass::CreateFramebuffers(Cascades)
			nvrhi::FramebufferHandle SpotAtlasFramebuffer{};
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

		// M18 (C18.1, Docs/Decisions/0029-m18-contract.md decision 6): the output stage's per-view objects at the renderer's
		// (view) size, owned by stream F (SceneRendererOutput.cpp) and created only while the output mapping is not the
		// identity; at the identity the editor overlays draw into the scene's LDR target with SceneDepth, as before.
		struct SceneRendererOutputTargets
		{
			uint32_t Width = 0;
			uint32_t Height = 0;
			nvrhi::TextureHandle Color{};                  // OutputColor: LdrColorFormat (UpscalePass::GetColorFramebufferInfo)
			nvrhi::TextureHandle Depth{};                  // OutputDepth: SceneDepthFormat, created when an editor overlay needs depth
			nvrhi::FramebufferHandle ColorFramebuffer{};   // Color alone: UpscalePassInputs::Color
			nvrhi::FramebufferHandle DepthFramebuffer{};   // Depth alone: UpscalePassInputs::Depth
			nvrhi::FramebufferHandle OverlayFramebuffer{}; // Color with Depth: the EditorOverlay stage's target
			// The ViewConstants of the editor overlays: the scene camera's, with the viewport of the upscaled scene rectangle
			// (Scale times the scene size), so lines and labels keep their pixel sizes at the view's resolution.
			nvrhi::BufferHandle OverlayViewConstants{};
		};

		// M18 (C18.1, decision 1): what the stage functions of one Render share (SceneRenderer::State's Record*Stage). Render
		// fills the first group before the Prepare stage; each later group is filled by the stage it names and read by the
		// stages after it. These members are the frozen hand-over between stream A (Prepare to LdrPost) and stream F
		// (SceneOverlay, Output, EditorOverlay); stream A may add members that only its own stages read.
		struct SceneRendererFrame
		{
			// Set by Render before the Prepare stage.
			const RenderSnapshot* Snapshot = nullptr;
			nvrhi::ICommandList* CommandList = nullptr; // open, never null (the Prepare stage records nothing into it)
			RenderRecordingContext* Recording = nullptr;
			// ComputeRenderOutputMapping of the renderer's size and Snapshot->Output.
			RenderOutputMapping Output{};
			RenderDebugView DebugView = RenderDebugView::Lit;
			bool IsLit = true;       // DebugView == Lit
			bool IsDataView = false; // AO, ShadowCascades or Overdraw
			bool IsOverdraw = false;
			bool Wireframe = false; // Lit with RenderViewFlags::Wireframe
			bool Picking = false;   // RenderViewFlags::Picking
			bool Selection = false; // the selection outline is drawn (EditorOverlays and Selection with a selection, not a data view)
			bool PixelArt = false;  // RenderOutputSettings::PixelArt: the 2D colour path (decision 7)
			// Set by the Prepare stage.
			bool HasCamera = false; // the snapshot's camera, false also for one with a non-finite matrix (the view is cleared)
			CameraData Camera{};    // the snapshot's camera with its viewport at the scene resolution
			// Set by the Tonemap and LdrPost stages: the index of the scene-resolution LDR target holding the scene image
			// (SceneRendererTargets::Ldr), which the SceneOverlay stage draws into through OverlayFramebuffers.
			size_t SceneTarget = 0;
			// Set by the Output stage: the editor overlays' target and the depth they test against, at the renderer's size:
			// the scene's LDR target with SceneDepth at the identity, OutputColor with OutputDepth otherwise.
			nvrhi::ITexture* ViewColor = nullptr;
			nvrhi::IFramebuffer* ViewOverlayFramebuffer = nullptr;
			nvrhi::IBuffer* ViewOverlayConstants = nullptr;
			// The first partial error of the render, returned by Render (Architecture §8.2).
			Status FirstError{};

			// Keeps `result` when it is the render's first error.
			void KeepFirstError(Status result)
			{
				if (!result && FirstError)
					FirstError = std::move(result);
			}
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
		// M18 (C18.1): the QuadBatcher and the output stage's upscale (stream F), created by CreatePipelines once the wave
		// integrates them.
		Scope<QuadBatcher> Quads{};
		Scope<UpscalePass> Upscale{};
		uint32_t PipelineCount = 0;
		// M18 (C18.1): SceneRendererPipelines::GetGeneration.
		uint64_t Generation = 1;
		// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision 26): the set's registration with its device's recreation
		// registry (Create); kept by ReleaseGpuObjects like the back-references and the generation.
		GpuRecreationRegistration Recreation{};
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
		bool ShadowRejectionLogged = false; // a light the shadow planners reject is reported once per renderer
		bool WireframeBudgetLogged = false; // wireframe edges beyond the debug draw budget are reported once per renderer
		bool AllocationLimitLogged = false;
		nvrhi::BufferHandle ShadowConstantsBuffer{};
		nvrhi::BufferHandle ShadowCasterConstantsBuffer{}; // ShadowPass::GetCasterConstantsDesc
		Utils::MaterialBindingCache Materials{};

		Detail::SceneRendererTargets Targets{};
		size_t FinalTarget = 0; // the index of the LDR target holding the last image
		// M18 (C18.1): the output stage (stream F): its targets, its binding sets and the last Render's mapping
		// (SceneRenderer::GetOutputMapping); and the pipeline generation this renderer's binding sets were made for.
		Detail::SceneRendererOutputTargets OutputTargets{};
		PassBindingCache UpscaleBindings{};
		RenderOutputMapping OutputMapping{};
		uint64_t PipelineGeneration = 0;
		nvrhi::BufferHandle ViewConstantsBuffer{};
		nvrhi::BufferHandle EnvironmentConstantsBuffer{};
		nvrhi::BufferHandle LightBuffer{};
		// M18 (C18.3, decision 26): the renderer's registration with its device's recreation registry (Create); kept by
		// ReleaseGpuObjects like the back-references.
		GpuRecreationRegistration Recreation{};
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

		// M18 (C18.1, decision 1): Render's stage functions, one per RenderStage, called in that order between
		// RenderRecordingContext::BeginStage and EndStage; each records its stage's declared passes in their declared order,
		// or none, keeping partial errors in frame.FirstError. Stream A owns Prepare to LdrPost (SceneRenderer.cpp), stream F
		// SceneOverlay, Output and EditorOverlay (SceneRendererOutput.cpp).
		// Prepare (CPU): validates the snapshot, culls and sorts the draws, packs the lights, plans the shadows, writes the
		// per-view constants and ensures the frame's targets at the scene resolution. An error returned here is one of
		// Render's errors before recording: Render returns it and records nothing.
		[[nodiscard]] Status RecordPrepareStage(Detail::SceneRendererFrame& frame);
		// Reserved for M23's Skinning pass; records nothing in M18.
		void RecordDeformStage(Detail::SceneRendererFrame& frame);
		void RecordShadowsStage(Detail::SceneRendererFrame& frame);
		void RecordDepthNormalStage(Detail::SceneRendererFrame& frame);
		void RecordAmbientStage(Detail::SceneRendererFrame& frame);
		void RecordForwardStage(Detail::SceneRendererFrame& frame);
		// Reserved for M21's Sprites and Lights2D passes; records nothing in M18.
		void RecordSpritesStage(Detail::SceneRendererFrame& frame);
		void RecordHdrPostStage(Detail::SceneRendererFrame& frame);
		// Tonemap (or the data views' composite); with frame.PixelArt the 2D colour path (decision 7).
		void RecordTonemapStage(Detail::SceneRendererFrame& frame);
		// FXAA, never with frame.PixelArt or a debug view.
		void RecordLdrPostStage(Detail::SceneRendererFrame& frame);
		// Game Text (world, then screen) at the scene resolution into the scene LDR target; M22 appends Ui after it.
		void RecordSceneOverlayStage(Detail::SceneRendererFrame& frame);
		// The upscale when frame.Output is not the identity (UpscalePass); sets frame's ViewColor, ViewOverlayFramebuffer and
		// ViewOverlayConstants either way.
		void RecordOutputStage(Detail::SceneRendererFrame& frame);
		// Wireframe, the selection outline, the debug lines with annotations, grid and icons, then the labels, at the
		// renderer's size into frame.ViewOverlayFramebuffer, with the upscale's viewport and scissor (UpscalePass.h): the
		// viewport is the upscaled scene rectangle, partly outside the view for a cropped scene (negative offsets), and the
		// scissor is that rectangle intersected with the view, never a negative offset.
		void RecordEditorOverlayStage(Detail::SceneRendererFrame& frame);
	};

}
