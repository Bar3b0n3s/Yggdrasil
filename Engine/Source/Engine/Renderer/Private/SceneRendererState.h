#pragma once

#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/BloomPass.h"
#include "Engine/Renderer/BrdfLut.h"
#include "Engine/Renderer/DebugRenderer.h"
#include "Engine/Renderer/DepthPyramidPass.h"
#include "Engine/Renderer/ExposurePass.h"
#include "Engine/Renderer/FxaaPass.h"
#include "Engine/Renderer/GtaoPass.h"
#include "Engine/Renderer/LightClusterPass.h"
#include "Engine/Renderer/LightClusters.h"
#include "Engine/Renderer/Lights2DPass.h"
#include "Engine/Renderer/ParticlePass.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/PlanarReflectionPass.h"
#include "Engine/Renderer/PlanarReflections.h"
#include "Engine/Renderer/PointShadowMaps.h"
#include "Engine/Renderer/Private/ForwardPipelines.h"
#include "Engine/Renderer/Private/MaterialBindingCache.h"
#include "Engine/Renderer/Private/ProbeShading.h"
#include "Engine/Renderer/Private/TilemapDrawCache.h"
#include "Engine/Renderer/QuadBatcher.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SelectionPass.h"
#include "Engine/Renderer/ShadowCascades.h"
#include "Engine/Renderer/ShadowPass.h"
#include "Engine/Renderer/SkinningPass.h"
#include "Engine/Renderer/SkyboxPass.h"
#include "Engine/Renderer/SpotShadowAtlas.h"
#include "Engine/Renderer/SsrPass.h"
#include "Engine/Renderer/TaaPass.h"
#include "Engine/Renderer/TerrainPass.h"
#include "Engine/Renderer/TextRenderer.h"
#include "Engine/Renderer/TonemapPass.h"
#include "Engine/Renderer/UpscalePass.h"
#include "Engine/Renderer/WaterPass.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/FogConstants.h"
#include "Shared/InstanceRecord.h"
#include "Shared/LightClusterConstants.h"
#include "Shared/PlanarReflectionConstants.h"
#include "Shared/ShaderLight.h"
#include "Shared/ShadowConstants.h"
#include "Shared/ViewConstants.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

	struct SpriteSheetData;

	namespace Detail {

		// One submesh draw of the frame, resolved by pass 1.
		struct SceneRendererDraw
		{
			nvrhi::IBuffer* VertexBuffer = nullptr; // held by the GpuResourceCache for the render
			// M23 (C23.1, C-MESH2; Docs/Decisions/0034-m23-contract.md decision 9): vertex binding 1, the mesh's attribute stream
			// or the cache's shared default stream (GpuMesh::AttributeBuffer), a function of the mesh; held by the cache.
			nvrhi::IBuffer* AttributeBuffer = nullptr;
			nvrhi::IBuffer* IndexBuffer = nullptr;
			uint32_t IndexOffset = 0;
			uint32_t IndexCount = 0;
			nvrhi::IBindingSet* Material = nullptr; // held by the renderer's material cache for the render
			uint32_t ForwardVariant = 0;
			uint32_t PrepassVariant = 0; // opaque draws only
			// M23 (C23.1, R-INST; Docs/Decisions/0034-m23-contract.md decision 2): the draw's instance record (its world matrix,
			// pick id and draw flags) at index Instance of the view's Instances buffer, and the instanced draw it belongs to: a
			// draw that starts one has Batch, the number of its draws (itself and the next Batch - 1 draws of its list, whose
			// records follow its own); the others have 0. Set by the Prepare stage after sorting (GroupMeshInstances, or 1 for
			// every draw without instancing).
			uint32_t Instance = 0;
			uint32_t Batch = 1;
			// M23 (C23.3, Docs/Decisions/0034-m23-contract.md decision 27; stream C): 0 when VertexBuffer is the mesh's own
			// vertices; for a deformed item's draw, the VertexStream of its SceneRendererDeformedMesh, whose Vertices it binds
			// (MeshInstanceKey::VertexStream, so a deformed draw never shares an instanced draw with another item's).
			uint32_t VertexStream = 0;
			// M26 (C26.1, R-TAA-VEL; Docs/Decisions/0037-m26-contract.md decision 11; stream X-C): a deformed item's draw's
			// previous positions (its SceneRendererDeformedMesh's PreviousPositions, held by the renderer for the render), which
			// the prepass variants bind as vertex binding 2 (Utils::PreviousPositionSlot); null for every other draw, which
			// binds VertexBuffer there instead (ForwardPipelines.h).
			nvrhi::IBuffer* PreviousPositions = nullptr;
			// Sort keys (§8.3 pass 1).
			AssetHandle MaterialHandle{};
			AssetHandle Mesh{};
			UUID Entity{};
			uint32_t Submesh = 0;
			size_t SnapshotIndex = 0;
			float ViewDepth = 0.0f; // transparent draws: the view depth of the submesh bounds' centre
			// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): the bounds the draw was culled by (its submesh's,
			// or a deformed item's deformed bounds) in the item's local space, whose rectangle on screen ranks a planar
			// reflection's glass.
			Aabb Bounds{};
		};

		// A material of the frame, resolved once per render: its set, or null when its draws are skipped.
		struct SceneRendererMaterial
		{
			nvrhi::IBindingSet* Set = nullptr;
			Engine::AlphaMode Mode = Engine::AlphaMode::Opaque; // the type is spelled Engine:: (GCC's "changes meaning")
			bool DoubleSided = false;
		};

		// M23 (C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md decision 27; stream C): one deformed item's GPU
		// state, kept by its SceneRenderer across renders (the placement rule's place 2, SceneRenderer::State::DeformedMeshes),
		// keyed by the item's Entity and Mesh, which is how its previous positions survive from one render to the next: each
		// deformed item of a snapshot carries its own Entity (render extraction gives every item its entity, and a snapshot
		// built by hand gives each deformed item one, as the tests do), so two deformed items never share an entry (two items
		// of one key in one snapshot take two entries, in snapshot order), and each entry's VertexStream keeps its draws out of
		// every other item's instanced draw (each has its own pose). The Prepare stage acquires it (AcquireDeformedMesh) when
		// one of the item's draws survives culling, creating it at the item's first deformation and growing its buffers with
		// the mesh's vertex and joint counts; the Deform stage writes it each render that draws the item; one that a render
		// did not use is released at that render's end (with ReleaseUnusedBindings), so an item that leaves the view deforms
		// afresh, its previous positions its new ones, when it returns. A device loss's release drops every entry with the
		// state.
		struct SceneRendererDeformedMesh
		{
			UUID Entity{};
			AssetHandle Mesh{};
			uint64_t MeshVersion = 0;                // the GpuMesh's Version it was made for; another version deforms afresh
			nvrhi::BufferHandle Vertices{};          // SkinningPass::GetVerticesDesc: the deformed vertices, vertex binding 0 of its draws
			nvrhi::BufferHandle PreviousPositions{}; // SkinningPass::GetPreviousPositionsDesc: the last render's deformed positions
			nvrhi::BufferHandle Palette{};           // SkinningPass::GetPaletteDesc: the item's JointPalette
			nvrhi::BufferHandle MorphWeights{};      // SkinningPass::GetMorphWeightsDesc: the item's MorphWeights
			uint32_t VertexStream = 0;               // never 0: SceneRendererDraw::VertexStream of its draws, unique per renderer
			uint64_t LastRender = 0;                 // the render that last deformed it
			bool Deformed = false;                   // a deformation was recorded since its creation (otherwise FirstDeform)
			size_t SnapshotIndex = 0;                // the item of the LastRender's snapshot it deforms (SceneRenderer::State::ItemDeforms)
		};

		// M23 (C23.3, decision 27; stream C): how one snapshot item is drawn this render, resolved once by the Prepare stage
		// from its mesh's GPU mirror (SceneRenderer::State::ResolveItemDeform) by MeshDrawItem's rule: deformed when the mesh
		// has a skin that JointPalette covers, or morph targets with a resolved weight that is not 0. It holds what the Deform
		// stage dispatches with, so the mirror may change meanwhile, and is dropped at the render's end.
		struct SceneRendererItemDeform
		{
			bool Resolved = false;
			bool Deformed = false;
			// The item's deformation cannot be drawn this render (a palette matrix or morph weight that is not finite, its
			// buffers' creation failed, or its dispatch failed): none of its draws is recorded, and the error is the render's.
			bool Skipped = false;
			std::optional<size_t> Entry{}; // its DeformedMeshes entry, once acquired this render
			Aabb Bounds{};                 // ComputeDeformedBounds in the item's local space, which culls its draws
			std::vector<float> Weights{};  // ResolveMorphWeights against the mesh's defaults, MorphTargetCount of them
			nvrhi::BufferHandle SourceVertices{};
			nvrhi::BufferHandle Skin{};
			nvrhi::BufferHandle Morphs{};
			uint64_t MeshVersion = 0;
			uint32_t VertexCount = 0;
			uint32_t SkinJointCount = 0; // the mesh's when JointPalette covers its skin, else 0 (no skinning)
			uint32_t MorphTargetCount = 0;
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
			// M26 (C26.1, R-TAA-VEL, R-SSR-IN; Docs/Decisions/0037-m26-contract.md decision 14; stream X-C): the prepass's motion
			// vectors and SSR's specular inputs (SceneTargetFormats.h), attachments of PrepassFramebuffer and
			// PickingFramebuffer after SceneNormals (and EntityId), cleared to zero by RecordViewSetup.
			nvrhi::TextureHandle SceneVelocity{};
			nvrhi::TextureHandle SceneRoughness{};
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
			// M23 (C23.1, Docs/Decisions/0034-m23-contract.md decision 3; stream B): the PointShadowMaps cube array (6 x
			// MaxPointShadowLights layers of GetPointShadowFaceSize texels, the Shadows declaration's depth format), created
			// while a point light is planned, and one framebuffer per face layer.
			nvrhi::TextureHandle PointShadowMaps{};
			std::vector<nvrhi::FramebufferHandle> PointShadowFramebuffers{};
			// M29 (Docs/Decisions/0044-m29-light-limits.md decision 2): the PointShadowMapsHalf cube array (6 x
			// MaxHalfPointShadowLights layers of GetPointShadowHalfFaceSize texels), created while a Half-tier light is planned,
			// and one framebuffer per face layer.
			nvrhi::TextureHandle PointShadowMapsHalf{};
			std::vector<nvrhi::FramebufferHandle> PointShadowHalfFramebuffers{};
			nvrhi::TextureHandle SelectionMask{};
			nvrhi::TextureHandle SelectionScratch{};
			nvrhi::TextureHandle Overdraw{};
			nvrhi::FramebufferHandle PickingFramebuffer{};
			nvrhi::FramebufferHandle OverdrawFramebuffer{};
			nvrhi::TextureHandle Bloom{}; // BloomPass::GetChainDesc
			nvrhi::FramebufferHandle PrepassFramebuffer{};
			nvrhi::FramebufferHandle ForwardFramebuffer{};
			// M33 (Docs/Decisions/0048-m33-particle-look.md decision 4): the Particles pass's targets, SceneColor alone and
			// SceneVelocity alone (no depth attachment: the particles read SceneDepth as a texture).
			nvrhi::FramebufferHandle ParticleColorFramebuffer{};
			nvrhi::FramebufferHandle ParticleVelocityFramebuffer{};
			std::array<nvrhi::FramebufferHandle, 2> OverlayFramebuffers{}; // each LDR target with SceneDepth (tested, not written)
			// M21 (C21.2, Docs/Decisions/0032-m21-contract.md decision 22): the Lights2D pass's targets, created by the first
			// render that draws a lit 2D item (SceneRenderer::State::EnsureLights2DTargets) and kept like the other lazily
			// created targets until the size changes.
			nvrhi::TextureHandle Lights2DNormals{};                // Lights2DNormalsFormat
			nvrhi::TextureHandle Lights2DLight{};                  // Lights2DLightFormat
			nvrhi::FramebufferHandle Lights2DNormalsFramebuffer{}; // Lights2DNormals with SceneDepth (tested, not written)
			nvrhi::FramebufferHandle Lights2DLightFramebuffer{};   // Lights2DLight alone
			// M26 (C26.1, R-TAA; Docs/Decisions/0037-m26-contract.md decisions 17 and 29; stream X-C): TAA's history, the
			// declaration's TaaHistory (TaaPass::GetHistoryDesc), two images the Taa pass ping-pongs between (it reads the
			// previous resolved image from one and writes the new one into the other), created by the first render with TAA at
			// this size and kept like the other lazily created targets (the placement rule's place 2); SceneRenderer::State's
			// TaaHistoryValid says which one holds the view's history.
			std::array<nvrhi::TextureHandle, 2> TaaHistory{};
			// M26 (C26.1, R-SSR; Docs/Decisions/0037-m26-contract.md decisions 18 and 29, stream X-D): SSR's traced reflections
			// (SsrPass::GetTargetDesc) and the composite's framebuffer (SceneColor alone, SsrPass::GetCompositeFramebufferInfo),
			// created by the first render with SSR (SceneRenderer::State::EnsureSsrTargets) and kept like the other lazily created
			// targets until the size changes or a device loss's release drops them.
			nvrhi::TextureHandle SsrColor{};
			nvrhi::FramebufferHandle SsrFramebuffer{};
			// M47 (Docs/Decisions/0065-m47-water.md decision 3 and the W47.2 record): the Water pass's copy of SceneColor
			// (WaterRefraction, SceneColorFormat, from the pool) and the surfaces' framebuffers (SceneColor, SceneNormals,
			// SceneVelocity and SceneRoughness with SceneDepth, and the picking one with EntityId after them), created by the first
			// render that draws water (SceneRenderer::State::EnsureWaterTargets) and dropped by the first that draws none.
			nvrhi::TextureHandle WaterRefraction{};
			nvrhi::FramebufferHandle WaterFramebuffer{};
			nvrhi::FramebufferHandle WaterPickingFramebuffer{};
		};

		// M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 13): one timing slot's SSR area readback: the copy of the arguments
		// buffer (SsrPass::GetAreaReadbackDesc) a render that recorded SSR in the slot made, created on its first use, and what the
		// slot's last submitted render recorded (OnSubmitted sets it from SceneRenderer::State's PendingSsrArea).
		struct SceneRendererSsrAreaSlot
		{
			nvrhi::BufferHandle Readback{};
			uint64_t FrameIndex = 0;
			bool Submitted = false; // a render used the slot since its last collection
			bool Recorded = false;  // that render copied its arguments into Readback
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
			// The ViewConstants of the editor overlays: the overlay camera's (SceneRendererOutput.cpp: the scene camera whose
			// projection maps the scene's clip volume onto the upscaled scene rectangle, Scale times the scene size, of the
			// view-size viewport), so lines and labels keep their pixel sizes at the view's resolution.
			nvrhi::BufferHandle OverlayViewConstants{};
			// The 1 x 1 LdrColorFormat texel the letterbox repaint clears to the letterbox colour and upscales over the
			// letterbox bars after the editor overlays (UpscalePass::RecordLetterbox), created with OverlayViewConstants.
			nvrhi::TextureHandle LetterboxTexel{};
			// Stream F: the selection outline's mask and dilation scratch at the view size (SelectionPass::GetMaskDesc), created
			// when a selection is drawn; the EditorOverlay stage draws the outline at the view's resolution.
			nvrhi::TextureHandle SelectionMask{};
			nvrhi::TextureHandle SelectionScratch{};
			// Stream F: a 1 x 1 EntityId texel cleared to 0 by every picking render, which a pick of a letterbox pixel reads
			// (SceneRenderer::RequestPick: the background's result).
			nvrhi::TextureHandle LetterboxEntityId{};
		};

		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): where RecordMeshPass draws a mirror view's draws: its
		// framebuffer of `Width` x `Height` pixels, rasterization limited to `Scissor`. A mirror view's draws add nothing to
		// SceneRenderStats, which describes the main view.
		struct SceneRendererMeshTarget
		{
			nvrhi::IFramebuffer* Framebuffer = nullptr;
			uint32_t Width = 0;
			uint32_t Height = 0;
			nvrhi::Rect Scissor{};
		};

		// M42: what PrepareViewDraws leaves out of a mirror view besides its cull camera's frustum: the reflection's own entity
		// (a reflection never draws its own glass) and the submeshes wholly behind its clip plane.
		struct SceneRendererMirrorCull
		{
			UUID Excluded{};
			glm::vec4 ClipPlane = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		};

		// M42: the prepass targets of the mirror views of one size (the declaration's PlanarReflectionDepth, Normals, Velocity
		// and Roughness, SceneTargetFormats.h's formats, from the renderer's pool) with their framebuffer: the slots of one
		// size share them, since slots render in sequence. Kept across renders while a slot of the size is drawn
		// (ReleaseUnusedReflections).
		struct SceneRendererReflectionScratch
		{
			uint32_t Width = 0;
			uint32_t Height = 0;
			nvrhi::TextureHandle Depth{};
			nvrhi::TextureHandle Normals{};
			nvrhi::TextureHandle Velocity{};
			nvrhi::TextureHandle Roughness{};
			nvrhi::FramebufferHandle PrepassFramebuffer{};
			bool Used = false; // a slot of this render uses it
		};

		// M42 (decision 3): one planar reflection slot of a renderer (the placement rule's place 2,
		// SceneRenderer::State::ReflectionSlots): the mirror view the Prepare stage plans for the reflection drawn in the slot
		// this render (PrepareReflectionSlot), which the Forward stage records (RecordPlanarReflections), and its GPU objects,
		// kept across renders while the slot is drawn and released by the first render that does not draw it
		// (ReleaseUnusedReflections).
		struct SceneRendererReflectionSlot
		{
			// Set by the Prepare stage for a drawn slot.
			bool Drawn = false;
			UUID Entity{};
			CameraData Camera{}; // the mirror camera at the slot's size (MakePlanarReflectionCamera)
			ViewConstants View{};
			FogConstants Fog{};
			LightClusterGrid Clusters{};
			// The mirror view's light list: the main view's (Lights, at their indices, with their shadow slots), then the lights
			// only the mirror view sees, unshadowed, up to the snapshot's MaxShadedLights.
			std::vector<ShaderLight> Lights{};
			std::vector<SceneRendererDraw> OpaqueDraws{};
			std::vector<SceneRendererDraw> TransparentDraws{};
			// The glass's rectangle widened by the blur's footprint (ComputePlanarReflectionBlurMargin, from the slot's chain),
			// mirrored into the reflection's pixels: the rectangle its draws are culled to and rasterized in.
			nvrhi::Rect Scissor{};
			size_t Scratch = 0; // its SceneRenderer::State::ReflectionScratch entry
			// The GPU objects: the reflection (PlanarReflectionPass::GetColorDesc of the slot's size and chain, from the pool), its
			// level 0 with the scratch's depth (the forward passes' and the skybox's framebuffer) and alone (the particles'),
			// and the mirror view's constants, light list and light clusters.
			nvrhi::TextureHandle Color{};
			nvrhi::FramebufferHandle ForwardFramebuffer{};
			nvrhi::FramebufferHandle ParticleFramebuffer{};
			nvrhi::BufferHandle ViewConstantsBuffer{};
			nvrhi::BufferHandle FogConstantsBuffer{};
			nvrhi::BufferHandle LightBuffer{};
			nvrhi::BufferHandle LightClusterConstantsBuffer{};
			nvrhi::BufferHandle LightClusterBuffer{};
			// The mirror view's terrain state (TerrainPass.h) and particle plan, as the main view's.
			TerrainViewResources TerrainResources{};
			ParticlePassPlan ParticlePlan{};
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
			bool IsDataView = false; // AO, ShadowCascades, Overdraw or LightCount
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

			// Stream A's hand-over between its own stages (Prepare to LdrPost), which no later stage reads.
			// Set by the Prepare stage: GTAO and its resolution; the per-view constants Render writes before the Deform stage
			// (ViewConstants b0, EnvironmentConstants b2, ShadowConstants b1); the shadow plans; the environment's cubes (the
			// black cube without a map) and the blue noise of the dither (null without one).
			bool Ao = false;
			bool HalfAo = false;
			// M26 (C26.1, R-SSR; decision 18, stream X-D): SSR records this render (PostProcessSettings::SsrEnabled in a Lit view
			// with a camera outside the wireframe), so the Ambient stage records the depth pyramid it traces over even without
			// GTAO; cleared when its targets or the pyramid fail (their error kept), so the Forward stage records no SSR pass.
			bool Ssr = false;
			// M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 11): the trace at half resolution
			// (RenderQualitySettings::SsrHalfResolution).
			bool HalfSsr = false;
			// M47 (Docs/Decisions/0065-m47-water.md decision 3): the Water pass records this render (a snapshot with water in a Lit
			// view with a camera outside the wireframe), so the Ambient stage records the depth pyramid for it; cleared when its
			// targets or the pyramid fail (their error kept), so the Forward stage draws no water. Set by the Forward stage:
			// WaterSurfaces, the waters it drew (RenderStats::WaterSurfaces).
			bool Water = false;
			uint32_t WaterSurfaces = 0;
			// M32's milestone review: the skybox draws over the cleared pixels (a Lit view clearing to a skybox it has and shows).
			bool DrawsSkybox = false;
			ViewConstants View{};
			EnvironmentConstants Environment{};
			// M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 5): the view's fog (Renderer/Fog.h), Enabled only in a Lit view
			// with a camera outside the wireframe whose snapshot holds fog.
			FogConstants Fog{};
			ShadowConstants Shadow{};
			ShadowCascadeSet Cascades{};
			SpotShadowAtlas Spots{};
			// M23 (C23.1, decisions 3 and 5; streams B and C): the point-shadow plan (ShadowConstants' Points), and the cluster
			// grid the LightClusters pass and the forward passes read (LightClusterConstants b3).
			PointShadowSet Points{};
			LightClusterGrid Clusters{};
			// M29 (decision 4): set by the Shadows stage: the planned point and spot lights whose maps the static shadow cache
			// gave (RenderStats::CachedPointShadows and CachedSpotShadows).
			uint32_t CachedPointShadows = 0;
			uint32_t CachedSpotShadows = 0;
			// M23 (C23.3, decision 27; stream C): set by the Prepare stage: the indices into SceneRenderer::State::DeformedMeshes
			// of the items the Deform stage deforms this render (every deformed item one of whose draws survived culling, the
			// shadow casters' included), in snapshot order.
			std::vector<size_t> Deforms{};
			// M28 (Docs/Decisions/0043-m28-game-services.md decision 4): set by the Deform stage: the items with a skin it
			// deformed this render (RenderStats::SkinnedMeshes).
			uint32_t SkinnedMeshes = 0;
			nvrhi::TextureHandle EnvironmentSpecular{};
			nvrhi::TextureHandle EnvironmentSkybox{};
			nvrhi::TextureHandle BlueNoise{};
			// M31 (Docs/Decisions/0046-m31-material-parameters.md decision 6): set by the Prepare stage: the dissolve noise the
			// prepass, forward and shadow sets bind at Utils::DissolveNoiseRegister: the built-in DissolveNoise texture's mirror
			// while an item of the snapshot dissolves, else (and when the mirror is a placeholder or missing) the neutral white
			// texture: unread without a dissolve; with a dissolve and a placeholder mirror, only an amount of 1 dissolves.
			nvrhi::TextureHandle DissolveNoise{};
			// Set by the DepthNormal stage: the view's prepass set (null without a camera or when it cannot be created), and
			// (M26: the overdraw pipelines' own set 0, Private/ForwardPipelines.h) the set the Overdraw data view draws with
			// (null outside it).
			nvrhi::IBindingSet* PrepassSet = nullptr;
			nvrhi::IBindingSet* OverdrawSet = nullptr;
			// M26 (C26.1, R-TAA; Docs/Decisions/0037-m26-contract.md decisions 10 and 17; stream X-C): set by the Prepare stage.
			// Taa: the Taa pass resolves this render (PostProcessSettings::AntiAliasing Taa in a Lit view with a camera, outside
			// the wireframe and pixel-art mode), so the scene passes' matrices in View include the snapshot's jitter
			// (ViewConstants::Jitter); OverlayView: the unjittered ViewConstants the passes after the HDR post chain draw with
			// (game Text, the Ui, and the editor overlays at the identity output mapping), the view's ViewConstantsBuffer when
			// nothing is jittered, else its UnjitteredViewConstantsBuffer, which RecordViewSetup writes with OverlayConstants.
			bool Taa = false;
			nvrhi::IBuffer* OverlayView = nullptr;
			ViewConstants OverlayConstants{};
			// Set by the Ambient stage: the occlusion and linear depth the forward pass and the data views read (the neutral
			// white AO and far depth when GTAO did not run).
			nvrhi::ITexture* Occlusion = nullptr;
			nvrhi::ITexture* LinearDepth = nullptr;
			// Set by the HdrPost stage: the bloom chain's result for the Tonemap stage (null without bloom) and its intensity.
			nvrhi::ITexture* Bloom = nullptr;
			float BloomIntensity = 0.0f;
			// M26 (C26.1, R-AEX; Docs/Decisions/0037-m26-contract.md decision 15, stream X-A): set by the HdrPost stage: the
			// Exposure pass's adapted exposure for the Tonemap stage (SceneRenderer::State::ExposureStateBuffer), null when the
			// pass did not run (Manual exposure, a debug view, the wireframe, pixel-art mode, or its error).
			nvrhi::IBuffer* AdaptedExposure = nullptr;
			// M23 (C23.1, R-INST; Docs/Decisions/0034-m23-contract.md decision 2): set by the Prepare stage with the view's
			// instance records (SceneRenderer::State::Instances, after the camera's draws'): the shadow casters the Shadows stage
			// records (ShadowPass::PrepareCasters; empty without a planned shadow) and the selection mask's draws the
			// EditorOverlay stage records (SelectionPass::PrepareDraws; empty without frame.Selection).
			ShadowCasterSet ShadowCasters{};
			SelectionDrawSet SelectionDraws{};
			// M26 (C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 16; stream Q-B): set by the Prepare stage: the
			// view's probes (Private/ProbeShading.h), which RecordViewSetup uploads into the renderer's ProbeResources and the
			// forward passes read (b4, t13 and t14 of set 0).
			Utils::ProbeShadingInputs Probes{};
			// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): set by the Prepare stage: the main view's light list
			// as indices into the snapshot's Lights (CullLights' Visible, Lights' order), which every mirror view's list starts
			// with; the reflections drawn this render (the slots 0 to PlanarReflectionCount - 1 of
			// SceneRenderer::State::ReflectionSlots) and the view's PlanarReflectionConstants (b7 of the forward set); and
			// RenderStats' DroppedPlanarReflections, CulledPlanarReflections and PlanarReflectionEntities.
			std::vector<uint32_t> VisibleLights{};
			uint32_t PlanarReflectionCount = 0;
			PlanarReflectionConstants PlanarReflections{};
			uint32_t DroppedPlanarReflections = 0;
			uint32_t CulledPlanarReflections = 0;
			std::vector<UUID> PlanarReflectionEntities{};

			// Keeps `result` when it is the render's first error.
			void KeepFirstError(Status result)
			{
				if (!result && FirstError)
					FirstError = std::move(result);
			}
		};

	}

	namespace Detail {

		// M42: SceneRenderer.cpp's helpers that the renderer's other files share (SceneRendererReflections.cpp).
		// A `width` x `height` attachment of `format` from `pool`, sampled by later passes and kept between command lists in its
		// writing state (RenderTarget, or DepthWrite for SceneDepthFormat), named "SceneRenderer.<name>".
		[[nodiscard]] Result<nvrhi::TextureHandle> CreateSceneAttachment(RenderTargetPool& pool, uint32_t width, uint32_t height, nvrhi::Format format,
			std::string_view name);
		// A buffer of `size` bytes kept in the renderer's state between command lists (keepInitialState): a constant buffer, or
		// a structured buffer of `structStride`-byte elements (SceneRendererRecovery.cpp's, which the per-view buffers and each
		// planar reflection slot's use). Errors: those of GraphicsDevice::CreateBuffer (Gpu).
		[[nodiscard]] Result<nvrhi::BufferHandle> CreateViewBuffer(GraphicsDevice& device, size_t size, uint32_t structStride, std::string debugName);
		// The ViewConstants of `camera` for a `width` x `height` target without jitter, with `exposure` (SceneRenderer.cpp's
		// MakeViewConstants).
		[[nodiscard]] ViewConstants MakeSceneViewConstants(const CameraData& camera, uint32_t width, uint32_t height, float exposure);

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
		// M23 (R-PLS): the neutral PointShadowMaps (t12) of a view without a planned point light (SceneRenderer.cpp).
		nvrhi::TextureHandle EmptyPointShadows{};
		// M26 (C26.1, R-GI-PROBE; stream Q-B): the neutral IrradianceProbes (t13) and ReflectionProbeCubes (t14) of a view
		// without irradiance volumes or reflection probes (Private/ProbeShading.h).
		nvrhi::BufferHandle EmptyIrradianceProbes{};
		nvrhi::TextureHandle EmptyReflectionProbeCubes{};
		nvrhi::TextureHandle WhiteAo{};
		nvrhi::TextureHandle FarDepth{};
		nvrhi::SamplerHandle ShadowCompare{};
		Scope<ShadowPass> Shadows{};
		Scope<DepthPyramidPass> DepthPyramid{};
		Scope<GtaoPass> Gtao{};
		// M23 (C23.1, Docs/Decisions/0034-m23-contract.md decision 5; stream C): the LightClusters pass (the placement rule's
		// place 1).
		Scope<LightClusterPass> LightClusters{};
		// M23 (C23.3, decision 27; stream C): the Skinning pass of the Deform stage (place 1), created by CreatePipelines.
		Scope<SkinningPass> Skinning{};
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
		// M21 (C21.2, Docs/Decisions/0032-m21-contract.md decision 22): the 2D lights' accumulation (place 1 of ADR 0029
		// decision 27).
		Scope<Lights2DPass> Lights2D{};
		// M26 (C26.1, R-AEX; Docs/Decisions/0037-m26-contract.md decisions 15 and 29, stream X-A): histogram auto-exposure's
		// Exposure pass (place 1).
		Scope<ExposurePass> Exposure{};
		// M26 (C26.1, R-SSR; decisions 18 and 29, stream X-D): the SSR pass's trace and composite (place 1).
		Scope<SsrPass> Ssr{};
		// M26 (C26.1, R-TAA; decisions 17 and 29, stream X-C): the Taa pass (place 1).
		Scope<TaaPass> Taa{};
		// M26 (C26.1, C-TERR; decisions 22 and 29, stream T-C): the terrain passes' pipelines and layouts (place 1).
		Scope<TerrainPass> Terrain{};
		// M33 (Docs/Decisions/0048-m33-particle-look.md decision 4): the Particles pass's pipelines, samplers and buffers
		// (place 1).
		Scope<ParticlePass> Particles{};
		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): the planar reflections' mip chain (place 1), and the
		// forward set's neutral 1 x 1 black reflection (SceneColorFormat), which a mirror view, and a main view's slot without
		// a drawn reflection, binds at t17 to t20.
		Scope<PlanarReflectionPass> PlanarReflections{};
		nvrhi::TextureHandle EmptyPlanarReflection{};
		// M47 (Docs/Decisions/0065-m47-water.md decision 3): the Water pass's pipelines and clipmap meshes (place 1).
		Scope<WaterPass> Water{};
		uint32_t PipelineCount = 0;
		// M18 (C18.1): SceneRendererPipelines::GetGeneration: 0 until Create's CreatePipelines, which makes it 1, and increased
		// by every later successful CreatePipelines.
		uint64_t Generation = 0;
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
		PassBindingCache TextBindings{}; // also the Ui pass's glyph runs (the same atlases and ViewConstants as game Text)
		// M22 (C22.1, Docs/Decisions/0033-m22-contract.md decision 7): the Ui pass's quad sets (UiPass.h, QuadBatcher::Record:
		// one per texture and sampler), released with the others (ClearBindings, ReleaseUnusedBindings).
		PassBindingCache UiQuadBindings{};
		PassBindingCache ShadowBindings{};
		PassBindingCache DepthBindings{};
		PassBindingCache GtaoBindings{};
		PassBindingCache SelectionBindings{};
		PassBindingCache DataViewBindings{};
		PassBindingCache SkinningBindings{};     // M23 (C23.3, stream C): the Skinning pass's sets
		PassBindingCache LightClusterBindings{}; // M23 (stream C): the LightClusters pass's sets
		// M21 (Docs/Decisions/0032-m21-contract.md decision 4): the Sprites stage's sets, the QuadBatcher's one per texture and
		// sampler (place 2 of ADR 0029 decision 27: released with the state on a device loss, like every set above).
		PassBindingCache SpriteBindings{};
		// M21 (C21.2, decision 22): the Lights2D pass's set (its view's Lights2DNormals).
		PassBindingCache Lights2DBindings{};
		// M26 (C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decision 7): the Particles pass's sets, one per texture and
		// sampler (place 2 of ADR 0029 decision 27; M33's own program since Docs/Decisions/0048-m33-particle-look.md decision 4,
		// each with the view's constants and SceneDepth), released with the others (ClearBindings, ReleaseUnusedBindings).
		PassBindingCache ParticleBindings{};
		// M26 (C26.1, R-AEX; decision 15, stream X-A): the Exposure pass's two sets (its view's SceneColor, bloom chain and
		// exposure buffers).
		PassBindingCache ExposureBindings{};
		// M26 (C26.1, R-SSR; decision 18, stream X-D): the SSR pass's sets (its view's prepass targets, pyramid, SsrColor and
		// SceneColor; M32: three, with the classification's tile list and arguments).
		PassBindingCache SsrBindings{};
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
		bool Light2DLimitLogged = false; // lights beyond MaxVisibleLights2D are reported once per renderer (M21, C21.2)
		bool TileLimitLogged = false;    // tiles beyond MaxTileQuadsPerView are reported once per renderer (M21's milestone review)
		nvrhi::BufferHandle ShadowConstantsBuffer{};
		nvrhi::BufferHandle ShadowCasterConstantsBuffer{}; // ShadowPass::GetCasterConstantsDesc
		Utils::MaterialBindingCache Materials{};

		Detail::SceneRendererTargets Targets{};
		// The renderer's (view) size: Targets are at the scene resolution of the last Render's output mapping.
		uint32_t ViewWidth = 1;
		uint32_t ViewHeight = 1;
		size_t FinalTarget = 0; // the index of the LDR target holding the last image
		// M18 (C18.1): the output stage (stream F): its targets, its binding sets and the last Render's mapping
		// (SceneRenderer::GetOutputMapping); and the pipeline generation this renderer's binding sets were made for.
		Detail::SceneRendererOutputTargets OutputTargets{};
		PassBindingCache UpscaleBindings{};
		RenderOutputMapping OutputMapping{};
		// M18's milestone review: the last Render's output settings (the defaults before the first), at whose scene resolution
		// Resize and the device-loss recovery create the targets, so the next Render with the same settings keeps them. CPU
		// state that a device loss's release keeps (SceneRendererRecovery.cpp).
		RenderOutputSettings OutputSettings{};
		uint64_t PipelineGeneration = 0;
		nvrhi::BufferHandle ViewConstantsBuffer{};
		nvrhi::BufferHandle EnvironmentConstantsBuffer{};
		// M32 (decision 5): the view's FogConstants (b5 of the forward, skybox and SSR-composite sets, b1 of the Sprites stage's
		// quads), written with the other per-view constants.
		nvrhi::BufferHandle FogConstantsBuffer{};
		nvrhi::BufferHandle LightBuffer{};
		// M23 (C23.1, Docs/Decisions/0034-m23-contract.md decisions 2 and 5): the mesh path's per-view buffers (the placement
		// rule's place 2), null until their streams create them: the Instances structured buffer of InstanceRecord (stream
		// A: t10 of set 0, grown to the view's instance count), the cluster masks (stream C: t11 of set 0, the LightClusters
		// pass's u0) and the cluster grid's constants (stream C: b3 of set 0).
		nvrhi::BufferHandle InstanceBuffer{};
		nvrhi::BufferHandle LightClusterBuffer{};
		nvrhi::BufferHandle LightClusterConstantsBuffer{};
		// M26 (C26.1, R-AEX; Docs/Decisions/0037-m26-contract.md decisions 10, 15 and 29, stream X-A): the view's exposure
		// buffers (place 2), created by the first render with auto-exposure (EnsureExposureBuffers): the Exposure pass's
		// histogram and its adapted exposure (ExposureState, which the next render's pass adapts from and the Tonemap pass
		// reads). ExposureValid says the state buffer holds this view's adapted exposure of its previous render: false until a
		// render records the pass, after a render that did not (Manual exposure, a debug view, the wireframe, pixel-art mode, a
		// failed record), and in the new state a device loss's release makes, so the next pass starts afresh (a reset).
		nvrhi::BufferHandle ExposureHistogramBuffer{};
		nvrhi::BufferHandle ExposureStateBuffer{};
		bool ExposureValid = false;
		// M26 (C26.1, R-TAA; Docs/Decisions/0037-m26-contract.md decisions 10, 17 and 29, stream X-C): the unjittered
		// ViewConstants of a render with TAA's jitter (place 2), created by the first such render; the Taa pass's binding sets;
		// and which of Targets.TaaHistory holds the view's resolved image of its previous render (TaaHistoryIndex), valid only
		// while TaaHistoryValid: false until a render records the pass, after a render that did not (TAA off, a debug view,
		// the wireframe, pixel-art mode, a failed record), whenever the history targets are created again (a new size, a
		// device loss's release, ReleaseTargets), so the next pass starts afresh (a reset).
		nvrhi::BufferHandle UnjitteredViewConstantsBuffer{};
		PassBindingCache TaaBindings{};
		uint32_t TaaHistoryIndex = 0;
		bool TaaHistoryValid = false;
		// M26 (W26.2's review): RenderStats::HistoryFrames of the last render, which the next continues; 0 in the new state a
		// device loss's release makes.
		uint32_t HistoryFrames = 0;
		// M32 (Docs/Decisions/0047-m32-fog-ssr.md decisions 10 and 13): the SSR pass's tile list (grown to the trace image's
		// tiles, never shrunk) and arguments buffer, created by the first render with SSR (EnsureSsrTargets); one readback slot
		// per timing slot (resized with TimingSubmissions); and what the render awaiting OnSubmitted recorded in its timing slot
		// (Recorded with the frame's index), which OnSubmitted moves into the slot. A device loss's release drops them all.
		nvrhi::BufferHandle SsrTiles{};
		nvrhi::BufferHandle SsrArguments{};
		std::vector<Detail::SceneRendererSsrAreaSlot> SsrAreaSlots{};
		Detail::SceneRendererSsrAreaSlot PendingSsrArea{};
		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): the planar reflections (the placement rule's place 2):
		// the view's PlanarReflectionConstants (b7 of the forward set, written by every Render, created with the other per-view
		// buffers), the slots and the mirror views' shared prepass targets (both released when a render leaves them unused,
		// ReleaseUnusedReflections), the mip chain's binding sets, and whether the reflections dropped at the cap were logged
		// (once per renderer).
		nvrhi::BufferHandle PlanarReflectionConstantsBuffer{};
		std::array<Detail::SceneRendererReflectionSlot, MaxPlanarReflectionsPerView> ReflectionSlots{};
		std::vector<Detail::SceneRendererReflectionScratch> ReflectionScratch{};
		PassBindingCache ReflectionBindings{};
		bool PlanarReflectionLimitLogged = false;
		// M18 (C18.3, decision 26): the renderer's registration with its device's recreation registry (Create); kept by
		// ReleaseGpuObjects like the back-references.
		GpuRecreationRegistration Recreation{};
		// Reused by every Render.
		std::vector<Detail::SceneRendererDraw> OpaqueDraws{};
		std::vector<Detail::SceneRendererDraw> TransparentDraws{};
		// M21: the Sprites stage's quads in the snapshot's draw order, their textures borrowed from the GpuResourceCache's
		// mirrors for the render, and, one per quad, whether it is lit (C21.2, decision 22: 1 for a lit sprite or tile, which
		// draws with the HdrLit pipeline); AddSpriteQuad appends to both.
		std::vector<QuadDraw> SpriteQuads{};
		std::vector<uint8_t> SpriteQuadLit{};
		// C21.2: the lit quads of SpriteQuads, in order, for the Normals record, and the 2D lights the view draws.
		std::vector<QuadDraw> LitSpriteQuads{};
		std::vector<Light2DItem> VisibleLights2D{};
		// M21's milestone review: the tiles this render's Sprites stage has drawn and dropped (SceneRenderer::
		// MaxTileQuadsPerView), counted by AppendTilemapQuads. The quad vectors above hold at most that many tiles and the
		// snapshot's sprites during a render, and keep room for at most QuadBatcher::MaxQuadsPerRecord quads after it
		// (ReleaseOutsizedSpriteQuads).
		uint32_t TileQuadsDrawn = 0;
		uint32_t TileQuadsDropped = 0;
		// M21 (C21.2, Docs/Decisions/0032-m21-contract.md decision 24): the Sprites stage's chunk bounds of each tilemap it
		// drew in its last render (TilemapDrawCache.h; a render without drawn 2D items keeps none), and its scratch flags of
		// the chunks that meet the view. CPU state; a device loss's release drops it with the rest of the state, and the next
		// render rebuilds it.
		std::vector<Detail::TilemapDrawCache> TilemapCaches{};
		std::vector<uint8_t> TilemapChunksDrawn{};
		// M26 (C26.1, C-PART; decision 7): the Particles pass's plan of the render (ParticlePass.h), and M33's flipbooks of its
		// emitters (Docs/Decisions/0048-m33-particle-look.md decision 5). CPU state reused by every render, keeping room for at
		// most ParticlePass::MaxQuadsPerBatch quads after it (RecordParticles); a device loss's release drops it with the rest
		// of the state.
		ParticlePassPlan ParticlePlan{};
		std::vector<ParticleFlipbook> ParticleFlipbooks{};
		std::vector<ShaderLight> Lights{};
		bool LightLimitLogged = false; // RENDER_LIGHT_LIMIT_EXCEEDED is logged once per renderer
		// M29 (Docs/Decisions/0044-m29-light-limits.md decision 3): the previous render's shaded point and spot lights
		// (LightCullResult::Shaded), which CullLights' hysteresis reads in a render that continues the view's history
		// (CameraData::PreviousViewProjection); any other render clears it first, and a device loss's release drops it.
		std::vector<LightKey> ShadedLightHistory{};
		// M29 (decision 3): the previous render's spot and point shadow plans, which AllocateSpotShadowAtlas' and
		// AllocatePointShadows' hysteresis and stable slots read under the same rule (empty after any other render).
		SpotShadowAtlas SpotShadowHistory{};
		PointShadowSet PointShadowHistory{};
		// M29 (decision 4): the static shadow cache (ShadowPass.h's ShadowMapCache), cleared by a render that does not continue
		// the view's history, by EnsureFrameTargets when it creates or re-acquires a spot or point shadow target, by a shader
		// reload (Render's pipeline-generation check) and, with the state, by a device loss's release.
		ShadowMapCache ShadowCache{};
		// M23 (C23.1): the view's instance records, reused by every Render (stream A: the opaque draws', then the transparent
		// draws', the shadow casters' and the selection's, written into InstanceBuffer by RecordViewSetup), and
		// RENDER_POINT_SHADOW_BUDGET logged once per renderer (stream B).
		std::vector<InstanceRecord> Instances{};
		bool PointShadowLimitLogged = false;
		// M26 (C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 16; stream Q-B): the view's copies of its probes'
		// mirrors (Private/ProbeShading.h, the placement rule's place 2), and whether the probes a view dropped (beyond its
		// limits, or a reflection probe whose cube's format or size differs from the first's) were logged, once per renderer
		// each.
		Utils::ProbeViewResources ProbeResources{};
		bool ProbeLimitLogged = false;
		bool ProbeFormatLogged = false;
		// M26 (C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decisions 22 and 29; stream T-C): the view's terrain state
		// (TerrainPass.h, the placement rule's place 2): its chunk meshes, its terrains' constants and sets, and the render's
		// prepared terrains, which the DepthNormal stage prepares and the Forward stage draws again.
		TerrainViewResources TerrainResources{};
		// M47 (Docs/Decisions/0065-m47-water.md decision 3): the view's water records and their sets (WaterPass.h, place 2).
		WaterViewResources WaterResources{};
		// M23 (C23.3, Docs/Decisions/0034-m23-contract.md decision 27; stream C): the deformed items' GPU state (place 2), the
		// next VertexStream value to give a new entry (from 1), and the render count that stamps LastRender. A skinned item
		// whose palette is shorter than its mesh's skin draws its bind pose, which is logged once per renderer
		// (RenderSkinMismatchCode): ReleaseGpuObjects keeps SkinMismatchLogged with the other once-per-renderer flags
		// (SceneRendererRecovery.cpp's kept members, stream C), so a recovered renderer does not warn again.
		std::vector<Detail::SceneRendererDeformedMesh> DeformedMeshes{};
		uint32_t NextVertexStream = 1;
		uint64_t DeformRender = 0;
		bool SkinMismatchLogged = false;
		// The render's items' deformations, one per snapshot item (ResolveItemDeform), cleared at the render's end.
		std::vector<Detail::SceneRendererItemDeform> ItemDeforms{};
		SceneRenderStats Stats{};
		// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decision 44): the debug view the last Render recorded (Lit before the
		// first), which a Render records instead of a debug view whose first pipelines ran out of memory after their trim and
		// retry (SceneRenderer::GetRenderedDebugView); and the views whose switch failed that way, logged once per renderer and
		// view. CPU state that a device loss's release keeps (SceneRendererRecovery.cpp).
		RenderDebugView RenderedDebugView = RenderDebugView::Lit;
		std::array<bool, RenderDebugViewCount> DebugViewFailureLogged{};

		// The targets of `width` x `height` with their framebuffers.
		[[nodiscard]] Result<Detail::SceneRendererTargets> CreateTargets(uint32_t width, uint32_t height) const;
		// `pointShadowFaceSize`: the PointShadowMaps' face size while a point light is planned (GetPointShadowFaceSize), else 0;
		// M29: `pointShadowHalfFaceSize` the PointShadowMapsHalf's while a Half-tier light is (GetPointShadowHalfFaceSize), else 0.
		[[nodiscard]] Status EnsureFrameTargets(bool picking, bool ao, bool halfAo, uint32_t shadowSize, bool spots, uint32_t pointShadowFaceSize,
			uint32_t pointShadowHalfFaceSize, bool selection, bool overdraw);
		// Clears every binding-set cache that references the targets.
		void ClearTargetBindings();
		// Clears every binding-set cache and the material sets: Render calls it before its first recording with a new
		// generation of the pipeline set (SceneRendererPipelines::GetGeneration), whose binding layouts replaced those the
		// sets were made for.
		void ClearBindings();
		// The material `handle` of this render: its mirror's set, created or found in the material cache; an empty
		// Detail::SceneRendererMaterial (no set: its draws are skipped) when the mirror lacks GPU objects, which the cache reported.
		// Errors: those of the material cache's GetOrCreate (Gpu), with the material named.
		[[nodiscard]] Result<Detail::SceneRendererMaterial> ResolveMaterial(AssetHandle handle);
		// Pass 1: the draws of `snapshot` seen by `camera` into OpaqueDraws and TransparentDraws, culled, resolved and sorted;
		// then (M23) their instance records into Instances and their instanced draws (SceneRendererDraw's Instance and Batch).
		// A material of a culled submesh keeps the set it already has while its mirror is current (R-MATSET,
		// MaterialBindingCache::Keep), so it makes no new set when it re-enters the view; a culled material never gets one.
		// M23 (C23.3, stream C): a deformed item's submeshes are culled by its deformed bounds, and its surviving draws bind
		// its DeformedMeshes entry's vertices with its VertexStream (AcquireDeformedMesh, into frame.Deforms).
		// Returns the first non-finite world matrix's error, after skipping that draw, or a material set's creation error,
		// or a deformation's (ResolveItemDeform, AcquireDeformedMesh), after skipping that item's draws.
		[[nodiscard]] Status PrepareDraws(Detail::SceneRendererFrame& frame, const RenderSnapshot& snapshot, const CameraData& camera);
		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): PrepareDraws' work into `opaque` and `transparent`:
		// the main view's without `mirror`; with it a mirror view's, culled by `camera` (its cull camera), without `mirror`'s
		// excluded entity and the submeshes wholly behind its clip plane, counting no culled submesh in Stats, its instance
		// records appended to Instances after those already there.
		[[nodiscard]] Status PrepareViewDraws(Detail::SceneRendererFrame& frame, const RenderSnapshot& snapshot, const CameraData& camera,
			std::vector<Detail::SceneRendererDraw>& opaque, std::vector<Detail::SceneRendererDraw>& transparent, const Detail::SceneRendererMirrorCull* mirror);
		// M23 (C23.3, decision 27; stream C): resolves ItemDeforms[index] (a valid mesh handle) once per render from the item's
		// mesh mirror (GetMesh, which may upload: the caller holds no mirror reference across it), logging a palette shorter
		// than a skin once per renderer (RenderSkinMismatchCode; an empty palette is the bind pose without a warning). Errors,
		// with the item Skipped: InvalidArgument for a palette matrix or a morph weight that is not finite.
		[[nodiscard]] Status ResolveItemDeform(const RenderSnapshot& snapshot, size_t index);
		// M23 (C23.3): the DeformedMeshes entry of the resolved, deformed ItemDeforms[index], acquired once per render: the
		// first entry of the item's Entity and Mesh this render has not acquired, or a new one with the next VertexStream, its
		// buffers created or grown for the item's mesh (a new mesh version deforms afresh), its index added to frame.Deforms.
		// Errors, with the item Skipped: those of GraphicsDevice::CreateBuffer (Gpu), naming the entity.
		[[nodiscard]] Result<size_t> AcquireDeformedMesh(Detail::SceneRendererFrame& frame, size_t index);
		// M23 (C23.3): what the draws of snapshot items `indices` bind (SkinningPass.h's DeformedVertexSource), acquiring
		// the deformed ones' entries (their errors kept in frame), for the shadow casters and the selection mask.
		[[nodiscard]] std::vector<DeformedVertexSource> PrepareVertexSources(Detail::SceneRendererFrame& frame, std::span<const size_t> indices);
		// M23 (C23.3): the draws of the DeformedMeshes entry `entry` are not recorded this render (its dispatch failed): the
		// camera's draws of its VertexStream, and the shadow casters' and the selection's batches that bind its vertices.
		void SkipDeformedDraws(Detail::SceneRendererFrame& frame, const Detail::SceneRendererDeformedMesh& entry);
		// M23 (C23.3): releases the DeformedMeshes entries the last render did not acquire, and the render's ItemDeforms.
		void ReleaseUnusedDeforms();
		// Passes 4, 7 and 9: `draws` with the pipelines of `pass` (the forward ones of `view`) and the view set `viewSet`, one
		// instanced draw per batch of `draws` (M23, R-INST).
		// M42: with `target`, a mirror view's draws into its framebuffer and scissor, added to no SceneRenderStats count.
		void RecordMeshPass(nvrhi::ICommandList& commandList, Detail::SceneRendererMeshPass pass, std::span<const Detail::SceneRendererDraw> draws, nvrhi::IBindingSet& viewSet,
			RenderDebugView view, RenderPassCounters& counters, const Detail::SceneRendererMeshTarget* target = nullptr);
		// Drops what the render did not use from every binding-set cache.
		void ReleaseUnusedBindings();
		// M23 (C23.1, R-INST): grows InstanceBuffer, the view's Instances structured buffer (null after a device loss's
		// release, so the next Render creates it again), to hold Instances (at least one record, so every set binds a buffer).
		// Errors: Gpu when the buffer cannot be created (the previous one stays).
		[[nodiscard]] Status EnsureInstanceBuffer();
		// What Render records between the Prepare stage and the Deform stage: the per-view constants the Prepare stage made
		// (frame's View, Environment and Shadow, the packed Lights and the instance records), then the clears of SceneColor (the camera's
		// ClearColor), SceneDepth (0, the far plane), SceneNormals and, for a picking view, EntityId.
		void RecordViewSetup(Detail::SceneRendererFrame& frame);

		// M18 (C18.1, decision 1): Render's stage functions, one per RenderStage, called in that order between
		// RenderRecordingContext::BeginStage and EndStage; each records its stage's declared passes in their declared order,
		// or none, keeping partial errors in frame.FirstError. Stream A owns Prepare to LdrPost (SceneRenderer.cpp), stream F
		// SceneOverlay, Output and EditorOverlay (SceneRendererOutput.cpp).
		// Prepare (CPU): validates the snapshot, culls and sorts the draws, packs the lights, plans the shadows, writes the
		// per-view constants and ensures the frame's targets at the scene resolution. An error returned here is one of
		// Render's errors before recording: Render returns it and records nothing.
		[[nodiscard]] Status RecordPrepareStage(Detail::SceneRendererFrame& frame);
		// M23 (C23.3, decision 27; stream C): the Skinning pass, one dispatch per entry of frame.Deforms in one Record (one
		// pass scope for the stage, W23.3's integration), each writing the item's DeformedMeshes entry; an entry whose
		// dispatch fails keeps its error in frame.FirstError and its draws are skipped this render (SkipDeformedDraws).
		// Records nothing without a deformed item.
		void RecordDeformStage(Detail::SceneRendererFrame& frame);
		void RecordShadowsStage(Detail::SceneRendererFrame& frame);
		void RecordDepthNormalStage(Detail::SceneRendererFrame& frame);
		// M26 (C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 22, stream T-C): the end of the DepthNormal stage,
		// whenever the mesh prepass records: with terrains in the snapshot, prepares them (TerrainPass::Prepare into
		// TerrainResources, its first error kept) and records the TerrainDepthNormal row into the prepass's framebuffer, or the
		// picking prepass's when picking, with the view's terrain prepass set (ViewBindings).
		void RecordTerrainDepthNormal(Detail::SceneRendererFrame& frame);
		void RecordAmbientStage(Detail::SceneRendererFrame& frame);
		// M23 (C23.1, decision 5; stream C): the end of the Ambient stage. Computes frame.Clusters at the scene resolution,
		// makes LightClusterConstantsBuffer and grows LightClusterBuffer to the grid (EnsureLightClusterBuffers), then records
		// the LightClusters pass, which writes the clustered constants, when Quality.ClusteredLights is on, the light list
		// holds a point or spot light (with directional lights alone every cluster would hold the whole list) and a pass reads
		// the masks (the Lit view's forward passes with a camera, or the LightCount view); otherwise, or when the pass cannot be
		// recorded (its error kept), it writes the brute-force constants. Without both buffers (their creation error kept) it
		// writes nothing, and the forward passes and the data views' composite, which bind them, are skipped.
		void RecordLightClusters(Detail::SceneRendererFrame& frame);
		// LightClusterConstantsBuffer when it is null, and LightClusterBuffer when it is null or holds fewer than
		// `clusterCount` clusters of `wordCount` words (LightClusterPass's descs; M29: the grid's WordCount), each replacing
		// the member only when its creation succeeded. Errors: those of GraphicsDevice::CreateBuffer (Gpu).
		[[nodiscard]] Status EnsureLightClusterBuffers(uint32_t clusterCount, uint32_t wordCount);
		// The planar reflections' mirror views first (RecordPlanarReflections), then the main view's forward passes.
		void RecordForwardStage(Detail::SceneRendererFrame& frame);
		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): the forward set 0 of a view (Private/ForwardPipelines.h)
		// with the frame's shadows, environment, probes and dissolve noise, and the view's own `viewConstants`, `fog`, `lights`,
		// light clusters (`clusterConstants`, `clusterMasks`), occlusion and linear depth, `sceneNormals` and the planar
		// reflections it samples (the neutral black texture for a slot without one).
		[[nodiscard]] nvrhi::BindingSetDesc MakeForwardSetDesc(const Detail::SceneRendererFrame& frame, nvrhi::IBuffer* viewConstants, nvrhi::IBuffer* fog,
			nvrhi::IBuffer* lights, nvrhi::IBuffer* clusterConstants, nvrhi::IBuffer* clusterMasks, nvrhi::ITexture* occlusion, nvrhi::ITexture* linearDepth,
			nvrhi::ITexture* sceneNormals, const std::array<nvrhi::ITexture*, MaxPlanarReflectionsPerView>& planarReflections) const;
		// M42 (decision 3; SceneRendererReflections.cpp): the end of the Prepare stage, after the main view's draws, shadow
		// casters and selection: the snapshot's planar reflections that the view draws, ranked and capped
		// (RankPlanarReflections; the drops logged once per renderer), each prepared in its slot (PrepareReflectionSlot) or,
		// when its objects or the view's probe constants cannot be created, falling back (the error kept, counted as dropped);
		// then the main view's records of a drawn reflection's entity flagged with its slot (DrawFlagPlanarReflection) and
		// frame.PlanarReflections written.
		// In a view other than a Lit view with a camera outside the wireframe, every reflection is culled.
		void PreparePlanarReflections(Detail::SceneRendererFrame& frame);
		// M42: the mirror view of snapshot reflection `item` (its world plane `plane`, its glass's rectangle `rect` on the main
		// view) in `slot`: its camera, scissor, GPU objects (EnsureReflectionSlot), draws (PrepareViewDraws), light list,
		// constants, fog and cluster grid. Errors: those of EnsureReflectionSlot, with the slot not drawn.
		[[nodiscard]] Status PrepareReflectionSlot(Detail::SceneRendererFrame& frame, Detail::SceneRendererReflectionSlot& slot, size_t item,
			const PlanarReflectionPlane& plane, const PlanarReflectionScreenRect& rect);
		// M42: `slot`'s GPU objects for a `width` x `height` reflection of `mipCount` levels: its colour (replaced at another
		// size or chain), the scratch of that size (found or created), its framebuffers, its constant buffers and light list,
		// and its cluster buffers grown to `clusters`. Errors: those of the pool, the framebuffer and the buffer creations (Gpu).
		[[nodiscard]] Status EnsureReflectionSlot(Detail::SceneRendererReflectionSlot& slot, uint32_t width, uint32_t height, uint32_t mipCount,
			const LightClusterGrid& clusters);
		// M42: the Forward stage's mirror views, slot by slot, each under its one row PlanarReflection<k>, which times its whole
		// view: the clears, the prepass and TerrainDepthNormal, the slot's light clusters (LightClusterPass::RecordInRow), then
		// ForwardOpaque, TerrainForward, the skybox, ForwardTransparent, the particles and the mip chain. A slot's error is the
		// render's; its reflection then holds what was recorded.
		void RecordPlanarReflections(Detail::SceneRendererFrame& frame);
		// M42: releases the slots the last render did not draw and the scratch it did not use, and the drawn slots' unused
		// terrain resources; called at the render's end.
		void ReleaseUnusedReflections();
		// M26 (C26.1, R-SSR; Docs/Decisions/0037-m26-contract.md decision 18, stream X-D): the SSR row, after the skybox and before
		// the transparent pass, with frame.Ssr: SsrPass::Record of the view's SceneDepth, SceneNormals, SceneRoughness, depth
		// pyramid, SceneColor (with SsrFramebuffer) and SsrColor, and the forward passes' specular inputs (the view's
		// constants, frame's occlusion, its depth and environment cube, and the reflection probes' cubes or the neutral array).
		// A record error is the render's, and SceneColor keeps the forward passes' image.
		void RecordSsr(Detail::SceneRendererFrame& frame);
		// M47 (Docs/Decisions/0065-m47-water.md decision 3): with frame.Water, after the skybox and before SSR: the copy of
		// SceneColor into WaterRefraction (row WaterRefraction), then WaterPass::RecordSurface (row Water) with the forward set
		// `forwardDesc` with SceneNormals and the ambient occlusion swapped for the neutral textures, the depth pyramid and the
		// surfaces' framebuffer (the picking one with frame.Picking); frame.WaterSurfaces counts the snapshot's waters when it
		// succeeds. An error is the render's, and nothing of the water is drawn.
		void RecordWater(Detail::SceneRendererFrame& frame, const nvrhi::BindingSetDesc& forwardDesc);
		// M47: WaterRefraction at the scene resolution, the surfaces' framebuffer (with `picking` the picking one, with EntityId),
		// and the depth pyramid's ViewDepth when GTAO and SSR did not create it, each created when missing. Errors: those of the
		// pool and the framebuffer creations (Gpu).
		[[nodiscard]] Status EnsureWaterTargets(bool picking);
		// M26 (stream X-D): SsrColor and SsrFramebuffer at the scene resolution (Targets), and the depth pyramid's ViewDepth when
		// GTAO did not create it, each created when missing. M32: SsrColor at the trace image's size (SsrPass::GetTraceSize with
		// `halfResolution`), replaced when it has another size; the tile list when missing or too small for the trace image's
		// tiles; the arguments buffer when missing. Errors: those of the pool, the framebuffer and the buffer creations (Gpu,
		// OutOfMemory after its trim and retry).
		[[nodiscard]] Status EnsureSsrTargets(bool halfResolution);
		// M32 (decision 13): the SSR area of the render the timing slot `slot` last held, which has completed: its readback's
		// counts when that render recorded SSR, unavailable when it did not (or the map failed); nullopt when no render used
		// the slot since its last collection. Clears the slot's flags.
		[[nodiscard]] std::optional<RenderSsrAreaSample> CollectSsrArea(uint32_t slot);
		// M21's Sprites pass (Docs/Decisions/0032-m21-contract.md decision 4): in a Lit view with a camera (not the wireframe),
		// the snapshot's sprites in its draw order, one QuadBatcher quad each in world space (Asset/SpriteGeometry.h's quad of
		// the frame or texture this stage resolves through the GpuResourceCache, the camera's view-projection, the Point
		// sampler, Color as the quad's colour), with the Hdr pipeline into SceneColor depth-tested against SceneDepth, under
		// the Sprites row; a sprite whose quad lies wholly outside the view is culled. C21.2's tilemaps draw in the same order,
		// each before Sprites[DrawPosition], their tiles' quads appended by AppendTilemapQuads. C21.2 (decision 22): when a
		// drawn quad is lit, the Lights2D pass first (RecordLights2D), then the quads in runs of one lit-ness
		// (RecordSpriteQuads). M21's milestone review: at most SceneRenderer::MaxTileQuadsPerView tiles a render (the rest are
		// counted in Stats.DroppedTiles and logged once per renderer); a render without drawn 2D items releases every tilemap
		// cache, and every render releases its outsized quad vectors (ReleaseOutsizedSpriteQuads).
		// M26 (C26.1, decision 7): the stage then draws the particles (RecordParticles), whatever the sprites drew.
		void RecordSpritesStage(Detail::SceneRendererFrame& frame);
		// The sprites and tilemaps of RecordSpritesStage, with C21.2's 2D lights (the Sprites and Lights2D rows).
		void RecordSpritesAndTilemaps(Detail::SceneRendererFrame& frame);
		// M26 (C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decision 7): the Particles row, in a Lit view with a camera
		// (not the wireframe) whose snapshot holds particles: M33's flipbooks resolved into ParticleFlipbooks
		// (Docs/Decisions/0048-m33-particle-look.md decision 5: each emitter's Sheet through the AssetManager, a missing or
		// failed one reported as the sprites' are, and its texture's size through the GpuResourceCache), PlanParticlePass of
		// the snapshot's ParticleEmitters and Particles with the frame's camera into ParticlePlan, then the Particles pass's
		// Record into ParticleColorFramebuffer and, with frame.Taa, ParticleVelocityFramebuffer, with the view's constants,
		// SceneDepth, ParticleBindings, the Linear sampler and the Point one in pixel-art mode (ParticlePass.h). A plan without
		// quads records nothing (no row). A planning or recording error is kept as frame's error; the plan keeps room for at
		// most ParticlePass::MaxQuadsPerBatch quads afterwards.
		void RecordParticles(Detail::SceneRendererFrame& frame);
		// M33 (decision 5): ParticleFlipbooks for `emitters`: empty when none has a Sheet, else one per emitter, each with a
		// Sheet resolved to its texture and its range's texture rectangles (none for a sheet that is missing, fails to load or
		// whose range does not fit it).
		void ResolveParticleFlipbooks(std::span<const ParticleEmitterItem> emitters);
		// C21.2 (decision 22): appends `quad` to SpriteQuads and its lit-ness to SpriteQuadLit, in the 2D draw order.
		void AddSpriteQuad(const QuadDraw& quad, bool lit);
		// M21 (C21.2, decision 24): appends to SpriteQuads the quads of `tilemap`'s tiles that the Sprites stage draws, in the
		// tilemap's draw order (Asset/TilemapDrawOrder.h): every tile of each chunk whose bounds (TilemapCaches, rebuilt when
		// stale) meet the clip volume of `viewProjection`, each the quad of the sprite TilemapRendererComponent.h describes,
		// its sheet resolved through `resolveSheet` (the stage's sheets of the render: null for one that is missing or fails
		// to load, reported there). A tilemap that is missing or fails to load draws nothing and is reported with
		// GetOrPlaceholder's codes; a non-finite world matrix or PixelsPerUnit draws nothing and is kept as frame's error.
		// M21's milestone review: once the render has drawn MaxTileQuadsPerView tiles (TileQuadsDrawn), a further tile adds no
		// quad and counts in TileQuadsDropped.
		void AppendTilemapQuads(Detail::SceneRendererFrame& frame, const TilemapItem& tilemap, const glm::mat4& viewProjection,
			const std::function<const SpriteSheetData*(AssetHandle)>& resolveSheet);
		// M21 (C21.2, decision 24): drops the tilemap caches the render did not draw, and clears the others' Used flags.
		void ReleaseUnusedTilemaps();
		// M21's milestone review: releases each of the Sprites stage's quad vectors (and its chunk flags) whose capacity a render
		// grew past QuadBatcher::MaxQuadsPerRecord quads, so an outsized render keeps no memory after it.
		void ReleaseOutsizedSpriteQuads();
		// C21.2: the Lights2D targets and their framebuffers at the scene resolution (Targets), created when missing.
		// Errors: those of the pool and the framebuffer creation (Gpu, OutOfMemory after its trim and retry).
		[[nodiscard]] Status EnsureLights2DTargets();
		// C21.2, the Lights2D row, recorded when a quad of SpriteQuads is lit: Lights2DNormals cleared to the flat normal and,
		// when a lit quad has a normal map, every lit quad's normal drawn into it in draw order (the QuadBatcher's Normals
		// pipeline); then Lights2DLight cleared and the lights the view draws added (Lights2DPass: the snapshot's Lights2D that
		// reach the view of `viewProjection`, in their order, at most MaxVisibleLights2D, the first skip logged once per
		// renderer). Returns the light accumulation the lit quads sample, or null when no quad is lit, or when its targets or
		// its pass failed (the error kept in frame.FirstError).
		[[nodiscard]] nvrhi::ITexture* RecordLights2D(Detail::SceneRendererFrame& frame, const glm::mat4& viewProjection);
		// C21.2, the Sprites row: SpriteQuads in their order, each run of one lit-ness one QuadBatcher record (Hdr, or HdrLit
		// sampling `light`); a lit run without `light` is not drawn (RecordLights2D kept its error).
		void RecordSpriteQuads(Detail::SceneRendererFrame& frame, const glm::mat4& viewProjection, nvrhi::ITexture* light);
		// M26's Taa pass (RecordTaa), then Bloom, then M26's Exposure pass (RecordExposure).
		void RecordHdrPostStage(Detail::SceneRendererFrame& frame);
		// M26 (C26.1, R-TAA; decisions 10 and 17, stream X-C): the TAA row, with frame.Taa: Targets.TaaHistory created when
		// missing (EnsureTaaHistory), then the Taa pass resolving SceneColor against the view's history (from scratch when the
		// snapshot's history was reset, CameraData::PreviousViewProjection empty, or TaaHistoryValid is false) into the other
		// history image, which it copies back into SceneColor for bloom, exposure and the tonemap. Sets TaaHistoryIndex and
		// TaaHistoryValid; without frame.Taa only clears TaaHistoryValid. A creation or record error is the render's, and the
		// chain then reads the unresolved SceneColor.
		void RecordTaa(Detail::SceneRendererFrame& frame);
		// M26 (stream X-C): Targets.TaaHistory at the scene resolution when they are missing, clearing TaaHistoryValid.
		// Errors: those of the pool (Gpu, OutOfMemory after its trim and retry).
		[[nodiscard]] Status EnsureTaaHistory();
		// M26 (C26.1, R-AEX; decisions 10 and 15, stream X-A): the Exposure row, with ExposureMode Auto in a Lit view outside the
		// wireframe and pixel-art mode (where Auto acts as Manual), from the view's exposure buffers (EnsureExposureBuffers),
		// resetting the adapted exposure when the snapshot's history was reset (CameraData::PreviousViewProjection empty) or
		// ExposureValid is false; sets frame.AdaptedExposure and ExposureValid. A creation or record error is the render's,
		// and the Tonemap stage then exposes manually.
		void RecordExposure(Detail::SceneRendererFrame& frame);
		// M26 (stream X-A): ExposureHistogramBuffer and ExposureStateBuffer when they are null (ExposurePass's descs), each
		// replacing the member only when its creation succeeded. Errors: those of GraphicsDevice::CreateBuffer.
		[[nodiscard]] Status EnsureExposureBuffers();
		// Tonemap (or the data views' composite); with frame.PixelArt the 2D colour path (decision 7).
		void RecordTonemapStage(Detail::SceneRendererFrame& frame);
		// FXAA, never with frame.PixelArt or a debug view.
		void RecordLdrPostStage(Detail::SceneRendererFrame& frame);
		// Game Text (world, then screen), then M22's Ui (UiPass.h), at the scene resolution into the scene LDR target.
		void RecordSceneOverlayStage(Detail::SceneRendererFrame& frame);
		// The upscale when frame.Output is not the identity (UpscalePass); sets frame's ViewColor, ViewOverlayFramebuffer and
		// ViewOverlayConstants either way.
		void RecordOutputStage(Detail::SceneRendererFrame& frame);
		// Wireframe, the selection outline, the debug lines with annotations, grid and icons, then the labels, at the
		// renderer's size into frame.ViewOverlayFramebuffer. When frame.Output is not the identity they draw over the whole
		// view with the overlay camera (the scene camera whose projection maps the scene's clip volume onto the upscaled
		// scene rectangle, partly outside the view for a cropped scene), so inside the rectangle every overlay lands where
		// that rectangle's viewport would put it; then, when the mapping has a letterbox, the letterbox repaint (the
		// "Letterbox" row, UpscalePass::RecordLetterbox) gives the bars their letterbox colour again, which clips the
		// overlays to the rectangle as its scissor, intersected with the view, would.
		void RecordEditorOverlayStage(Detail::SceneRendererFrame& frame);
	};

}
