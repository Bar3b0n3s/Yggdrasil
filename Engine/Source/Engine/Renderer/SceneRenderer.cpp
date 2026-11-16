#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/SpriteGeometry.h"
#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Asset/TilemapData.h"
#include "Engine/Asset/TilemapDrawOrder.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/CommandListRecording.h"
#include "Engine/Graphics/GpuOutOfMemory.h"
#include "Engine/Graphics/GpuProfiler.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/BloomPass.h"
#include "Engine/Renderer/BrdfLut.h"
#include "Engine/Renderer/DebugRenderer.h"
#include "Engine/Renderer/EditorOverlay.h"
#include "Engine/Renderer/ExposurePass.h"
#include "Engine/Renderer/FxaaPass.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/Light2DShading.h"
#include "Engine/Renderer/LightClusterPass.h"
#include "Engine/Renderer/Lights2DPass.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/Private/ForwardPipelines.h"
#include "Engine/Renderer/Private/LightingInputs.h"
#include "Engine/Renderer/Private/MaterialBindingCache.h"
#include "Engine/Renderer/Private/ProbeShading.h"
#include "Engine/Renderer/Private/SceneRendererState.h"
#include "Engine/Renderer/RenderPassDeclaration.h"
#include "Engine/Renderer/RenderPrepare.h"
#include "Engine/Renderer/RenderStage.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Engine/Renderer/SkyboxPass.h"
#include "Engine/Renderer/SsrPass.h"
#include "Engine/Renderer/TaaPass.h"
#include "Engine/Renderer/TerrainPass.h"
#include "Engine/Renderer/TextRenderer.h"
#include "Engine/Renderer/TonemapPass.h"
#include "Shared/DrawConstants.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/InstanceRecord.h"
#include "Shared/LightClusterConstants.h"
#include "Shared/Private/SceneDataViewConstants.h"
#include "Shared/ShaderLight.h"
#include "Shared/ShadowConstants.h"
#include "Shared/ViewConstants.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
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
// the reads of earlier submissions), and M26's probe resources (Private/ProbeShading.h: the ProbeConstants buffer, written
// by every Render, and the copies of the probes' mirrors, made when the view's probes change); one PassBindingCache for the
// view's own set-0 sets and one per shared pass, each keeping exactly what the last Render used, and the material sets
// (Private/MaterialBindingCache.h), which also keep the sets of the materials the last Render culled (R-MATSET). A Render
// with a new generation of the pipeline set clears them all first.
//
// Render follows SceneRenderer.h's pass list as stage functions (Docs/Decisions/0029-m18-contract.md decision 1), one per
// RenderStage, handing over through Detail::SceneRendererFrame. The Prepare stage resolves every draw before recording:
// its mesh and material mirrors (GpuResourceCache), its pipeline variant (the material's alpha mode and double-sidedness,
// the world matrix's mirroring) and its material set, then sorts the opaque draws (Opaque and Mask) by (forward variant,
// material, mesh, submesh, snapshot order) and the transparent ones (Blend) back to front; the prepass draws the opaque ones
// in the same order with its own variants. Render records the per-view constants and the target clears between the Prepare
// and Deform stages, then the GPU stages.
//
// Instancing (M23, R-INST; Docs/Decisions/0034-m23-contract.md decision 2): every mesh draw is instanced. The Prepare stage
// writes one instance record (Shared/InstanceRecord.h: world matrix, previous world matrix, pick id, draw flags) per
// submesh draw of the view into Instances, in this order: the opaque draws in their order, the transparent draws in theirs,
// the shadow casters (ShadowPass::PrepareCasters) and the selection mask's draws (SelectionPass::PrepareDraws); it splits
// each list into maximal runs of adjacent draws with equal keys (GroupMeshInstances: pipeline variant, material, mesh,
// submesh, vertex stream), whose records are therefore adjacent, and RecordViewSetup writes the records once into the
// view's Instances structured buffer (t10 of set 0, InstanceBuffer, grown by the Prepare stage). Each run is one draw of
// its length in instances whose DrawConstants::FirstInstance is its first record. Vulkan rasterizes the instances of a draw
// in order, so the image equals the one RenderQualitySettings::Instancing false records with one draw per submesh draw
// (the reference path), byte for byte; RenderPassStats::DrawCalls counts the instanced draws and SceneRenderStats::MeshDraws
// the submesh draws.
//
// Temporal anti-aliasing (M26, C26.1, R-TAA-VEL, R-TAA; Docs/Decisions/0037-m26-contract.md decisions 10, 11, 14 and 17;
// stream X-C). The prepass and picking prepass write SceneVelocity (the motion vectors, from the instance records'
// PreviousWorld, a deformed item's previous positions and the view's previous unjittered view-projection) and
// SceneRoughness (SSR's specular inputs) after SceneNormals and EntityId, cleared to zero first. A render with TAA (frame.Taa)
// rasterizes its scene passes with the snapshot's jitter, which the Prepare stage folds into the ViewConstants' projection
// (MakeViewConstants), while the passes after the HDR post chain (game Text, the Ui, the editor overlays at the identity
// mapping) draw with the unjittered constants of frame.OverlayView, so they never shake; the Taa pass then resolves
// SceneColor against the view's TaaHistory first in HdrPost (RecordTaa).
//
// Deformation (M23, C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md decision 27). The Prepare stage resolves each
// item it looks at (ResolveItemDeform) from its mesh's mirror: an item is deformed when its mesh has a skin its JointPalette
// covers, or morph targets with a resolved weight that is not 0 (MeshDrawItem's rule). A deformed item's submeshes are culled
// by its deformed bounds (ComputeDeformedBounds, in its local space), and when one of its draws survives, the camera's, a
// shadow caster's or the selection mask's, the item acquires its DeformedMeshes entry (AcquireDeformedMesh: keyed by Entity
// and Mesh, with buffers of the mesh's size and a VertexStream of its own), whose deformed vertices its draws bind as vertex
// binding 0 in every pass, keying that VertexStream so they never batch with another item's. The Deform stage records one
// Skinning dispatch per acquired entry (frame.Deforms, in snapshot order) before any pass draws, and the render releases the
// entries it did not acquire at its end.

namespace Engine {

	// The startup count is the sum of the pipelines each part of the set declares (SceneRenderer.h).
	static_assert(SceneRendererPipelines::StartupPipelineCount
		== SceneRendererPipelines::MeshPipelineCount + SkyboxPass::PipelineCount + BloomPass::PipelineCount + TonemapPass::PipelineCount
			+ FxaaPass::PipelineCount + DebugRenderer::PipelineCount + TextRenderer::PipelineCount + BrdfLut::PipelineCount
			+ ShadowPass::PipelineCount + DepthPyramidPass::PipelineCount + GtaoPass::PipelineCount + LightClusterPass::PipelineCount
			+ SelectionPass::PipelineCount + 1 + QuadBatcher::PipelineCount + UpscalePass::PipelineCount + SkinningPass::PipelineCount + Lights2DPass::PipelineCount
			+ ExposurePass::PipelineCount + TaaPass::PipelineCount + SsrPass::PipelineCount + TerrainPass::PipelineCount
			+ ParticlePass::PipelineCount);
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

		// M23 (R-PLS): the largest radius of a point shadow's filter kernel, in face UV units (PointShadowConstants'
		// DepthSoftness.w; Common/Shadows.slang's SamplePointShadow), about 4.6 degrees from the light.
		constexpr float PointShadowMaxFilterRadius = 0.04f;

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

		// The clip volume's planes as (a, b, c, d) with a point (x, y, z, w) inside when ax + by + cz + dw >= 0: -w <= x <= w,
		// -w <= y <= w and 0 <= z <= w (reverse-Z, §8.3).
		constexpr std::array<glm::vec4, 6> ClipVolumePlanes = { {
			{ 1.0f, 0.0f, 0.0f, 1.0f },
			{ -1.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 1.0f, 0.0f, 1.0f },
			{ 0.0f, -1.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 1.0f, 0.0f },
			{ 0.0f, 0.0f, -1.0f, 1.0f },
		} };

		// Whether the quad of clip-space corners `clip` lies wholly outside one plane of the clip volume, so it covers no pixel
		// (the Sprites stage's culling).
		static bool IsOutsideClipVolume(const std::array<glm::vec4, 4>& clip)
		{
			return std::ranges::any_of(ClipVolumePlanes, [&clip](const glm::vec4& plane)
			{
				return std::ranges::all_of(clip, [&plane](const glm::vec4& point)
				{
					return glm::dot(plane, point) < 0.0f;
				});
			});
		}

		// The Sprites stage's report of sprite sheet `handle`, which is missing, fails to load (`error`) or is not a sprite
		// sheet (`error` null): GetOrPlaceholder's codes (AssetManager.h), so every manager reports alike, the manager logging
		// each once. Sheets have no placeholder: the sprites draw nothing.
		static void ReportSpriteSheetFailure(AssetManager& assets, AssetHandle handle, const Error* error)
		{
			AssetDiagnostic diagnostic;
			diagnostic.Severity = DiagnosticSeverity::Error;
			diagnostic.Asset = handle;
			diagnostic.Path = assets.GetReferencePath(handle);
			if (error == nullptr || error->GetCode() == ErrorCode::InvalidArgument)
			{
				diagnostic.Code = std::string(AssetTypeMismatchCode);
				diagnostic.Message = std::format("asset {} cannot be used as a SpriteSheet; its sprites draw nothing", handle.ToString());
				diagnostic.Hint = "assign an asset of type SpriteSheet";
			}
			else if (error->GetCode() == ErrorCode::NotFound)
			{
				diagnostic.Code = std::string(AssetMissingCode);
				diagnostic.Message = std::format("sprite sheet {} is missing; its sprites draw nothing", handle.ToString());
				diagnostic.Hint = "restore the asset, or assign another one";
			}
			else
			{
				diagnostic.Code = std::string(AssetImportFailedCode);
				diagnostic.Message = std::format("sprite sheet {} failed to load; its sprites draw nothing: {}", handle.ToString(),
					Error(*error).WithHint(std::string()).ToString());
				diagnostic.Hint = error->GetHint();
			}
			assets.ReportDiagnostic(std::move(diagnostic));
		}

		// The Sprites stage's report of tilemap `handle` (M21, C21.2, decision 24), which is missing, fails to load (`error`) or
		// is not a tilemap (`error` null), with GetOrPlaceholder's codes as a sheet's: tilemaps have no placeholder, so it draws
		// nothing.
		static void ReportTilemapFailure(AssetManager& assets, AssetHandle handle, const Error* error)
		{
			AssetDiagnostic diagnostic;
			diagnostic.Severity = DiagnosticSeverity::Error;
			diagnostic.Asset = handle;
			diagnostic.Path = assets.GetReferencePath(handle);
			if (error == nullptr || error->GetCode() == ErrorCode::InvalidArgument)
			{
				diagnostic.Code = std::string(AssetTypeMismatchCode);
				diagnostic.Message = std::format("asset {} cannot be used as a Tilemap; its tilemap renderers draw nothing", handle.ToString());
				diagnostic.Hint = "assign an asset of type Tilemap";
			}
			else if (error->GetCode() == ErrorCode::NotFound)
			{
				diagnostic.Code = std::string(AssetMissingCode);
				diagnostic.Message = std::format("tilemap {} is missing; its tilemap renderers draw nothing", handle.ToString());
				diagnostic.Hint = "restore the asset, or assign another one";
			}
			else
			{
				diagnostic.Code = std::string(AssetImportFailedCode);
				diagnostic.Message = std::format("tilemap {} failed to load; its tilemap renderers draw nothing: {}", handle.ToString(),
					Error(*error).WithHint(std::string()).ToString());
				diagnostic.Hint = error->GetHint();
			}
			assets.ReportDiagnostic(std::move(diagnostic));
		}

		// One palette entry of a tilemap as its tiles draw in a render (M21, C21.2, decision 24): the quad of the sprite they
		// draw as (SpriteRendererComponent.h's one rule), HasQuad false for an entry whose tiles draw nothing (a null, missing
		// or failed sheet, a Frame the sheet lacks), and Draws false also when not even its texture's placeholder exists.
		struct TilemapPaletteQuad
		{
			bool HasQuad = false;
			bool Draws = false;
			std::array<glm::vec2, 4> Corners{};
			glm::vec2 TexCoordMin = glm::vec2(0.0f);
			glm::vec2 TexCoordMax = glm::vec2(1.0f);
			nvrhi::ITexture* Texture = nullptr;
		};

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

		// The ViewConstants of `camera` for a `width` x `height` target (§8.3, Shared/ViewConstants.h). M26 (stream X-C): its
		// projection jittered by `jitter` scene pixels (+x right, +y down; (0, 0) without TAA) as the clip-space translation J,
		// x' = x + 2 jitter.x / width * w and y' = y - 2 jitter.y / height * w, which moves every rasterized point by `jitter`
		// pixels whatever the projection kind and leaves the diagonal (TanHalfFovY, OrthoHalfExtents) unchanged: Projection is
		// J * Projection and ViewProjection J * (Projection * View), no product with J at all without a jitter; Jitter itself;
		// and PreviousViewProjection, J times the camera's previous one when it has a finite one, else ViewProjection itself,
		// so the motion vectors of what did not move are exactly zero (Common/Motion.slang).
		static ViewConstants MakeViewConstants(const CameraData& camera, uint32_t width, uint32_t height, float exposure, glm::vec2 jitter = glm::vec2(0.0f))
		{
			const glm::vec2 viewportSize(static_cast<float>(width), static_cast<float>(height));
			const bool jittered = jitter != glm::vec2(0.0f);
			const glm::mat4 translation = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f * jitter.x / viewportSize.x, -2.0f * jitter.y / viewportSize.y, 0.0f));
			const auto jitterMatrix = [jittered, &translation](const glm::mat4& matrix)
			{
				return jittered ? translation * matrix : matrix;
			};
			ViewConstants view{};
			view.View = camera.View;
			view.Projection = jitterMatrix(camera.Projection);
			view.ViewProjection = jitterMatrix(camera.Projection * camera.View);
			view.InverseProjection = glm::inverse(view.Projection);
			view.InverseViewProjection = glm::inverse(view.ViewProjection);
			view.PreviousViewProjection = camera.PreviousViewProjection.has_value() && IsFinite(*camera.PreviousViewProjection)
				? jitterMatrix(*camera.PreviousViewProjection)
				: view.ViewProjection;
			view.Jitter = jitter;
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

		// M23 (R-PLS): the forward set's neutral point-shadow cube array, one 1 x 1 cube in the shadows' depth format at depth 0
		// (the far plane, so no comparison finds a blocker), bound at t12 while no point light of the view is planned.
		static Result<nvrhi::TextureHandle> CreateEmptyPointShadowMaps(GraphicsDevice& device)
		{
			nvrhi::TextureDesc desc;
			desc.width = 1;
			desc.height = 1;
			desc.arraySize = PointShadowFaceCount;
			desc.dimension = nvrhi::TextureDimension::TextureCubeArray;
			desc.format = ShadowDepthFormat;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = "SceneRenderer.EmptyPointShadowMaps";
			ENGINE_TRY_ASSIGN(nvrhi::TextureHandle texture, device.CreateTexture(desc));
			ENGINE_TRY_ASSIGN(const nvrhi::CommandListHandle list, device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
			CommandListRecording recording(device, *list);
			list->clearDepthStencilTexture(texture, nvrhi::AllSubresources, true, 0.0f, false, 0);
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
		ENGINE_TRY_ASSIGN(created.EmptyPointShadows, Utils::CreateEmptyPointShadowMaps(device));
		// M26 (C26.1, R-GI-PROBE; stream Q-B): the forward set's neutral probe resources.
		ENGINE_TRY_ASSIGN(created.EmptyIrradianceProbes, Utils::CreateEmptyIrradianceProbes(device));
		ENGINE_TRY_ASSIGN(created.EmptyReflectionProbeCubes, Utils::CreateEmptyReflectionProbeCubes(device));
		ENGINE_TRY_ASSIGN(created.WhiteAo, Utils::CreateSceneNeutralTexture(device, AmbientOcclusionFormat, false, 1.0f));
		ENGINE_TRY_ASSIGN(created.FarDepth, Utils::CreateSceneNeutralTexture(device, ViewDepthFormat, false, 65504.0f));
		ENGINE_TRY_ASSIGN(created.ShadowCompare, device.CreateSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp).setReductionType(nvrhi::SamplerReductionType::Comparison)));
		ENGINE_TRY_ASSIGN(created.Shadows, ShadowPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(created.DepthPyramid, DepthPyramidPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(created.Gtao, GtaoPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(created.LightClusters, LightClusterPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(created.Skinning, SkinningPass::Create(device, pipelines));
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
		ENGINE_TRY_ASSIGN(created.Lights2D, Lights2DPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(created.Exposure, ExposurePass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(created.Ssr, SsrPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(created.Taa, TaaPass::Create(device, pipelines));
		// M26 (C26.1, C-TERR; decision 22, stream T-C): its forward pipeline shares the mesh forward passes' set-0 layout.
		ENGINE_TRY_ASSIGN(created.Terrain, TerrainPass::Create(device, pipelines, *created.Layouts.ForwardView));
		// M33 (Docs/Decisions/0048-m33-particle-look.md decision 4): the particles' own program.
		ENGINE_TRY_ASSIGN(created.Particles, ParticlePass::Create(device, pipelines));
		created.PipelineCount = MeshPipelineCount + created.Skybox->GetPipelineCount() + created.Bloom->GetPipelineCount()
			+ created.Tonemap->GetPipelineCount() + created.Fxaa->GetPipelineCount() + created.Debug->GetPipelineCount()
			+ created.Text->GetPipelineCount() + created.DfgLut->GetPipelineCount() + ShadowPass::PipelineCount + DepthPyramidPass::PipelineCount
			+ GtaoPass::PipelineCount + LightClusterPass::PipelineCount + SkinningPass::PipelineCount + SelectionPass::PipelineCount + 1
			+ created.Quads->GetPipelineCount() + created.Upscale->GetPipelineCount() + created.Lights2D->GetPipelineCount()
			+ created.Exposure->GetPipelineCount() + created.Taa->GetPipelineCount() + created.Ssr->GetPipelineCount()
			+ created.Terrain->GetPipelineCount() + created.Particles->GetPipelineCount();

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
		Utils::AppendDescriptions(descriptions, SkinningPass::GetLayoutDescriptions());
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
		Utils::AppendDescriptions(descriptions, LightClusterPass::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, SelectionPass::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, QuadBatcher::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, UpscalePass::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, Lights2DPass::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, ExposurePass::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, SsrPass::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, TaaPass::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, TerrainPass::GetLayoutDescriptions());
		Utils::AppendDescriptions(descriptions, ParticlePass::GetLayoutDescriptions());
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
		// M26 (C26.1, decision 14; stream X-C): the prepass's motion vectors and SSR's specular inputs.
		ENGINE_TRY_ASSIGN(targets.SceneVelocity,
			Utils::CreateTarget(*TargetPool, width, height, SceneVelocityFormat, Utils::TargetUsage::Attachment, "SceneVelocity"));
		ENGINE_TRY_ASSIGN(targets.SceneRoughness,
			Utils::CreateTarget(*TargetPool, width, height, SceneRoughnessFormat, Utils::TargetUsage::Attachment, "SceneRoughness"));
		ENGINE_TRY_ASSIGN(targets.SceneColor,
			Utils::CreateTarget(*TargetPool, width, height, SceneColorFormat, Utils::TargetUsage::Attachment, "SceneColor"));
		ENGINE_TRY_ASSIGN(targets.Ldr[0], Utils::CreateTarget(*TargetPool, width, height, LdrColorFormat, Utils::TargetUsage::LdrColor, "LdrColor"));
		ENGINE_TRY_ASSIGN(targets.Ldr[1], Utils::CreateTarget(*TargetPool, width, height, LdrColorFormat, Utils::TargetUsage::LdrColor, "LdrColorFxaa"));
		ENGINE_TRY_ASSIGN(targets.Bloom, TargetPool->Acquire(BloomPass::GetChainDesc(width, height, Pipelines->Bloom->GetFormat())));

		ENGINE_TRY_ASSIGN(targets.PrepassFramebuffer,
			device.CreateFramebuffer(nvrhi::FramebufferDesc()
					.addColorAttachment(targets.SceneNormals)
					.addColorAttachment(targets.SceneVelocity)
					.addColorAttachment(targets.SceneRoughness)
					.setDepthAttachment(targets.SceneDepth)));
		ENGINE_TRY_ASSIGN(targets.ForwardFramebuffer,
			device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(targets.SceneColor).setDepthAttachment(targets.SceneDepth)));
		// M33 (decision 4): the particles' colour and motion targets, each alone.
		ENGINE_TRY_ASSIGN(targets.ParticleColorFramebuffer, device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(targets.SceneColor)));
		ENGINE_TRY_ASSIGN(targets.ParticleVelocityFramebuffer, device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(targets.SceneVelocity)));
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

	Status SceneRenderer::State::EnsureFrameTargets(bool picking, bool ao, bool halfAo, uint32_t shadowSize, bool spots, uint32_t pointShadowFaceSize,
		bool selection, bool overdraw)
	{
		const uint32_t width = Targets.Width;
		const uint32_t height = Targets.Height;
		if (picking && Targets.EntityId == nullptr)
		{
			ENGINE_TRY_ASSIGN(Targets.EntityId, Utils::CreateTarget(*TargetPool, width, height, EntityIdFormat, Utils::TargetUsage::Attachment, "EntityId"));
		}
		if (picking && Targets.PickingFramebuffer == nullptr)
		{
			ENGINE_TRY_ASSIGN(Targets.PickingFramebuffer, Device->CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(Targets.SceneNormals).addColorAttachment(Targets.EntityId).addColorAttachment(Targets.SceneVelocity).addColorAttachment(Targets.SceneRoughness).setDepthAttachment(Targets.SceneDepth)));
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
		if (pointShadowFaceSize != 0 && (Targets.PointShadowMaps == nullptr || Targets.PointShadowMaps->getDesc().width != pointShadowFaceSize))
		{
			ClearTargetBindings();
			ENGINE_TRY_ASSIGN(nvrhi::TextureHandle maps, TargetPool->Acquire(ShadowPass::GetPointTargetDesc(pointShadowFaceSize)));
			ENGINE_TRY_ASSIGN(std::vector<nvrhi::FramebufferHandle> framebuffers, ShadowPass::CreateFramebuffers(*Device, *maps));
			Targets.PointShadowMaps = std::move(maps);
			Targets.PointShadowFramebuffers = std::move(framebuffers);
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
		SpriteBindings.Clear();
		Lights2DBindings.Clear();
		ExposureBindings.Clear();
		SsrBindings.Clear();
		TaaBindings.Clear();
	}

	void SceneRenderer::State::ClearBindings()
	{
		ClearTargetBindings();
		LightClusterBindings.Clear();
		SkinningBindings.Clear();
		UpscaleBindings.Clear();
		UiQuadBindings.Clear();
		ParticleBindings.Clear();
		TerrainResources.Bindings.Clear(); // M26 (stream T-C): made for the terrain pass's set-1 layout
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
		SkinningBindings.ReleaseUnused();
		LightClusterBindings.ReleaseUnused();
		SpriteBindings.ReleaseUnused();
		Lights2DBindings.ReleaseUnused();
		ExposureBindings.ReleaseUnused();
		SsrBindings.ReleaseUnused();
		TaaBindings.ReleaseUnused();
		UpscaleBindings.ReleaseUnused();
		UiQuadBindings.ReleaseUnused();
		ParticleBindings.ReleaseUnused();
		// M26 (stream T-C): the chunk meshes and terrain sets the render did not use, and its prepared terrains.
		TerrainPass::ReleaseUnused(TerrainResources);

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

	Status SceneRenderer::State::PrepareDraws(Detail::SceneRendererFrame& frame, const RenderSnapshot& snapshot, const CameraData& camera)
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

			// M23 (C23.3): the item's deformation, resolved before its mirror is read (resolving it uploads the mesh too).
			const Status deformResolved = ResolveItemDeform(snapshot, index);
			if (!deformResolved.has_value())
			{
				if (status.has_value())
					status = deformResolved;
				continue;
			}
			const Detail::SceneRendererItemDeform& deform = ItemDeforms[index];

			// The mirror is valid until the next upload (resolving materials uploads textures): what is needed is copied now.
			const GpuMesh& mesh = Cache->GetMesh(item.Mesh);
			if (mesh.VertexBuffer == nullptr || mesh.AttributeBuffer == nullptr || mesh.IndexBuffer == nullptr)
				continue; // not even the placeholder could be created (reported by the cache)
			nvrhi::IBuffer* vertexBuffer = mesh.VertexBuffer.Get();
			nvrhi::IBuffer* attributeBuffer = mesh.AttributeBuffer.Get();
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
				// M23 (C23.3): a deformed item's submeshes are culled by its deformed bounds.
				const Aabb& bounds = deform.Deformed ? deform.Bounds : submesh.Bounds;
				if (IsOutsideView(bounds, item.World, camera))
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
				// M23 (C23.3): a deformed item's draws bind its entry's deformed vertices and key its VertexStream; M26 (stream X-C):
				// and its previous positions, which the prepass's motion vectors read.
				nvrhi::IBuffer* drawVertices = vertexBuffer;
				nvrhi::IBuffer* previousPositions = nullptr;
				uint32_t vertexStream = 0;
				if (deform.Deformed)
				{
					const Result<size_t> entry = AcquireDeformedMesh(frame, index);
					if (!entry.has_value())
					{
						if (status.has_value())
							status = std::unexpected(entry.error());
						break; // the item's draws are skipped
					}
					drawVertices = DeformedMeshes[*entry].Vertices.Get();
					previousPositions = DeformedMeshes[*entry].PreviousPositions.Get();
					vertexStream = DeformedMeshes[*entry].VertexStream;
				}

				const Utils::MeshCullMode cullMode = Utils::SelectCullMode(mirrored, material.DoubleSided);
				Detail::SceneRendererDraw draw;
				draw.VertexBuffer = drawVertices;
				draw.PreviousPositions = previousPositions;
				draw.VertexStream = vertexStream;
				draw.AttributeBuffer = attributeBuffer;
				draw.IndexBuffer = indexBuffer;
				draw.IndexOffset = submesh.IndexOffset;
				draw.IndexCount = submesh.IndexCount;
				draw.Material = material.Set;
				draw.ForwardVariant = Utils::GetForwardVariant(material.Mode, cullMode);
				draw.MaterialHandle = materialHandle;
				draw.Mesh = item.Mesh;
				draw.Entity = item.Entity;
				draw.Submesh = submeshIndex;
				draw.SnapshotIndex = index;
				if (material.Mode == AlphaMode::Blend)
				{
					const glm::vec3 centre = bounds.IsEmpty() ? glm::vec3(0.0f) : (bounds.Min + bounds.Max) * 0.5f;
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

		// §8.3 pass 1: opaque draws by pipeline, then material, then mesh, then submesh (M23: so the draws of one submesh are
		// adjacent and batch), ties in snapshot order; transparent draws back to front, ties by entity UUID (RenderPrepare.h).
		std::vector<OpaqueSortKey> opaqueKeys;
		opaqueKeys.reserve(OpaqueDraws.size());
		for (uint32_t index = 0; index < OpaqueDraws.size(); ++index)
		{
			const Detail::SceneRendererDraw& draw = OpaqueDraws[index];
			opaqueKeys.push_back({ .Pipeline = draw.ForwardVariant, .Material = draw.MaterialHandle, .Mesh = draw.Mesh, .Submesh = draw.Submesh, .DrawIndex = index });
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

		// M23 (R-INST): the draws' instance records in their order, opaque then transparent, and their instanced draws.
		const auto appendInstances = [this, &snapshot](std::vector<Detail::SceneRendererDraw>& draws)
		{
			std::vector<MeshInstanceKey> keys;
			keys.reserve(draws.size());
			Instances.reserve(Instances.size() + draws.size());
			for (Detail::SceneRendererDraw& draw : draws)
			{
				const MeshDrawItem& item = snapshot.Meshes[draw.SnapshotIndex];
				draw.Instance = static_cast<uint32_t>(Instances.size());
				draw.Batch = 1;
				// M26 (stream X-C): a deformed item that continues its view's history (it has a PreviousWorld) moves from its
				// previous deformation; otherwise its motion is its world matrix's alone (none after a reset).
				const uint32_t previousPositions = draw.PreviousPositions != nullptr && item.PreviousWorld.has_value() ? DrawFlagPreviousPositions : 0U;
				Instances.push_back(MakeMeshInstanceRecord(item, (item.ReceiveShadows ? DrawFlagReceiveShadows : 0U) | previousPositions));
				keys.push_back({ .Pipeline = draw.ForwardVariant,
					.Material = draw.MaterialHandle,
					.Mesh = draw.Mesh,
					.Submesh = draw.Submesh,
					.VertexStream = draw.VertexStream });
			}
			if (!snapshot.Quality.Instancing)
				return; // the reference path: one draw per submesh draw
			for (const MeshInstanceBatch& batch : GroupMeshInstances(keys))
			{
				for (uint32_t index = 0; index < batch.Count; ++index)
					draws[batch.First + index].Batch = index == 0 ? batch.Count : 0;
			}
		};
		appendInstances(OpaqueDraws);
		appendInstances(TransparentDraws);
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
		for (size_t index = 0; index < draws.size(); ++index)
		{
			const Detail::SceneRendererDraw& draw = draws[index];
			if (draw.Batch == 0)
				continue; // an instance of the draw its batch's first draw recorded
			ENGINE_CORE_ASSERT(index + draw.Batch <= draws.size() && draws[index + draw.Batch - 1].Instance == draw.Instance + draw.Batch - 1,
				"the instance records of a batch's draws must be adjacent");
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
				// The attribute stream (M23, C-MESH2): the mesh's own, or the cache's shared default stream, read from offset 0.
				state.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(draw.AttributeBuffer).setSlot(Utils::MeshAttributeStreamSlot).setOffset(0));
				// M26 (stream X-C): the prepass variants' previous positions (Private/ForwardPipelines.h): a deformed draw's, else
				// its own vertices, which the shaders do not read as positions.
				if (pass == Detail::SceneRendererMeshPass::Prepass || pass == Detail::SceneRendererMeshPass::Picking)
				{
					nvrhi::IBuffer* previous = draw.PreviousPositions != nullptr ? draw.PreviousPositions : draw.VertexBuffer;
					state.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(previous).setSlot(Utils::PreviousPositionSlot).setOffset(0));
				}
				state.setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(draw.IndexBuffer).setFormat(nvrhi::Format::R32_UINT).setOffset(0));
				commandList.setGraphicsState(state);
				lastPipeline = pipeline.Pipeline.Get();
				lastMaterial = draw.Material;
				lastVertexBuffer = draw.VertexBuffer;
			}
			DrawConstants constants{};
			constants.FirstInstance = draw.Instance;
			commandList.setPushConstants(&constants, sizeof(constants));
			commandList.drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.IndexCount).setInstanceCount(draw.Batch).setStartIndexLocation(draw.IndexOffset));
			++counters.DrawCalls;
			counters.Triangles += draw.IndexCount / 3 * draw.Batch;
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
		// The targets are at the scene resolution the last Render's output settings give the new size (the view's size for the
		// default settings), so the next Render with the same settings keeps them instead of replacing a set it never used (M18's
		// milestone review).
		const RenderOutputMapping mapping = ComputeRenderOutputMapping(width, height, state.OutputSettings);
		Detail::SceneRendererTargets targets;
		const Status created = RetryAfterTrim(*state.Device, std::format("a scene view's resize to {}x{}", width, height), [&state, &targets, &mapping]() -> Status
		{
			ENGINE_TRY_ASSIGN(targets, state.CreateTargets(mapping.SceneWidth, mapping.SceneHeight));
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
		state.OutputSettings = snapshot.Output;
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

		// M26 (W26.2's review): the renders since the view's history started afresh (RenderStats::HistoryFrames).
		const bool continuesHistory = snapshot.HasCamera && snapshot.Camera.PreviousViewProjection.has_value();
		state.HistoryFrames = continuesHistory ? state.HistoryFrames + (state.HistoryFrames < std::numeric_limits<uint32_t>::max() ? 1U : 0U) : 0U;
		RenderStats stats{ .Available = true, .FrameIndex = snapshot.FrameIndex, .Width = state.ViewWidth, .Height = state.ViewHeight, .ShadowMapSize = snapshot.Quality.ShadowMapSize, .SsaoHalfResolution = snapshot.Quality.SsaoHalfResolution, .HistoryFrames = state.HistoryFrames };
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
		// M18 (the milestone review; decision 39): a render that an exception unwinds (a vk::SystemError thrown out of NVRHI, a
		// device loss that an upload's first write meets, on its way to the frame boundary or an automation method's handler,
		// which report the loss) is abandoned: the renderer awaits no OnSubmitted for it, and the profiler whose scopes the
		// unwinding left open (RenderRecordingContext leaves them, RenderStats.h) is replaced, as the device-loss release
		// replaces it, so a render or a capture before the recovery finds the renderer as after any render on the lost device.
		// Declared before the recording context, so it runs after that context's destructor.
		struct AbandonOnUnwind
		{
			State* Renderer = nullptr;
			int UncaughtExceptions = std::uncaught_exceptions();

			~AbandonOnUnwind()
			{
				if (std::uncaught_exceptions() <= UncaughtExceptions)
					return;
				Renderer->PendingSubmission = false;
				Renderer->PendingTimingSlot.reset();
				Renderer->Profiler = CreateScope<GpuProfiler>(*Renderer->Device, Renderer->Device->GetFramesInFlight());
			}
		};
		const AbandonOnUnwind abandonOnUnwind{ .Renderer = &state };
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
		state.ReleaseUnusedDeforms();
		std::set<size_t> visibleMeshes;
		for (const auto& draw : state.OpaqueDraws)
			visibleMeshes.insert(draw.SnapshotIndex);
		for (const auto& draw : state.TransparentDraws)
			visibleMeshes.insert(draw.SnapshotIndex);
		stats.VisibleMeshes = static_cast<uint32_t>(visibleMeshes.size());
		stats.CulledSubmeshes = state.Stats.CulledSubmeshes;
		stats.ShadowedSpotLights = static_cast<uint32_t>(frame.Spots.Tiles.size());
		stats.DroppedSpotShadows = frame.Spots.Dropped;
		stats.ShadowedPointLights = static_cast<uint32_t>(frame.Points.Lights.size());
		stats.DroppedPointShadows = frame.Points.Dropped;
		for (const RenderPassStats& pass : stats.Passes)
			if (pass.Name == "DirectionalShadows" || pass.Name == "SpotShadows" || pass.Name == "PointShadows")
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

	Status SceneRenderer::State::EnsureInstanceBuffer()
	{
		// At least one record, so the sets of a view without mesh draws bind a buffer too; grown geometrically, so a scene that
		// adds draws frame by frame makes few buffers, and never shrunk (a device loss's release drops it).
		constexpr uint64_t MinInstanceCapacity = 64;
		const uint64_t needed = std::max<uint64_t>(Instances.size(), 1);
		const uint64_t capacity = InstanceBuffer != nullptr ? InstanceBuffer->getDesc().byteSize / sizeof(InstanceRecord) : 0;
		if (needed <= capacity)
			return {};
		nvrhi::BufferDesc desc;
		desc.byteSize = std::max({ needed, capacity * 2, MinInstanceCapacity }) * sizeof(InstanceRecord);
		desc.structStride = sizeof(InstanceRecord);
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.Instances";
		ENGINE_TRY_ASSIGN(InstanceBuffer, Device->CreateBuffer(desc));
		return {};
	}

	void SceneRenderer::State::RecordViewSetup(Detail::SceneRendererFrame& frame)
	{
		nvrhi::ICommandList& commandList = *frame.CommandList;
		commandList.writeBuffer(ViewConstantsBuffer, &frame.View, sizeof(frame.View));
		// M26 (stream X-C): the unjittered constants of the passes after the HDR post chain, when the scene passes' are jittered.
		if (frame.OverlayView != ViewConstantsBuffer.Get())
			commandList.writeBuffer(UnjitteredViewConstantsBuffer, &frame.OverlayConstants, sizeof(frame.OverlayConstants));
		commandList.writeBuffer(EnvironmentConstantsBuffer, &frame.Environment, sizeof(frame.Environment));
		commandList.writeBuffer(ShadowConstantsBuffer, &frame.Shadow, sizeof(frame.Shadow));
		if (!Lights.empty())
			commandList.writeBuffer(LightBuffer, Lights.data(), Lights.size() * sizeof(ShaderLight));
		if (!Instances.empty())
			commandList.writeBuffer(InstanceBuffer, Instances.data(), Instances.size() * sizeof(InstanceRecord));
		// M26 (C26.1, R-GI-PROBE; stream Q-B): the probes' constants, and their mirrors' copies when the view's probes changed.
		Utils::RecordProbeUploads(commandList, ProbeResources, frame.Probes);
		const glm::vec3 clearColor = frame.Camera.ClearColor;
		commandList.clearTextureFloat(Targets.SceneColor, nvrhi::AllSubresources, nvrhi::Color(clearColor.r, clearColor.g, clearColor.b, 1.0f));
		commandList.clearDepthStencilTexture(Targets.SceneDepth, nvrhi::AllSubresources, true, 0.0f, false, 0);
		commandList.clearTextureFloat(Targets.SceneNormals, nvrhi::AllSubresources, nvrhi::Color(0.0f));
		// M26 (decision 14): no motion and no specular to replace where nothing is drawn.
		commandList.clearTextureFloat(Targets.SceneVelocity, nvrhi::AllSubresources, nvrhi::Color(0.0f));
		commandList.clearTextureUInt(Targets.SceneRoughness, nvrhi::AllSubresources, 0);
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
		// M26 (stream X-D, decision 18): SSR in the Lit view's shaded image only.
		frame.Ssr = hasCamera && frame.IsLit && !frame.Wireframe && post.SsrEnabled;
		const bool shadows = hasCamera && ((frame.IsLit && !frame.Wireframe) || frame.DebugView == RenderDebugView::ShadowCascades);
		// The Lit view follows Post; the debug views and the 2D colour path (decision 7) fix exposure 1 and no dither.
		const bool tonemapped = frame.IsLit && !frame.PixelArt;

		recording.BeginPass("Prepare");
		// M26 (C26.1, R-TAA; decisions 10 and 17, stream X-C): TAA resolves a Lit view outside the wireframe and pixel-art mode
		// (where Taa acts as None, as FXAA does), whose scene passes then rasterize with the snapshot's jitter; the passes after
		// the HDR post chain draw with the unjittered constants (frame.OverlayView).
		frame.Taa = hasCamera && frame.IsLit && !frame.Wireframe && !frame.PixelArt && post.AntiAliasing == RenderAntiAliasing::Taa;
		const glm::vec2 jitter = frame.Taa && std::isfinite(camera.Jitter.x) && std::isfinite(camera.Jitter.y) ? camera.Jitter : glm::vec2(0.0f);
		const float viewExposure = tonemapped ? Utils::GetExposure(post) : 1.0f;
		frame.View = Utils::MakeViewConstants(camera, Targets.Width, Targets.Height, viewExposure, jitter);
		frame.OverlayView = ViewConstantsBuffer.Get();
		if (jitter != glm::vec2(0.0f))
		{
			if (UnjitteredViewConstantsBuffer == nullptr)
			{
				nvrhi::BufferDesc desc;
				desc.byteSize = sizeof(ViewConstants);
				desc.isConstantBuffer = true;
				desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
				desc.keepInitialState = true;
				desc.debugName = "SceneRenderer.UnjitteredViewConstants";
				Result<nvrhi::BufferHandle> created = Device->CreateBuffer(desc);
				if (created.has_value())
					UnjitteredViewConstantsBuffer = std::move(*created);
				else
					frame.KeepFirstError(std::unexpected(std::move(created).error()));
			}
			if (UnjitteredViewConstantsBuffer != nullptr)
			{
				frame.OverlayConstants = Utils::MakeViewConstants(camera, Targets.Width, Targets.Height, viewExposure);
				frame.OverlayView = UnjitteredViewConstantsBuffer.Get();
			}
			else
				frame.View = Utils::MakeViewConstants(camera, Targets.Width, Targets.Height, viewExposure); // no jitter without them
		}
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
			// M23 (R-PLS): the point lights the same way, in CullLights' importance order (PointShadowMaps.h).
			auto points = AllocatePointShadows(snapshot.Lights, culled.Visible);
			if (!points)
				frame.KeepFirstError(std::unexpected(std::move(points).error()));
			else
			{
				frame.Points = std::move(*points);
				if (frame.Points.Rejected > 0 && rejection.empty())
					rejection = "a point light's range is not above twice its shadow near plane, or its values cannot be projected";
				rejectedLights += frame.Points.Rejected;
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
			// The point entries (Shared/ShadowConstants.h): Bias.z is a face texel's width in face-plane units at unit distance.
			const uint32_t faceSize = GetPointShadowFaceSize(snapshot.Quality.ShadowMapSize);
			frame.Shadow.Counts.w = static_cast<uint32_t>(frame.Points.Lights.size());
			for (size_t index = 0; index < frame.Points.Lights.size(); ++index)
			{
				const PointShadowLight& planned = frame.Points.Lights[index];
				const LightData& light = snapshot.Lights[planned.LightIndex];
				PointShadowConstants& constants = frame.Shadow.Points[index];
				constants.PositionRange = glm::vec4(light.Position, light.Range);
				constants.DepthSoftness = glm::vec4(planned.NearClip, planned.FarClip, light.SourceRadius, Utils::PointShadowMaxFilterRadius);
				constants.Bias = glm::vec4(light.DepthBias, light.NormalBias, 2.0f / static_cast<float>(faceSize), 0.0f);
				constants.Indices = glm::uvec4(static_cast<uint32_t>(std::find(culled.Visible.begin(), culled.Visible.end(), planned.LightIndex) - culled.Visible.begin()),
					planned.Layer, 0, 0);
			}
			if (frame.Points.Dropped > 0 && !PointShadowLimitLogged)
			{
				PointShadowLimitLogged = true;
				ENGINE_CORE_WARN("{}: {} visible point shadows exceed the budget of {} (logged once per view)", RenderPointShadowBudgetCode, frame.Points.Dropped,
					MaxPointShadowLights);
			}
		}
		OpaqueDraws.clear();
		TransparentDraws.clear();
		Instances.clear();
		// M23 (C23.3): this render's deformations, resolved as the passes look at their items.
		++DeformRender;
		ItemDeforms.assign(snapshot.Meshes.size(), Detail::SceneRendererItemDeform{});
		if (hasCamera)
			frame.KeepFirstError(PrepareDraws(frame, snapshot, camera));
		// M23 (R-INST): the shadow casters' and the selection's draws, with their instance records after the camera's. The
		// casters are the full snapshot list (off-screen casters shadow visible receivers), the ones with a finite world matrix;
		// one caster set serves every cascade, spot tile and point face (R-PLS), each culling it against its own frustum.
		if (frame.Cascades.Count != 0 || !frame.Spots.Tiles.empty() || !frame.Points.Lights.empty())
		{
			// M23 (C23.3): a deformed caster draws its deformed vertices; every shadow-casting item is a candidate of every
			// face, so each deformed one is deformed whether or not the camera sees it.
			std::vector<MeshDrawItem> finiteCasters;
			std::vector<size_t> castingPositions; // in finiteCasters
			std::vector<size_t> castingItems;     // in the snapshot
			for (size_t index = 0; index < snapshot.Meshes.size(); ++index)
			{
				const MeshDrawItem& item = snapshot.Meshes[index];
				if (!Utils::IsFinite(item.World))
					continue;
				if (item.CastShadows && item.Mesh.IsValid())
				{
					castingPositions.push_back(finiteCasters.size());
					castingItems.push_back(index);
				}
				finiteCasters.push_back(item);
			}
			std::vector<DeformedVertexSource> sources(finiteCasters.size());
			const std::vector<DeformedVertexSource> castingSources = PrepareVertexSources(frame, castingItems);
			for (size_t position = 0; position < castingItems.size(); ++position)
				sources[castingPositions[position]] = castingSources[position];
			Result<ShadowCasterSet> casters = ShadowPass::PrepareCasters(*Cache, finiteCasters, snapshot.Quality.Instancing, Instances, sources);
			if (casters.has_value())
				frame.ShadowCasters = std::move(*casters);
			else
				frame.KeepFirstError(std::unexpected(std::move(casters).error()));
		}
		if (frame.Selection)
		{
			// M23 (C23.3): a selected deformed item's mask draws its deformed vertices.
			std::vector<size_t> selected;
			for (size_t index = 0; index < snapshot.Meshes.size(); ++index)
			{
				const MeshDrawItem& item = snapshot.Meshes[index];
				if (item.Entity.IsValid() && std::ranges::find(snapshot.SelectedEntities, item.Entity) != snapshot.SelectedEntities.end())
					selected.push_back(index);
			}
			std::vector<DeformedVertexSource> sources(snapshot.Meshes.size());
			const std::vector<DeformedVertexSource> selectedSources = PrepareVertexSources(frame, selected);
			for (size_t position = 0; position < selected.size(); ++position)
				sources[selected[position]] = selectedSources[position];
			frame.SelectionDraws = SelectionPass::PrepareDraws(*Cache, snapshot, snapshot.Quality.Instancing, Instances, sources);
		}
		// M23 (C23.3): the Deform stage dispatches in snapshot order.
		std::ranges::sort(frame.Deforms, [this](size_t left, size_t right)
		{
			return DeformedMeshes[left].SnapshotIndex < DeformedMeshes[right].SnapshotIndex;
		});
		const GpuEnvironment* environment = hasCamera && frame.IsLit && !frame.Wireframe ? Cache->GetEnvironment(snapshot.Environment.Environment) : nullptr;
		frame.Environment = Utils::MakeEnvironmentConstants(snapshot.Environment, environment);
		frame.EnvironmentSpecular = environment != nullptr ? environment->Specular : Pipelines->BlackCube;
		frame.EnvironmentSkybox = environment != nullptr ? environment->Skybox : nvrhi::TextureHandle();
		// M26 (C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 16; stream Q-B): the view's probes, in the views
		// that light with the environment, never in a bake's captures (RenderQualitySettings::Probes false: one bounce).
		frame.Probes = Utils::PrepareProbeShading(*Cache, snapshot, camera, hasCamera && frame.IsLit && !frame.Wireframe && snapshot.Quality.Probes);
		frame.KeepFirstError(Utils::EnsureProbeViewResources(*Device, ProbeResources, frame.Probes));
		if ((frame.Probes.DroppedVolumes > 0 || frame.Probes.DroppedReflectionProbes > 0) && !ProbeLimitLogged)
		{
			ProbeLimitLogged = true;
			ENGINE_CORE_WARN("{} irradiance volume(s) beyond the view's {} and {} reflection probe(s) beyond its {} are not shaded (logged once per view)",
				frame.Probes.DroppedVolumes, MaxViewIrradianceVolumes, frame.Probes.DroppedReflectionProbes, MaxViewReflectionProbes);
		}
		if (frame.Probes.MismatchedReflectionProbes > 0 && !ProbeFormatLogged)
		{
			ProbeFormatLogged = true;
			ENGINE_CORE_WARN("{} reflection probe(s) whose Environment's specular cube differs in format or size from the view's first probe's are not "
							 "shaded; import every probe's Environment with the same Compression (logged once per view)",
				frame.Probes.MismatchedReflectionProbes);
		}
		if (tonemapped && Assets->GetAssetType(BuiltinAssetHandles::BlueNoiseTexture) == AssetType::Texture)
		{
			const GpuTexture& noise = Cache->GetTexture(BuiltinAssetHandles::BlueNoiseTexture);
			if (!noise.IsPlaceholder)
				frame.BlueNoise = noise.Texture;
		}
		recording.EndPass({});
		ENGINE_TRY(EnsureInstanceBuffer());
		return EnsureFrameTargets(frame.Picking, frame.Ao, frame.HalfAo, frame.Cascades.Count != 0 ? snapshot.Quality.ShadowMapSize : 0, !frame.Spots.Tiles.empty(),
			!frame.Points.Lights.empty() ? GetPointShadowFaceSize(snapshot.Quality.ShadowMapSize) : 0, frame.Selection, frame.IsOverdraw);
	}

	Status SceneRenderer::State::ResolveItemDeform(const RenderSnapshot& snapshot, size_t index)
	{
		Detail::SceneRendererItemDeform& deform = ItemDeforms[index];
		if (deform.Resolved)
			return deform.Skipped ? MakeError(ErrorCode::InvalidArgument, "the deformation of entity {} cannot be drawn", snapshot.Meshes[index].Entity) : Status{};
		deform.Resolved = true;
		const MeshDrawItem& item = snapshot.Meshes[index];
		if (!item.Mesh.IsValid())
			return {};
		const GpuMesh& mesh = Cache->GetMesh(item.Mesh);
		if (mesh.SkinJointCount == 0 && mesh.MorphTargetCount == 0)
			return {};
		for (const glm::mat4& joint : item.JointPalette)
		{
			if (!Utils::IsFinite(joint))
			{
				deform.Skipped = true;
				return MakeError(ErrorCode::InvalidArgument, "the joint palette of entity {} in the render snapshot is not finite; its draws are skipped", item.Entity);
			}
		}
		for (const float weight : item.MorphWeights)
		{
			if (!std::isfinite(weight))
			{
				deform.Skipped = true;
				return MakeError(ErrorCode::InvalidArgument, "a morph weight of entity {} in the render snapshot is not finite; its draws are skipped", item.Entity);
			}
		}
		const bool skinned = mesh.SkinJointCount > 0 && item.JointPalette.size() >= mesh.SkinJointCount;
		if (mesh.SkinJointCount > 0 && !item.JointPalette.empty() && !skinned && !SkinMismatchLogged)
		{
			SkinMismatchLogged = true;
			ENGINE_CORE_WARN("{}: entity {} has a joint palette of {} matrices for a skin of {} joints and draws its bind pose (logged once per view)",
				RenderSkinMismatchCode, item.Entity, item.JointPalette.size(), mesh.SkinJointCount);
		}
		deform.Weights = ResolveMorphWeights(mesh.DeformBounds.DefaultWeights, item.MorphWeights);
		const bool morphed = std::ranges::any_of(deform.Weights, [](float weight)
		{
			return weight != 0.0f;
		});
		if (!skinned && !morphed)
			return {};
		const std::span<const glm::mat4> palette = skinned ? std::span<const glm::mat4>(item.JointPalette) : std::span<const glm::mat4>();
		deform.Deformed = true;
		deform.Bounds = ComputeDeformedBounds(mesh.DeformBounds, palette, deform.Weights);
		deform.SourceVertices = mesh.VertexBuffer;
		deform.Skin = mesh.SkinBuffer;
		deform.Morphs = mesh.MorphBuffer;
		deform.MeshVersion = mesh.Version;
		deform.VertexCount = mesh.VertexCount;
		deform.SkinJointCount = skinned ? mesh.SkinJointCount : 0;
		deform.MorphTargetCount = mesh.MorphTargetCount;
		return {};
	}

	Result<size_t> SceneRenderer::State::AcquireDeformedMesh(Detail::SceneRendererFrame& frame, size_t index)
	{
		Detail::SceneRendererItemDeform& deform = ItemDeforms[index];
		ENGINE_CORE_ASSERT(deform.Resolved && deform.Deformed, "AcquireDeformedMesh needs a resolved, deformed item");
		const MeshDrawItem& item = frame.Snapshot->Meshes[index];
		if (deform.Entry.has_value())
			return *deform.Entry;
		if (deform.Skipped)
			return MakeError(ErrorCode::InvalidArgument, "the deformation of entity {} cannot be drawn", item.Entity);
		// The first entry of the item's key this render has not acquired, so two items of one key take two entries.
		auto found = std::ranges::find_if(DeformedMeshes, [this, &item](const Detail::SceneRendererDeformedMesh& entry)
		{
			return entry.Entity == item.Entity && entry.Mesh == item.Mesh && entry.LastRender != DeformRender;
		});
		if (found == DeformedMeshes.end())
		{
			Detail::SceneRendererDeformedMesh created;
			created.Entity = item.Entity;
			created.Mesh = item.Mesh;
			created.MeshVersion = deform.MeshVersion;
			created.VertexStream = NextVertexStream;
			// 0 is the mesh's own vertices: a renderer that made 2^32 entries starts again at 1.
			NextVertexStream = NextVertexStream == std::numeric_limits<uint32_t>::max() ? 1 : NextVertexStream + 1;
			DeformedMeshes.push_back(std::move(created));
			found = std::prev(DeformedMeshes.end());
		}
		Detail::SceneRendererDeformedMesh& entry = *found;
		if (entry.MeshVersion != deform.MeshVersion)
		{
			entry.MeshVersion = deform.MeshVersion;
			entry.Deformed = false; // another version of the mesh deforms afresh
		}
		// The buffers, created or grown for the mesh; a grown vertex buffer holds no earlier deformation.
		const auto ensure = [this, &item](nvrhi::BufferHandle& buffer, const nvrhi::BufferDesc& desc) -> Status
		{
			if (buffer != nullptr && buffer->getDesc().byteSize >= desc.byteSize)
				return {};
			Result<nvrhi::BufferHandle> created = Device->CreateBuffer(desc);
			if (!created.has_value())
				return std::unexpected(std::move(created).error().WithContext(std::format("while creating the deformation buffers of entity {}", item.Entity)));
			buffer = std::move(*created);
			return {};
		};
		const uint64_t verticesBefore = entry.Vertices != nullptr ? entry.Vertices->getDesc().byteSize : 0;
		Status created = ensure(entry.Vertices, SkinningPass::GetVerticesDesc(deform.VertexCount));
		if (created.has_value())
			created = ensure(entry.PreviousPositions, SkinningPass::GetPreviousPositionsDesc(deform.VertexCount));
		if (created.has_value())
			created = ensure(entry.Palette, SkinningPass::GetPaletteDesc(deform.SkinJointCount));
		if (created.has_value())
			created = ensure(entry.MorphWeights, SkinningPass::GetMorphWeightsDesc());
		if (entry.Vertices == nullptr || entry.Vertices->getDesc().byteSize != verticesBefore)
			entry.Deformed = false;
		if (!created.has_value())
		{
			deform.Skipped = true;
			return std::unexpected(std::move(created).error());
		}
		const auto entryIndex = static_cast<size_t>(found - DeformedMeshes.begin());
		entry.LastRender = DeformRender;
		entry.SnapshotIndex = index;
		deform.Entry = entryIndex;
		frame.Deforms.push_back(entryIndex);
		return entryIndex;
	}

	std::vector<DeformedVertexSource> SceneRenderer::State::PrepareVertexSources(Detail::SceneRendererFrame& frame, std::span<const size_t> indices)
	{
		const RenderSnapshot& snapshot = *frame.Snapshot;
		std::vector<DeformedVertexSource> sources(indices.size());
		for (size_t position = 0; position < indices.size(); ++position)
		{
			const size_t index = indices[position];
			if (!snapshot.Meshes[index].Mesh.IsValid())
				continue;
			DeformedVertexSource& source = sources[position];
			const Status resolved = ResolveItemDeform(snapshot, index);
			if (!resolved.has_value())
			{
				frame.KeepFirstError(resolved);
				source.Skipped = true;
				continue;
			}
			const Detail::SceneRendererItemDeform& deform = ItemDeforms[index];
			if (!deform.Deformed)
				continue;
			const Result<size_t> entry = AcquireDeformedMesh(frame, index);
			if (!entry.has_value())
			{
				frame.KeepFirstError(std::unexpected(entry.error()));
				source.Skipped = true;
				continue;
			}
			source.Vertices = DeformedMeshes[*entry].Vertices.Get();
			source.VertexStream = DeformedMeshes[*entry].VertexStream;
			source.Bounds = deform.Bounds;
		}
		return sources;
	}

	void SceneRenderer::State::SkipDeformedDraws(Detail::SceneRendererFrame& frame, const Detail::SceneRendererDeformedMesh& entry)
	{
		// A deformed draw's batch is itself alone (its VertexStream is its item's): the camera's lists skip it as an instance
		// of another draw, and the casters' and the selection's lose its batch.
		for (std::vector<Detail::SceneRendererDraw>* draws : { &OpaqueDraws, &TransparentDraws })
		{
			for (Detail::SceneRendererDraw& draw : *draws)
			{
				if (draw.VertexStream == entry.VertexStream)
					draw.Batch = 0;
			}
		}
		const nvrhi::IBuffer* vertices = entry.Vertices.Get();
		std::erase_if(frame.ShadowCasters.Batches, [&frame, vertices](const MeshInstanceBatch& batch)
		{
			return frame.ShadowCasters.Draws[batch.First].Vertices.Get() == vertices;
		});
		std::erase_if(frame.SelectionDraws.Batches, [&frame, vertices](const MeshInstanceBatch& batch)
		{
			return frame.SelectionDraws.Draws[batch.First].Vertices.Get() == vertices;
		});
	}

	void SceneRenderer::State::ReleaseUnusedDeforms()
	{
		std::erase_if(DeformedMeshes, [this](const Detail::SceneRendererDeformedMesh& entry)
		{
			return entry.LastRender != DeformRender;
		});
		ItemDeforms.clear();
	}

	// The Skinning pass (RenderStage::Deform), the first GPU work of a frame: every deformed item one of whose draws survived
	// culling, in snapshot order, deformed into its entry's vertices before the shadow casters, the prepass and the forward
	// passes draw them.
	void SceneRenderer::State::RecordDeformStage(Detail::SceneRendererFrame& frame)
	{
		if (frame.Deforms.empty())
			return;
		const RenderSnapshot& snapshot = *frame.Snapshot;
		std::vector<SkinningPassInputs> items;
		items.reserve(frame.Deforms.size());
		for (const size_t entryIndex : frame.Deforms)
		{
			const Detail::SceneRendererDeformedMesh& entry = DeformedMeshes[entryIndex];
			const Detail::SceneRendererItemDeform& deform = ItemDeforms[entry.SnapshotIndex];
			const MeshDrawItem& item = snapshot.Meshes[entry.SnapshotIndex];
			items.push_back({ .SourceVertices = deform.SourceVertices,
				.Skin = deform.Skin,
				.Morphs = deform.Morphs,
				.Palette = deform.SkinJointCount > 0 ? std::span<const glm::mat4>(item.JointPalette) : std::span<const glm::mat4>(),
				.MorphWeights = deform.Weights,
				.PaletteBuffer = entry.Palette,
				.MorphWeightBuffer = entry.MorphWeights,
				.Vertices = entry.Vertices,
				.PreviousPositions = entry.PreviousPositions,
				.VertexCount = deform.VertexCount,
				.SkinJointCount = deform.SkinJointCount,
				.MorphTargetCount = deform.MorphTargetCount,
				.FirstDeform = !entry.Deformed });
		}
		// One Record for the whole stage: the Skinning pass is one profiler scope however many items it deforms.
		const std::vector<Status> recorded = Pipelines->Skinning->Record(*frame.CommandList, *frame.Recording, SkinningBindings, items);
		for (size_t index = 0; index < frame.Deforms.size(); ++index)
		{
			Detail::SceneRendererDeformedMesh& entry = DeformedMeshes[frame.Deforms[index]];
			if (recorded[index].has_value())
			{
				entry.Deformed = true;
				continue;
			}
			const MeshDrawItem& item = snapshot.Meshes[entry.SnapshotIndex];
			frame.KeepFirstError(std::unexpected(Error(recorded[index].error()).WithContext(std::format("while deforming entity {}", item.Entity))));
			SkipDeformedDraws(frame, entry);
		}
	}

	void SceneRenderer::State::RecordShadowsStage(Detail::SceneRendererFrame& frame)
	{
		const ShadowCascadeSet& cascades = frame.Cascades;
		const SpotShadowAtlas& spots = frame.Spots;
		const PointShadowSet& points = frame.Points;
		if (cascades.Count == 0 && spots.Tiles.empty() && points.Lights.empty())
			return;
		const RenderSnapshot& snapshot = *frame.Snapshot;
		const ShadowRenderInputs inputs{
			.Casters = &frame.ShadowCasters,
			.Instances = InstanceBuffer,
			.Cascades = cascades.Count != 0 ? &cascades : nullptr,
			.Spots = !spots.Tiles.empty() ? &spots : nullptr,
			.CascadeTarget = cascades.Count != 0 ? Targets.Cascades.Get() : nullptr,
			.AtlasTarget = !spots.Tiles.empty() ? Targets.SpotAtlas.Get() : nullptr,
			.CascadeFramebuffers = Targets.CascadeFramebuffers,
			.AtlasFramebuffer = !spots.Tiles.empty() ? Targets.SpotAtlasFramebuffer.Get() : nullptr,
			.CasterConstants = ShadowCasterConstantsBuffer,
			.DepthBias = cascades.Count != 0 ? snapshot.Lights[cascades.LightIndex].DepthBias : 1.0f,
			.NormalBias = cascades.Count != 0 ? snapshot.Lights[cascades.LightIndex].NormalBias : 1.0f,
			.Points = !points.Lights.empty() ? &points : nullptr,
			.PointTarget = !points.Lights.empty() ? Targets.PointShadowMaps.Get() : nullptr,
			.PointFramebuffers = Targets.PointShadowFramebuffers,
		};
		auto result = Pipelines->Shadows->Record(*frame.CommandList, *frame.Recording, ShadowBindings, inputs);
		if (!result)
			frame.KeepFirstError(std::unexpected(std::move(result).error()));
	}

	void SceneRenderer::State::RecordDepthNormalStage(Detail::SceneRendererFrame& frame)
	{
		nvrhi::ICommandList& commandList = *frame.CommandList;
		frame.PrepassSet = nullptr;
		if (frame.HasCamera)
		{
			// M26 (stream X-C): the DFG LUT and its sampler for SceneRoughness's specular inputs.
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(Utils::ViewConstantsRegister, ViewConstantsBuffer),
				nvrhi::BindingSetItem::Texture_SRV(Utils::BrdfLutRegister, Pipelines->DfgLut->GetTexture()),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(Utils::InstancesRegister, InstanceBuffer),
				nvrhi::BindingSetItem::Sampler(Utils::LinearClampRegister, Pipelines->LinearClamp),
				nvrhi::BindingSetItem::Sampler(Utils::AnisoWrapRegister, Pipelines->AnisoWrap),
				nvrhi::BindingSetItem::PushConstants(Utils::DrawConstantsSlot, sizeof(DrawConstants)),
			};
			auto created = ViewBindings.GetOrCreate(*Device, desc, *Pipelines->Layouts.PrepassView);
			if (created)
				frame.PrepassSet = *created;
			else
				frame.KeepFirstError(std::unexpected(std::move(created).error()));
			// The Overdraw data view's pipelines bind a set 0 of their own (Private/ForwardPipelines.h).
			if (frame.IsOverdraw)
			{
				nvrhi::BindingSetDesc overdraw;
				overdraw.bindings = {
					nvrhi::BindingSetItem::ConstantBuffer(Utils::ViewConstantsRegister, ViewConstantsBuffer),
					nvrhi::BindingSetItem::StructuredBuffer_SRV(Utils::InstancesRegister, InstanceBuffer),
					nvrhi::BindingSetItem::Sampler(Utils::AnisoWrapRegister, Pipelines->AnisoWrap),
					nvrhi::BindingSetItem::PushConstants(Utils::DrawConstantsSlot, sizeof(DrawConstants)),
				};
				auto overdrawSet = ViewBindings.GetOrCreate(*Device, overdraw, *Pipelines->Layouts.OverdrawView);
				if (overdrawSet)
					frame.OverdrawSet = *overdrawSet;
				else
					frame.KeepFirstError(std::unexpected(std::move(overdrawSet).error()));
			}
		}
		// The Overdraw data view draws its own pass in the Forward stage; with picking it still needs EntityId.
		if (frame.PrepassSet == nullptr || (frame.IsOverdraw && !frame.Picking))
			return;
		frame.Recording->BeginPass("DepthNormal", &commandList);
		RenderPassCounters counters;
		RecordMeshPass(commandList, frame.Picking ? Detail::SceneRendererMeshPass::Picking : Detail::SceneRendererMeshPass::Prepass, OpaqueDraws,
			*frame.PrepassSet, frame.DebugView, counters);
		frame.Recording->EndPass(counters);
		RecordTerrainDepthNormal(frame);
	}

	void SceneRenderer::State::RecordTerrainDepthNormal(Detail::SceneRendererFrame& frame)
	{
		const RenderSnapshot& snapshot = *frame.Snapshot;
		if (snapshot.Terrains.empty())
			return;
		ENGINE_CORE_ASSERT(Pipelines->Terrain != nullptr, "the pipeline set has no terrain pass");
		if (Pipelines->Terrain == nullptr)
		{
			frame.KeepFirstError(MakeError(ErrorCode::InvalidState, "the scene renderer's pipelines have no terrain pass; the terrains are not drawn"));
			return;
		}
		nvrhi::ICommandList& commandList = *frame.CommandList;
		const TerrainPassInputs inputs{ .Snapshot = &snapshot, .Camera = &frame.Camera, .DebugView = frame.DebugView };
		frame.KeepFirstError(Pipelines->Terrain->Prepare(commandList, *Cache, TerrainResources, inputs));
		if (TerrainResources.Draws.empty())
			return;
		// The terrain prepass's set 0 (TerrainPass.h): the view's constants, the DFG LUT its SceneRoughness reads and the samplers.
		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(Utils::ViewConstantsRegister, ViewConstantsBuffer),
			nvrhi::BindingSetItem::Texture_SRV(Utils::BrdfLutRegister, Pipelines->DfgLut->GetTexture()),
			nvrhi::BindingSetItem::Sampler(Utils::LinearClampRegister, Pipelines->LinearClamp),
			nvrhi::BindingSetItem::Sampler(Utils::AnisoWrapRegister, Pipelines->AnisoWrap),
			nvrhi::BindingSetItem::PushConstants(Utils::DrawConstantsSlot, sizeof(TerrainDrawConstants)),
		};
		auto set = ViewBindings.GetOrCreate(*Device, desc, Pipelines->Terrain->GetPrepassViewLayout());
		if (!set)
		{
			frame.KeepFirstError(std::unexpected(std::move(set).error()));
			return;
		}
		nvrhi::IFramebuffer* framebuffer = frame.Picking ? Targets.PickingFramebuffer.Get() : Targets.PrepassFramebuffer.Get();
		frame.Recording->BeginPass("TerrainDepthNormal", &commandList);
		RenderPassCounters counters;
		Pipelines->Terrain->RecordDepthNormal(commandList, TerrainResources, *framebuffer, frame.Picking, **set, counters);
		frame.Recording->EndPass(counters);
	}

	void SceneRenderer::State::RecordAmbientStage(Detail::SceneRendererFrame& frame)
	{
		frame.Occlusion = Pipelines->WhiteAo;
		frame.LinearDepth = Pipelines->FarDepth;
		// M26 (stream X-D, decision 18): SSR traces over the depth pyramid, which records for it without GTAO too; the
		// forward passes keep the neutral occlusion and depth then.
		if (frame.Ssr)
		{
			const Status ensured = EnsureSsrTargets();
			frame.KeepFirstError(ensured);
			frame.Ssr = ensured.has_value();
		}
		if (frame.Ao || frame.Ssr)
		{
			nvrhi::ICommandList& commandList = *frame.CommandList;
			RenderRecordingContext& recording = *frame.Recording;
			const Status depth = Pipelines->DepthPyramid->Record(commandList, recording, DepthBindings, { .SceneDepth = Targets.SceneDepth, .ViewDepth = Targets.ViewDepth, .Camera = frame.Camera });
			frame.KeepFirstError(depth);
			frame.Ssr = frame.Ssr && depth.has_value();
			if (depth && frame.Ao)
			{
				auto result = Pipelines->Gtao->Record(commandList, recording, GtaoBindings, { .Camera = frame.Camera, .Post = frame.Snapshot->Post, .HalfResolution = frame.HalfAo, .ViewDepth = Targets.ViewDepth, .SceneNormals = Targets.SceneNormals, .Occlusion = Targets.Occlusion, .Scratch = Targets.AoScratch });
				if (result)
				{
					frame.Occlusion = *result;
					frame.LinearDepth = Targets.ViewDepth;
				}
				else
					frame.KeepFirstError(std::unexpected(std::move(result).error()));
			}
		}
		RecordLightClusters(frame);
	}

	Status SceneRenderer::State::EnsureLightClusterBuffers(uint32_t clusterCount)
	{
		if (LightClusterConstantsBuffer == nullptr)
		{
			ENGINE_TRY_ASSIGN(LightClusterConstantsBuffer, Device->CreateBuffer(LightClusterPass::GetConstantsDesc()));
		}
		const nvrhi::BufferDesc masks = LightClusterPass::GetMaskBufferDesc(clusterCount);
		if (LightClusterBuffer == nullptr || LightClusterBuffer->getDesc().byteSize < masks.byteSize)
		{
			ENGINE_TRY_ASSIGN(LightClusterBuffer, Device->CreateBuffer(masks));
		}
		return {};
	}

	void SceneRenderer::State::RecordLightClusters(Detail::SceneRendererFrame& frame)
	{
		frame.Clusters = ComputeLightClusterGrid(frame.Camera, Targets.Width, Targets.Height);
		const Status ensured = EnsureLightClusterBuffers(GetLightClusterCount(frame.Clusters));
		frame.KeepFirstError(ensured);
		if (LightClusterConstantsBuffer == nullptr || LightClusterBuffer == nullptr)
			return;
		nvrhi::ICommandList& commandList = *frame.CommandList;
		const bool hasLocalLight = std::ranges::any_of(Lights, [](const ShaderLight& light)
		{
			return light.Type != ShaderLightTypeDirectional;
		});
		const bool readsMasks = frame.HasCamera && ((frame.IsLit && !frame.Wireframe) || frame.DebugView == RenderDebugView::LightCount);
		if (ensured && frame.Snapshot->Quality.ClusteredLights && hasLocalLight && readsMasks)
		{
			const Status recorded = Pipelines->LightClusters->Record(commandList, *frame.Recording, LightClusterBindings,
				{ .Grid = frame.Clusters, .ViewConstants = ViewConstantsBuffer, .ClusterConstants = LightClusterConstantsBuffer, .Lights = LightBuffer, .Masks = LightClusterBuffer });
			if (recorded)
				return;
			frame.KeepFirstError(recorded);
		}
		const LightClusterConstants constants = MakeLightClusterConstants(frame.Clusters, LightClusterModeBruteForce);
		commandList.writeBuffer(LightClusterConstantsBuffer, &constants, sizeof(constants));
	}

	void SceneRenderer::State::RecordForwardStage(Detail::SceneRendererFrame& frame)
	{
		const RenderSnapshot& snapshot = *frame.Snapshot;
		nvrhi::ICommandList& commandList = *frame.CommandList;
		RenderRecordingContext& recording = *frame.Recording;
		// M23 (stream C): the forward set binds the cluster grid and masks (RecordLightClusters); without them (their creation
		// error is the render's) the forward passes are skipped.
		if (frame.HasCamera && !frame.IsDataView && !frame.Wireframe && LightClusterConstantsBuffer != nullptr && LightClusterBuffer != nullptr
			&& ProbeResources.Constants != nullptr)
		{
			// M26 (stream Q-B): the probes' resources (Private/ProbeShading.h), or the neutral ones without probes of a kind.
			const bool hasVolumes = !frame.Probes.Volumes.empty();
			const bool hasReflections = !frame.Probes.Cubes.empty();
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(Utils::ViewConstantsRegister, ViewConstantsBuffer),
				nvrhi::BindingSetItem::ConstantBuffer(Utils::ShadowConstantsRegister, ShadowConstantsBuffer),
				nvrhi::BindingSetItem::ConstantBuffer(Utils::EnvironmentConstantsRegister, EnvironmentConstantsBuffer),
				nvrhi::BindingSetItem::ConstantBuffer(Utils::LightClusterConstantsRegister, LightClusterConstantsBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(Utils::LightsRegister, LightBuffer),
				nvrhi::BindingSetItem::Texture_SRV(Utils::ShadowCascadesRegister, frame.Cascades.Count != 0 ? Targets.Cascades : Pipelines->EmptyCascades),
				nvrhi::BindingSetItem::Texture_SRV(Utils::ShadowAtlasRegister, !frame.Spots.Tiles.empty() ? Targets.SpotAtlas : Pipelines->EmptyAtlas),
				nvrhi::BindingSetItem::Texture_SRV(Utils::EnvSpecularRegister, frame.EnvironmentSpecular),
				nvrhi::BindingSetItem::Texture_SRV(Utils::BrdfLutRegister, Pipelines->DfgLut->GetTexture()),
				nvrhi::BindingSetItem::Texture_SRV(Utils::AmbientOcclusionRegister, frame.Occlusion),
				nvrhi::BindingSetItem::Texture_SRV(Utils::ViewDepthRegister, frame.LinearDepth),
				nvrhi::BindingSetItem::Texture_SRV(Utils::SceneNormalsRegister, Targets.SceneNormals),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(Utils::InstancesRegister, InstanceBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(Utils::LightClusterMasksRegister, LightClusterBuffer),
				nvrhi::BindingSetItem::Texture_SRV(Utils::PointShadowMapsRegister, !frame.Points.Lights.empty() ? Targets.PointShadowMaps : Pipelines->EmptyPointShadows),
				nvrhi::BindingSetItem::ConstantBuffer(Utils::ProbeConstantsRegister, ProbeResources.Constants),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(Utils::IrradianceProbesRegister, hasVolumes ? ProbeResources.Coefficients : Pipelines->EmptyIrradianceProbes),
				nvrhi::BindingSetItem::Texture_SRV(Utils::ReflectionProbeCubesRegister, hasReflections ? ProbeResources.Cubes : Pipelines->EmptyReflectionProbeCubes),
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
				// M26 (C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 22, stream T-C): the terrains the DepthNormal
				// stage prepared, with the same forward set, before the skybox fills the pixels left at the cleared depth.
				if (!TerrainResources.Draws.empty())
				{
					recording.BeginPass("TerrainForward", &commandList);
					RenderPassCounters counters;
					Pipelines->Terrain->RecordForward(commandList, TerrainResources, *Targets.ForwardFramebuffer, **forwardSet, counters);
					recording.EndPass(counters);
				}
				if (frame.IsLit && frame.Camera.ClearToSkybox && frame.EnvironmentSkybox != nullptr && snapshot.Environment.ShowSkybox)
				{
					recording.BeginPass("Skybox", &commandList);
					const Status drawn = Pipelines->Skybox->Record(commandList, SkyboxBindings, { .Framebuffer = Targets.ForwardFramebuffer, .ViewConstants = ViewConstantsBuffer, .EnvironmentConstants = EnvironmentConstantsBuffer, .Skybox = frame.EnvironmentSkybox });
					recording.EndPass({ .DrawCalls = drawn ? 1U : 0U, .Triangles = drawn ? 1U : 0U });
					frame.KeepFirstError(drawn);
				}
				// M26 (stream X-D, decision 18): SSR over the opaque image and the skybox, before the transparent pass.
				if (frame.Ssr)
					RecordSsr(frame);
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
			if (frame.OverdrawSet != nullptr)
			{
				RecordMeshPass(commandList, Detail::SceneRendererMeshPass::Overdraw, OpaqueDraws, *frame.OverdrawSet, frame.DebugView, counters);
				RecordMeshPass(commandList, Detail::SceneRendererMeshPass::Overdraw, TransparentDraws, *frame.OverdrawSet, frame.DebugView, counters);
			}
			recording.EndPass(counters);
		}
	}

	Status SceneRenderer::State::EnsureSsrTargets()
	{
		const uint32_t width = Targets.Width;
		const uint32_t height = Targets.Height;
		if (Targets.ViewDepth == nullptr)
		{
			ENGINE_TRY_ASSIGN(Targets.ViewDepth, TargetPool->Acquire(DepthPyramidPass::GetTargetDesc(width, height)));
		}
		if (Targets.SsrColor == nullptr)
		{
			ENGINE_TRY_ASSIGN(Targets.SsrColor, TargetPool->Acquire(SsrPass::GetTargetDesc(width, height)));
		}
		if (Targets.SsrFramebuffer == nullptr)
		{
			ENGINE_TRY_ASSIGN(Targets.SsrFramebuffer, Device->CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(Targets.SceneColor)));
		}
		return {};
	}

	void SceneRenderer::State::RecordSsr(Detail::SceneRendererFrame& frame)
	{
		nvrhi::ICommandList& commandList = *frame.CommandList;
		RenderRecordingContext& recording = *frame.Recording;
		const SsrPassInputs inputs{
			.ViewConstants = ViewConstantsBuffer,
			.EnvironmentConstants = EnvironmentConstantsBuffer,
			.ProbeConstants = ProbeResources.Constants,
			.SceneDepth = Targets.SceneDepth,
			.SceneNormals = Targets.SceneNormals,
			.SceneRoughness = Targets.SceneRoughness,
			.ViewDepth = Targets.ViewDepth,
			.SceneColor = Targets.SceneColor,
			.Framebuffer = Targets.SsrFramebuffer,
			.SsrColor = Targets.SsrColor,
			.AmbientOcclusion = frame.Occlusion,
			.LinearViewDepth = frame.LinearDepth,
			.EnvironmentSpecular = frame.EnvironmentSpecular,
			.ReflectionProbeCubes = !frame.Probes.Cubes.empty() ? ProbeResources.Cubes.Get() : Pipelines->EmptyReflectionProbeCubes.Get(),
			.Post = frame.Snapshot->Post,
		};
		recording.BeginPass("SSR", &commandList);
		RenderPassCounters counters;
		const Status recorded = Pipelines->Ssr->Record(commandList, SsrBindings, inputs, counters);
		recording.EndPass(counters);
		frame.KeepFirstError(recorded);
	}

	void SceneRenderer::State::RecordSpritesStage(Detail::SceneRendererFrame& frame)
	{
		RecordSpritesAndTilemaps(frame);
		// M26 (C26.1, decision 7): the particles after the sprites, the tilemaps and the 2D lights.
		RecordParticles(frame);
	}

	void SceneRenderer::State::RecordSpritesAndTilemaps(Detail::SceneRendererFrame& frame)
	{
		const RenderSnapshot& snapshot = *frame.Snapshot;
		// Sprites and tilemaps draw in the Lit view only (decision 4): the data views, the material views and the wireframe
		// show none. A render that draws no 2D item keeps no tilemap of an earlier render (M21's milestone review).
		if ((snapshot.Sprites.empty() && snapshot.Tilemaps.empty()) || !frame.HasCamera || !frame.IsLit || frame.Wireframe)
		{
			ReleaseUnusedTilemaps();
			return;
		}
		ENGINE_CORE_ASSERT(Pipelines->Quads != nullptr, "the pipeline set has no QuadBatcher");
		if (Pipelines->Quads == nullptr)
		{
			frame.KeepFirstError(MakeError(ErrorCode::InvalidState, "the scene renderer's pipelines have no QuadBatcher; the sprites are not drawn"));
			ReleaseUnusedTilemaps();
			return;
		}
		const glm::mat4 viewProjection = frame.Camera.Projection * frame.Camera.View;

		// The sheets of this render, each loaded once: null for one that is missing or fails to load, which draws nothing and is
		// reported with GetOrPlaceholder's codes (AssetManager.h), as sheets have no placeholder.
		std::vector<std::pair<AssetHandle, AssetRef<SpriteSheetData>>> sheets;
		const auto resolveSheet = [this, &sheets](AssetHandle handle) -> const SpriteSheetData*
		{
			if (const auto found = std::ranges::find(sheets, handle, &std::pair<AssetHandle, AssetRef<SpriteSheetData>>::first); found != sheets.end())
				return found->second.get();
			const Result<AssetRef<Asset>> loaded = Assets->Load(handle);
			AssetRef<SpriteSheetData> sheet = loaded.has_value() ? AssetCast<SpriteSheetData>(*loaded) : nullptr;
			if (sheet == nullptr)
				Utils::ReportSpriteSheetFailure(*Assets, handle, loaded.has_value() ? nullptr : &loaded.error());
			return sheets.emplace_back(handle, std::move(sheet)).second.get();
		};

		SpriteQuads.clear();
		SpriteQuadLit.clear();
		TileQuadsDrawn = 0;
		TileQuadsDropped = 0;
		// C21.2 (decision 24): the tilemaps draw in the one 2D draw order, each before Sprites[DrawPosition] (after every sprite
		// at Sprites.size()).
		size_t nextTilemap = 0;
		const auto appendTilemapsBefore = [this, &frame, &snapshot, &viewProjection, &resolveSheet, &nextTilemap](size_t position)
		{
			for (; nextTilemap < snapshot.Tilemaps.size() && snapshot.Tilemaps[nextTilemap].DrawPosition <= position; ++nextTilemap)
				AppendTilemapQuads(frame, snapshot.Tilemaps[nextTilemap], viewProjection, resolveSheet);
		};
		for (size_t spriteIndex = 0; spriteIndex < snapshot.Sprites.size(); ++spriteIndex)
		{
			appendTilemapsBefore(spriteIndex);
			const SpriteItem& sprite = snapshot.Sprites[spriteIndex];
			if (!Utils::IsFinite(sprite.World))
			{
				frame.KeepFirstError(MakeError(ErrorCode::InvalidArgument,
					"the world matrix of sprite entity {} in the render snapshot is not finite; its draw is skipped", sprite.Entity));
				continue;
			}
			if (!std::isfinite(sprite.PixelsPerUnit) || !(sprite.PixelsPerUnit > 0.0f))
			{
				frame.KeepFirstError(MakeError(ErrorCode::InvalidArgument,
					"sprite entity {} in the render snapshot has {} pixels per unit, not a finite positive value; its draw is skipped", sprite.Entity,
					sprite.PixelsPerUnit));
				continue;
			}
			// The image (SpriteRendererComponent.h's one rule): the sheet's frame over the sheet's texture (white without one),
			// or the whole texture centred, the built-in white square without either. A missing or failed texture is its
			// placeholder's mirror, shown whole over the quad.
			std::optional<SpriteFrameGeometry> geometry;
			nvrhi::ITexture* texture = nullptr;
			bool wholeTexture = false;
			if (sprite.Sheet.IsValid())
			{
				const SpriteSheetData* sheet = resolveSheet(sprite.Sheet);
				if (sheet == nullptr || sprite.Frame >= sheet->Frames.size())
					continue; // no quad: SPRITE_FRAME_OUT_OF_RANGE, or the sheet's failure reported above
				glm::uvec2 textureSize = glm::uvec2(sheet->Frames[sprite.Frame].Rect.X + sheet->Frames[sprite.Frame].Rect.Width,
					sheet->Frames[sprite.Frame].Rect.Y + sheet->Frames[sprite.Frame].Rect.Height);
				if (sheet->Texture.IsValid())
				{
					const GpuTexture& mirror = Cache->GetTexture(sheet->Texture.GetHandle());
					if (mirror.Texture == nullptr)
						continue; // not even the placeholder could be created (GpuResourceCache.h)
					texture = mirror.Texture.Get();
					wholeTexture = mirror.IsPlaceholder;
					textureSize = glm::uvec2(std::max(texture->getDesc().width, 1u), std::max(texture->getDesc().height, 1u));
				}
				geometry = GetSpriteFrameGeometry(sheet, sprite.Frame, textureSize);
			}
			else
			{
				const GpuTexture& mirror = Cache->GetTexture(sprite.Texture.IsValid() ? sprite.Texture : BuiltinAssetHandles::SpriteSquareTexture);
				if (mirror.Texture == nullptr)
					continue;
				texture = mirror.Texture.Get();
				geometry = GetSpriteFrameGeometry(nullptr, 0, glm::uvec2(std::max(texture->getDesc().width, 1u), std::max(texture->getDesc().height, 1u)));
			}
			if (!geometry.has_value())
				continue;
			const SpriteLocalQuad local = ComputeSpriteLocalQuad(*geometry, sprite.PixelsPerUnit, sprite.FlipX, sprite.FlipY);

			QuadDraw quad{ .TexCoordMin = wholeTexture ? glm::vec2(0.0f) : local.TexCoordMin,
				.TexCoordMax = wholeTexture ? glm::vec2(1.0f) : local.TexCoordMax,
				.Color = sprite.Color,
				.Texture = texture,
				.Sampler = QuadSampler::Point };
			std::array<glm::vec4, 4> clip{};
			for (size_t index = 0; index < quad.Corners.size(); ++index)
			{
				const glm::vec4 world = sprite.World * glm::vec4(local.Corners[index], 0.0f, 1.0f);
				quad.Corners[index] = glm::vec3(world);
				clip[index] = viewProjection * world;
			}
			if (Utils::IsOutsideClipVolume(clip))
				continue;
			// C21.2 (decision 21): a lit sprite's normal map, laid like its image; a missing or failed one is its placeholder's
			// mirror, as a material's (the cache reports it), and only a mirror without a texture leaves the flat normal. A BC5
			// map (a normal map's Auto compression) stores x and y only, on a device with BC and through the decode fallback
			// alike, so the Normals target reconstructs its z, as GetMaterial's MaterialFlagNormalMapTwoChannel has the 3D path
			// do (M23's milestone review).
			if (sprite.Lit && sprite.NormalMap.IsValid())
			{
				const GpuTexture& normalMap = Cache->GetTexture(sprite.NormalMap);
				quad.NormalMap = normalMap.Texture.Get();
				quad.NormalMapTwoChannel = normalMap.Format == TextureFormat::BC5Unorm;
			}
			AddSpriteQuad(quad, sprite.Lit);
		}
		appendTilemapsBefore(snapshot.Sprites.size());

		// M21's milestone review: the tiles past the view's limit, reported once per renderer.
		Stats.DroppedTiles = TileQuadsDropped;
		if (TileQuadsDropped > 0 && !TileLimitLogged)
		{
			TileLimitLogged = true;
			ENGINE_CORE_WARN("{} visible tile(s) exceed the view's budget of {} tiles; the view draws the first {} in its 2D draw order, and every "
							 "sprite (logged once per view)",
				TileQuadsDropped, MaxTileQuadsPerView, MaxTileQuadsPerView);
		}

		// C21.2 (decision 22): the lights the lit quads sample, then the quads in the one draw order.
		nvrhi::ITexture* light = RecordLights2D(frame, viewProjection);
		RecordSpriteQuads(frame, viewProjection, light);
		// The borrowed texture pointers are valid for this render only.
		SpriteQuads.clear();
		SpriteQuadLit.clear();
		ReleaseOutsizedSpriteQuads();
		ReleaseUnusedTilemaps();
	}

	void SceneRenderer::State::RecordParticles(Detail::SceneRendererFrame& frame)
	{
		const RenderSnapshot& snapshot = *frame.Snapshot;
		// Particles draw in the Lit view only, as sprites do: the data views, the material views, the Overdraw view and the
		// wireframe show none.
		if (snapshot.Particles.empty() || !frame.HasCamera || !frame.IsLit || frame.Wireframe)
			return;
		ENGINE_CORE_ASSERT(Pipelines->Particles != nullptr, "the pipeline set has no Particles pass");
		if (Pipelines->Particles == nullptr)
		{
			frame.KeepFirstError(MakeError(ErrorCode::InvalidState, "the scene renderer's pipelines have no Particles pass; the particles are not drawn"));
			return;
		}
		ResolveParticleFlipbooks(snapshot.ParticleEmitters);
		const glm::mat4 viewProjection = frame.Camera.Projection * frame.Camera.View;
		frame.KeepFirstError(PlanParticlePass(snapshot.ParticleEmitters, snapshot.Particles, ParticleFlipbooks, frame.Camera.View, viewProjection, ParticlePlan));
		if (!ParticlePlan.Quads.empty())
		{
			nvrhi::ICommandList& commandList = *frame.CommandList;
			frame.Recording->BeginPass("Particles", &commandList);
			RenderPassCounters counters;
			const Result<uint32_t> drawn = Pipelines->Particles->Record(commandList, ParticleBindings,
				{ .Plan = &ParticlePlan,
					.Resources = Cache,
					.ColorFramebuffer = Targets.ParticleColorFramebuffer,
					.VelocityFramebuffer = frame.Taa ? Targets.ParticleVelocityFramebuffer.Get() : nullptr,
					.ViewConstants = ViewConstantsBuffer,
					.SceneDepth = Targets.SceneDepth,
					.Sampler = frame.PixelArt ? QuadSampler::Point : QuadSampler::Linear },
				counters);
			frame.Recording->EndPass(counters);
			if (!drawn)
				frame.KeepFirstError(std::unexpected(drawn.error()));
		}
		// An outsized render keeps no memory after it (as the Sprites stage's quad vectors, M21's milestone review).
		if (ParticlePlan.Quads.capacity() > ParticlePass::MaxQuadsPerBatch)
			std::vector<ParticlePassQuad>().swap(ParticlePlan.Quads);
	}

	void SceneRenderer::State::ResolveParticleFlipbooks(std::span<const ParticleEmitterItem> emitters)
	{
		ParticleFlipbooks.clear();
		if (std::ranges::none_of(emitters, [](const ParticleEmitterItem& emitter)
		{
			return emitter.Sheet.IsValid();
		}))
			return;
		// The sheets of this render, each loaded once: null for one that is missing or fails to load, which draws nothing and is
		// reported with GetOrPlaceholder's codes (AssetManager.h), as the sprites' sheets.
		std::vector<std::pair<AssetHandle, AssetRef<SpriteSheetData>>> sheets;
		ParticleFlipbooks.resize(emitters.size());
		for (size_t index = 0; index < emitters.size(); ++index)
		{
			const ParticleEmitterItem& emitter = emitters[index];
			if (!emitter.Sheet.IsValid())
				continue;
			const SpriteSheetData* sheet = nullptr;
			if (const auto found = std::ranges::find(sheets, emitter.Sheet, &std::pair<AssetHandle, AssetRef<SpriteSheetData>>::first); found != sheets.end())
				sheet = found->second.get();
			else
			{
				const Result<AssetRef<Asset>> loaded = Assets->Load(emitter.Sheet);
				AssetRef<SpriteSheetData> resolved = loaded.has_value() ? AssetCast<SpriteSheetData>(*loaded) : nullptr;
				if (resolved == nullptr)
					Utils::ReportSpriteSheetFailure(*Assets, emitter.Sheet, loaded.has_value() ? nullptr : &loaded.error());
				sheet = sheets.emplace_back(emitter.Sheet, std::move(resolved)).second.get();
			}
			if (sheet == nullptr)
				continue;
			// The range (decision 5): [FirstFrame, FirstFrame + FrameCount), FrameCount 0 to the sheet's last frame; an empty
			// range or one past the sheet's frames draws nothing (PARTICLE_FRAME_OUT_OF_RANGE).
			const size_t frameTotal = sheet->Frames.size();
			if (emitter.FirstFrame >= frameTotal)
				continue;
			const size_t count = emitter.FrameCount != 0 ? emitter.FrameCount : frameTotal - emitter.FirstFrame;
			if (count > frameTotal - emitter.FirstFrame)
				continue;
			ParticleFlipbook& flipbook = ParticleFlipbooks[index];
			flipbook.Texture = sheet->Texture.IsValid() ? sheet->Texture.GetHandle() : BuiltinAssetHandles::WhiteTexture;
			const GpuTexture& mirror = Cache->GetTexture(flipbook.Texture);
			if (mirror.Texture == nullptr)
				continue; // not even the placeholder could be created (GpuResourceCache.h)
			// A missing or failed texture's placeholder is drawn whole over each quad, as a sprite's; a sheet without a texture
			// samples its white texel whatever the rectangle.
			const bool whole = mirror.IsPlaceholder;
			const glm::vec2 size(static_cast<float>(std::max(mirror.Texture->getDesc().width, 1U)), static_cast<float>(std::max(mirror.Texture->getDesc().height, 1U)));
			flipbook.Frames.reserve(count);
			for (size_t frame = emitter.FirstFrame; frame < emitter.FirstFrame + count; ++frame)
			{
				const SpriteRect& rect = sheet->Frames[frame].Rect;
				flipbook.Frames.push_back(whole ? glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)
												: glm::vec4(static_cast<float>(rect.X) / size.x, static_cast<float>(rect.Y) / size.y,
													  static_cast<float>(rect.X + rect.Width) / size.x, static_cast<float>(rect.Y + rect.Height) / size.y));
			}
		}
	}

	void SceneRenderer::State::AddSpriteQuad(const QuadDraw& quad, bool lit)
	{
		SpriteQuads.push_back(quad);
		SpriteQuadLit.push_back(lit ? 1 : 0);
	}

	// A view's tiles fill at most two of the QuadBatcher's batches (SceneRenderer.h).
	static_assert(SceneRenderer::MaxTileQuadsPerView == 2 * QuadBatcher::MaxQuadsPerRecord);

	void SceneRenderer::State::ReleaseOutsizedSpriteQuads()
	{
		constexpr size_t Kept = QuadBatcher::MaxQuadsPerRecord;
		if (SpriteQuads.capacity() > Kept)
			std::vector<QuadDraw>().swap(SpriteQuads);
		if (SpriteQuadLit.capacity() > Kept)
			std::vector<uint8_t>().swap(SpriteQuadLit);
		if (LitSpriteQuads.capacity() > Kept)
			std::vector<QuadDraw>().swap(LitSpriteQuads);
		if (TilemapChunksDrawn.capacity() > Kept)
			std::vector<uint8_t>().swap(TilemapChunksDrawn);
	}

	size_t SceneRenderer::GetSpriteQuadCapacity() const
	{
		const State& state = *m_State;
		return std::max({ state.SpriteQuads.capacity(), state.SpriteQuadLit.capacity(), state.LitSpriteQuads.capacity() });
	}

	Status SceneRenderer::State::EnsureLights2DTargets()
	{
		const uint32_t width = Targets.Width;
		const uint32_t height = Targets.Height;
		if (Targets.Lights2DNormals == nullptr)
		{
			ENGINE_TRY_ASSIGN(Targets.Lights2DNormals,
				Utils::CreateTarget(*TargetPool, width, height, Lights2DNormalsFormat, Utils::TargetUsage::Attachment, "Lights2DNormals"));
		}
		if (Targets.Lights2DLight == nullptr)
		{
			ENGINE_TRY_ASSIGN(Targets.Lights2DLight,
				Utils::CreateTarget(*TargetPool, width, height, Lights2DLightFormat, Utils::TargetUsage::Attachment, "Lights2DLight"));
		}
		if (Targets.Lights2DNormalsFramebuffer == nullptr)
		{
			ENGINE_TRY_ASSIGN(Targets.Lights2DNormalsFramebuffer,
				Device->CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(Targets.Lights2DNormals).setDepthAttachment(Targets.SceneDepth)));
		}
		if (Targets.Lights2DLightFramebuffer == nullptr)
		{
			ENGINE_TRY_ASSIGN(Targets.Lights2DLightFramebuffer, Device->CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(Targets.Lights2DLight)));
		}
		return {};
	}

	nvrhi::ITexture* SceneRenderer::State::RecordLights2D(Detail::SceneRendererFrame& frame, const glm::mat4& viewProjection)
	{
		// Without a lit quad nothing is recorded and the targets are not created: unlit sprites keep their texels.
		if (std::ranges::find(SpriteQuadLit, uint8_t{ 1 }) == SpriteQuadLit.end())
			return nullptr;
		ENGINE_CORE_ASSERT(Pipelines->Lights2D != nullptr, "the pipeline set has no Lights2D pass");
		if (Pipelines->Lights2D == nullptr)
		{
			frame.KeepFirstError(MakeError(ErrorCode::InvalidState, "the scene renderer's pipelines have no Lights2D pass; the lit sprites are not drawn"));
			return nullptr;
		}
		if (Status ensured = EnsureLights2DTargets(); !ensured)
		{
			frame.KeepFirstError(std::move(ensured));
			return nullptr;
		}
		nvrhi::ICommandList& commandList = *frame.CommandList;
		frame.Recording->BeginPass("Lights2D", &commandList);
		RenderPassCounters counters;

		// (1) The normals: the flat normal everywhere, then the lit quads' in draw order when one has a normal map. A failed
		// record leaves the normals flat and keeps its error.
		commandList.clearTextureFloat(Targets.Lights2DNormals, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 1.0f, 0.0f));
		bool normalMapped = false;
		for (size_t index = 0; index < SpriteQuads.size() && !normalMapped; ++index)
			normalMapped = SpriteQuadLit[index] != 0 && SpriteQuads[index].NormalMap != nullptr;
		if (normalMapped)
		{
			LitSpriteQuads.clear();
			for (size_t index = 0; index < SpriteQuads.size(); ++index)
			{
				if (SpriteQuadLit[index] != 0)
					LitSpriteQuads.push_back(SpriteQuads[index]);
			}
			Result<uint32_t> drawn = Pipelines->Quads->Record(commandList, SpriteBindings,
				{ .Quads = LitSpriteQuads, .Target = QuadBatchTarget::Normals, .Framebuffer = Targets.Lights2DNormalsFramebuffer, .Transform = viewProjection },
				counters);
			if (!drawn)
				frame.KeepFirstError(std::unexpected(std::move(drawn).error()));
			LitSpriteQuads.clear();
		}

		// (2) The lights the view draws: the first MaxVisibleLights2D of the snapshot's that reach it, in their order.
		VisibleLights2D.clear();
		uint32_t skipped = 0;
		for (const Light2DItem& item : frame.Snapshot->Lights2D)
		{
			if (!Light2DReachesView(item, viewProjection))
				continue;
			if (VisibleLights2D.size() < MaxVisibleLights2D)
				VisibleLights2D.push_back(item);
			else
				++skipped;
		}
		if (skipped > 0 && !Light2DLimitLogged)
		{
			Light2DLimitLogged = true;
			ENGINE_CORE_WARN("{} 2D light(s) that reach the view exceed its budget of {}; the view draws the first {} in canonical entity order (logged "
							 "once per view)",
				skipped, MaxVisibleLights2D, MaxVisibleLights2D);
		}
		Status added = Pipelines->Lights2D->Record(commandList, Lights2DBindings,
			{ .Lights = VisibleLights2D, .ViewProjection = viewProjection, .Normals = Targets.Lights2DNormals, .Light = Targets.Lights2DLightFramebuffer },
			counters);
		VisibleLights2D.clear();
		frame.Recording->EndPass(counters);
		if (!added)
		{
			frame.KeepFirstError(std::move(added));
			return nullptr;
		}
		return Targets.Lights2DLight.Get();
	}

	void SceneRenderer::State::RecordSpriteQuads(Detail::SceneRendererFrame& frame, const glm::mat4& viewProjection, nvrhi::ITexture* light)
	{
		nvrhi::ICommandList& commandList = *frame.CommandList;
		frame.Recording->BeginPass("Sprites", &commandList);
		RenderPassCounters counters;
		const std::span<const QuadDraw> quads = SpriteQuads;
		size_t first = 0;
		while (first < quads.size())
		{
			const bool lit = SpriteQuadLit[first] != 0;
			size_t end = first + 1;
			while (end < quads.size() && (SpriteQuadLit[end] != 0) == lit)
				++end;
			if (!lit || light != nullptr)
			{
				Result<uint32_t> drawn = Pipelines->Quads->Record(commandList, SpriteBindings,
					{ .Quads = quads.subspan(first, end - first),
						.Target = lit ? QuadBatchTarget::HdrLit : QuadBatchTarget::Hdr,
						.Framebuffer = Targets.ForwardFramebuffer,
						.Transform = viewProjection,
						.Light = lit ? light : nullptr },
					counters);
				if (!drawn)
					frame.KeepFirstError(std::unexpected(std::move(drawn).error()));
			}
			first = end;
		}
		frame.Recording->EndPass(counters);
	}

	void SceneRenderer::State::AppendTilemapQuads(Detail::SceneRendererFrame& frame, const TilemapItem& item, const glm::mat4& viewProjection,
		const std::function<const SpriteSheetData*(AssetHandle)>& resolveSheet)
	{
		if (!Utils::IsFinite(item.World))
		{
			frame.KeepFirstError(MakeError(ErrorCode::InvalidArgument,
				"the world matrix of tilemap entity {} in the render snapshot is not finite; its draw is skipped", item.Entity));
			return;
		}
		if (!std::isfinite(item.PixelsPerUnit) || !(item.PixelsPerUnit > 0.0f))
		{
			frame.KeepFirstError(MakeError(ErrorCode::InvalidArgument,
				"tilemap entity {} in the render snapshot has {} pixels per unit, not a finite positive value; its draw is skipped", item.Entity,
				item.PixelsPerUnit));
			return;
		}
		// Tilemaps have no placeholder (TilemapRendererComponent.h): a missing or failed one draws nothing.
		const Result<AssetRef<Asset>> loaded = Assets->Load(item.Tilemap);
		AssetRef<TilemapData> tilemap = loaded.has_value() ? AssetCast<TilemapData>(*loaded) : nullptr;
		if (tilemap == nullptr)
		{
			Utils::ReportTilemapFailure(*Assets, item.Tilemap, loaded.has_value() ? nullptr : &loaded.error());
			return;
		}

		// This render's quad of each palette entry: the sprite its tiles draw as, unflipped (SpriteRendererComponent.h's one
		// rule, as the sprites above: the sheet's frame over the sheet's texture, white without one, a missing or failed
		// texture's placeholder shown whole over the quad).
		std::vector<Utils::TilemapPaletteQuad> palette(tilemap->Palette.size());
		std::vector<std::pair<AssetHandle, uint64_t>> sheetVersions;
		for (size_t index = 0; index < palette.size(); ++index)
		{
			const TilemapTile& entry = tilemap->Palette[index];
			Utils::TilemapPaletteQuad& quad = palette[index];
			if (!entry.Sheet.GetHandle().IsValid())
				continue;
			const SpriteSheetData* sheet = resolveSheet(entry.Sheet.GetHandle());
			sheetVersions.emplace_back(entry.Sheet.GetHandle(), Assets->GetVersion(entry.Sheet.GetHandle()));
			if (sheet == nullptr || entry.Frame >= sheet->Frames.size())
				continue; // no quad: TILEMAP_FRAME_OUT_OF_RANGE, or the sheet's failure reported by resolveSheet
			const SpriteRect& rect = sheet->Frames[entry.Frame].Rect;
			glm::uvec2 textureSize = glm::uvec2(rect.X + rect.Width, rect.Y + rect.Height);
			bool wholeTexture = false;
			bool hasTexture = true;
			if (sheet->Texture.IsValid())
			{
				const GpuTexture& mirror = Cache->GetTexture(sheet->Texture.GetHandle());
				if (mirror.Texture == nullptr)
					hasTexture = false; // not even the placeholder could be created (GpuResourceCache.h)
				else
				{
					quad.Texture = mirror.Texture.Get();
					wholeTexture = mirror.IsPlaceholder;
					textureSize = glm::uvec2(std::max(quad.Texture->getDesc().width, 1u), std::max(quad.Texture->getDesc().height, 1u));
				}
			}
			const std::optional<SpriteFrameGeometry> geometry = GetSpriteFrameGeometry(sheet, entry.Frame, textureSize);
			if (!geometry.has_value())
				continue;
			const SpriteLocalQuad local = ComputeSpriteLocalQuad(*geometry, item.PixelsPerUnit, false, false);
			quad.HasQuad = true;
			quad.Draws = hasTexture;
			quad.Corners = local.Corners;
			quad.TexCoordMin = wholeTexture ? glm::vec2(0.0f) : local.TexCoordMin;
			quad.TexCoordMax = wholeTexture ? glm::vec2(1.0f) : local.TexCoordMax;
		}
		std::ranges::sort(sheetVersions);
		const auto duplicates = std::ranges::unique(sheetVersions);
		sheetVersions.erase(duplicates.begin(), duplicates.end());

		// The chunks' bounds, kept between renders (TilemapDrawCache.h) and rebuilt when stale: each the union of its tiles'
		// local quads (the cell's centre plus the entry's quad), whatever their frames' sizes and pivots.
		const uint64_t version = Assets->GetVersion(item.Tilemap);
		auto cache = std::ranges::find_if(TilemapCaches, [&item](const Detail::TilemapDrawCache& candidate)
		{
			return candidate.Tilemap == item.Tilemap && candidate.PixelsPerUnit == item.PixelsPerUnit;
		});
		if (cache == TilemapCaches.end())
		{
			TilemapCaches.push_back(Detail::TilemapDrawCache{ .Tilemap = item.Tilemap, .PixelsPerUnit = item.PixelsPerUnit });
			cache = std::prev(TilemapCaches.end());
		}
		if (cache->Data != tilemap || cache->Version != version || cache->SheetVersions != sheetVersions)
		{
			const int64_t chunkSize = tilemap->ChunkSize;
			cache->Chunks.assign(tilemap->Chunks.size(), Detail::TilemapChunkBounds{});
			for (size_t chunkIndex = 0; chunkIndex < tilemap->Chunks.size(); ++chunkIndex)
			{
				const TilemapChunk& chunk = tilemap->Chunks[chunkIndex];
				Detail::TilemapChunkBounds& bounds = cache->Chunks[chunkIndex];
				for (size_t cellIndex = 0; cellIndex < chunk.Tiles.size(); ++cellIndex)
				{
					const uint32_t value = chunk.Tiles[cellIndex];
					if (value == 0 || value > palette.size() || !palette[value - 1].HasQuad)
						continue;
					const int64_t column = static_cast<int64_t>(cellIndex) % chunkSize;
					const int64_t row = static_cast<int64_t>(cellIndex) / chunkSize;
					const glm::ivec2 cell(static_cast<int32_t>(chunk.X * chunkSize + column), static_cast<int32_t>(chunk.Y * chunkSize + row));
					const glm::vec2 center = GetTilemapCellCenter(*tilemap, cell);
					for (const glm::vec2& corner : palette[value - 1].Corners)
					{
						const glm::vec2 point = center + corner;
						bounds.Min = bounds.HasTiles ? glm::min(bounds.Min, point) : point;
						bounds.Max = bounds.HasTiles ? glm::max(bounds.Max, point) : point;
						bounds.HasTiles = true;
					}
				}
			}
			cache->Data = tilemap;
			cache->Version = version;
			cache->SheetVersions = std::move(sheetVersions);
		}
		cache->Used = true;

		// The chunks whose bounds meet the view: a chunk is culled only when its bounds, in world space, lie wholly outside
		// one plane of the clip volume, so a tile larger than its cell never pops in or out at the view's edge.
		TilemapChunksDrawn.assign(cache->Chunks.size(), 0);
		for (size_t chunkIndex = 0; chunkIndex < cache->Chunks.size(); ++chunkIndex)
		{
			const Detail::TilemapChunkBounds& bounds = cache->Chunks[chunkIndex];
			if (!bounds.HasTiles)
				continue;
			const std::array<glm::vec2, 4> corners = { bounds.Min, glm::vec2(bounds.Max.x, bounds.Min.y), bounds.Max, glm::vec2(bounds.Min.x, bounds.Max.y) };
			std::array<glm::vec4, 4> clip{};
			for (size_t index = 0; index < corners.size(); ++index)
				clip[index] = viewProjection * (item.World * glm::vec4(corners[index], 0.0f, 1.0f));
			TilemapChunksDrawn[chunkIndex] = Utils::IsOutsideClipVolume(clip) ? 0 : 1;
		}

		// The drawn chunks' tiles in the tilemap's cell order, never chunk by chunk, each the quad of its sprite at the tile's
		// world matrix (the tilemap's times the translation to its cell's centre), Color as the quad's colour.
		const auto includeChunk = [this](size_t chunk)
		{
			return TilemapChunksDrawn[chunk] != 0;
		};
		ForEachTilemapTileInDrawOrder(*tilemap, false, includeChunk, [this, &item, &tilemap, &palette](size_t /*chunk*/, glm::ivec2 cell, uint32_t value)
		{
			if (value > palette.size() || !palette[value - 1].Draws)
				return true;
			// M21's milestone review: the view's tiles past MaxTileQuadsPerView are counted, never built.
			if (TileQuadsDrawn >= MaxTileQuadsPerView)
			{
				if (TileQuadsDropped < std::numeric_limits<uint32_t>::max())
					++TileQuadsDropped;
				return true;
			}
			++TileQuadsDrawn;
			const Utils::TilemapPaletteQuad& entry = palette[value - 1];
			const glm::mat4 world = GetTilemapTileWorld(item.World, *tilemap, cell);
			QuadDraw quad{ .TexCoordMin = entry.TexCoordMin, .TexCoordMax = entry.TexCoordMax, .Color = item.Color, .Texture = entry.Texture, .Sampler = QuadSampler::Point };
			for (size_t index = 0; index < quad.Corners.size(); ++index)
				quad.Corners[index] = glm::vec3(world * glm::vec4(entry.Corners[index], 0.0f, 1.0f));
			AddSpriteQuad(quad, item.Lit);
			return true;
		});
	}

	void SceneRenderer::State::ReleaseUnusedTilemaps()
	{
		std::erase_if(TilemapCaches, [](const Detail::TilemapDrawCache& cache)
		{
			return !cache.Used;
		});
		for (Detail::TilemapDrawCache& cache : TilemapCaches)
			cache.Used = false;
	}

	void SceneRenderer::State::RecordHdrPostStage(Detail::SceneRendererFrame& frame)
	{
		RecordTaa(frame);
		const PostProcessSettings& post = frame.Snapshot->Post;
		frame.Bloom = nullptr;
		frame.BloomIntensity = std::isfinite(post.BloomIntensity) ? std::clamp(post.BloomIntensity, 0.0f, 1.0f) : 0.0f;
		// The debug views and the 2D colour path (decision 7: a bloom of any intensity changes texels) draw no bloom.
		if (frame.IsLit && !frame.Wireframe && !frame.PixelArt && post.BloomEnabled && frame.BloomIntensity > 0.0f)
		{
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
		RecordExposure(frame);
	}

	Status SceneRenderer::State::EnsureTaaHistory()
	{
		if (Targets.TaaHistory[0] != nullptr && Targets.TaaHistory[1] != nullptr)
			return {};
		TaaHistoryValid = false;
		const nvrhi::TextureDesc desc = TaaPass::GetHistoryDesc(Targets.Width, Targets.Height);
		std::array<nvrhi::TextureHandle, 2> history{};
		for (nvrhi::TextureHandle& image : history)
		{
			ENGINE_TRY_ASSIGN(image, TargetPool->Acquire(desc));
		}
		Targets.TaaHistory = std::move(history);
		TaaHistoryIndex = 0;
		return {};
	}

	void SceneRenderer::State::RecordTaa(Detail::SceneRendererFrame& frame)
	{
		if (!frame.Taa)
		{
			TaaHistoryValid = false;
			return;
		}
		if (const Status ensured = EnsureTaaHistory(); !ensured)
		{
			frame.KeepFirstError(ensured);
			TaaHistoryValid = false;
			return;
		}
		nvrhi::ICommandList& commandList = *frame.CommandList;
		frame.Recording->BeginPass("TAA", &commandList);
		RenderPassCounters counters;
		// The temporal-state section (decision 10): a reset history resolves to the current image, as does a view without the
		// resolved image of its previous render.
		const bool reset = !TaaHistoryValid || !frame.Camera.PreviousViewProjection.has_value();
		const uint32_t resolved = reset ? 0U : 1U - TaaHistoryIndex;
		const Status recorded = Pipelines->Taa->Record(commandList, TaaBindings,
			{ .SceneColor = Targets.SceneColor, .Velocity = Targets.SceneVelocity, .History = reset ? nullptr : Targets.TaaHistory[TaaHistoryIndex].Get(), .Resolved = Targets.TaaHistory[resolved], .Reset = reset },
			counters);
		frame.Recording->EndPass(counters);
		TaaHistoryValid = recorded.has_value();
		TaaHistoryIndex = resolved;
		frame.KeepFirstError(recorded);
	}

	Status SceneRenderer::State::EnsureExposureBuffers()
	{
		if (ExposureHistogramBuffer == nullptr)
		{
			ENGINE_TRY_ASSIGN(ExposureHistogramBuffer, Device->CreateBuffer(ExposurePass::GetHistogramDesc()));
		}
		if (ExposureStateBuffer == nullptr)
		{
			ENGINE_TRY_ASSIGN(ExposureStateBuffer, Device->CreateBuffer(ExposurePass::GetStateDesc()));
		}
		return {};
	}

	void SceneRenderer::State::RecordExposure(Detail::SceneRendererFrame& frame)
	{
		const PostProcessSettings& post = frame.Snapshot->Post;
		frame.AdaptedExposure = nullptr;
		// ExposureMode Auto acts as Manual in the debug views, the wireframe (no lit image to meter) and the 2D colour path
		// (decision 10: pixel-art mode keeps exposure 1), as bloom does.
		if (!frame.IsLit || frame.Wireframe || frame.PixelArt || post.ExposureMode != RenderExposureMode::Auto)
		{
			ExposureValid = false;
			return;
		}
		if (const Status ensured = EnsureExposureBuffers(); !ensured)
		{
			frame.KeepFirstError(ensured);
			ExposureValid = false;
			return;
		}
		nvrhi::ICommandList& commandList = *frame.CommandList;
		frame.Recording->BeginPass("Exposure", &commandList);
		RenderPassCounters counters;
		// The temporal-state section (decision 10): a reset history converges at once, as does a view whose state buffer
		// holds no adapted exposure of its previous render.
		const Status recorded = Pipelines->Exposure->Record(commandList, ExposureBindings,
			{ .SceneColor = Targets.SceneColor, .Bloom = frame.Bloom, .BloomIntensity = frame.Bloom != nullptr ? frame.BloomIntensity : 0.0f, .Histogram = ExposureHistogramBuffer, .State = ExposureStateBuffer, .MinEV = post.AutoExposureMinEV, .MaxEV = post.AutoExposureMaxEV, .SpeedUp = post.AutoExposureSpeedUp, .SpeedDown = post.AutoExposureSpeedDown, .DeltaTime = frame.Camera.DeltaTime, .Reset = !ExposureValid || !frame.Camera.PreviousViewProjection.has_value() },
			counters);
		frame.Recording->EndPass(counters);
		ExposureValid = recorded.has_value();
		if (recorded)
			frame.AdaptedExposure = ExposureStateBuffer;
		else
			frame.KeepFirstError(recorded);
	}

	void SceneRenderer::State::RecordTonemapStage(Detail::SceneRendererFrame& frame)
	{
		const PostProcessSettings& post = frame.Snapshot->Post;
		nvrhi::ICommandList& commandList = *frame.CommandList;
		RenderRecordingContext& recording = *frame.Recording;
		// Dedicated data views bypass all tone/color transforms; Tonemap names the final output stage in every view.
		recording.BeginPass("Tonemap", &commandList);
		Status encoded;
		// M23 (stream C): the composite binds the cluster grid and masks (RecordLightClusters) for the LightCount view; without
		// them (their creation error is the render's) it is skipped.
		if (frame.IsDataView && (LightClusterConstantsBuffer == nullptr || LightClusterBuffer == nullptr))
			encoded = MakeError(ErrorCode::Gpu, "the data views' composite has no light cluster buffers to bind");
		else if (frame.IsDataView)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, ViewConstantsBuffer),
				nvrhi::BindingSetItem::ConstantBuffer(1, ShadowConstantsBuffer),
				nvrhi::BindingSetItem::ConstantBuffer(Utils::SceneDataViewClustersRegister, LightClusterConstantsBuffer),
				nvrhi::BindingSetItem::Texture_SRV(0, Targets.SceneDepth),
				nvrhi::BindingSetItem::Texture_SRV(1, frame.Occlusion),
				nvrhi::BindingSetItem::Texture_SRV(2, frame.LinearDepth),
				nvrhi::BindingSetItem::Texture_SRV(3, Targets.SceneNormals),
				nvrhi::BindingSetItem::Texture_SRV(4, frame.IsOverdraw ? Targets.Overdraw : Targets.SceneColor),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(Utils::SceneDataViewMasksRegister, LightClusterBuffer),
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
			// M26 (Docs/Decisions/0037-m26-contract.md decisions 10 and 15, stream X-B): Post's grading LUT, where the tonemapper
			// follows Post; never on the 2D colour path, which delivers a sprite's texels unchanged. A LUT that does not load or
			// upload grades nothing (GpuResourceCache::GetLut reports why).
			const GpuLut* lut = tonemapped && post.Lut.IsValid() ? Cache->GetLut(post.Lut) : nullptr;
			TonemapPassInputs inputs{ .ViewConstants = ViewConstantsBuffer, .SceneColor = Targets.SceneColor, .Bloom = frame.Bloom, .BloomIntensity = frame.Bloom != nullptr ? frame.BloomIntensity : 0.0f, .BlueNoise = frame.BlueNoise, .LdrColor = Targets.Ldr[0], .Settings = settings, .AdaptedExposure = frame.AdaptedExposure };
			if (lut != nullptr)
			{
				inputs.Lut = lut->Texture;
				inputs.LutDomainMin = lut->DomainMin;
				inputs.LutDomainMax = lut->DomainMax;
				inputs.LutIntensity = post.LutIntensity;
			}
			encoded = Pipelines->Tonemap->Record(commandList, TonemapBindings, inputs);
		}
		recording.EndPass({ .Dispatches = encoded ? 1U : 0U });
		frame.KeepFirstError(std::move(encoded));
		frame.SceneTarget = 0;
	}

	void SceneRenderer::State::RecordLdrPostStage(Detail::SceneRendererFrame& frame)
	{
		const PostProcessSettings& post = frame.Snapshot->Post;
		// FXAA blends texels across edges: never in a debug view, the wireframe or the 2D colour path (decision 7).
		if (!frame.IsLit || frame.Wireframe || frame.PixelArt || post.AntiAliasing != RenderAntiAliasing::Fxaa)
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
