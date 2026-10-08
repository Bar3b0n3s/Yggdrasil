#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/BloomPass.h"
#include "Engine/Renderer/BrdfLut.h"
#include "Engine/Renderer/DebugRenderer.h"
#include "Engine/Renderer/FxaaPass.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Engine/Renderer/SkyboxPass.h"
#include "Engine/Renderer/TextRenderer.h"
#include "Engine/Renderer/TonemapPass.h"
#include "Shared/DrawConstants.h"
#include "Shared/LightingConstants.h"
#include "Shared/MaterialConstants.h"
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
#include <string>
#include <tuple>
#include <utility>

// The pipelines (SceneRendererPipelines): the Scene program's prepass and forward pipelines, each in two windings, and the
// passes the set owns. The M8 contract created them all here (Docs/Decisions/0013-m8-decisions.md decision 7): TonemapPass
// holds the walking skeleton's tonemap; the other passes belong to streams A to D, create pipelines from their stub
// programs (no bindings) and record nothing until they land. Stream A replaces the mesh pipelines with the PBR variants of
// SceneRenderer.h and records the M8 pass list. A draw whose world matrix mirrors (a negative determinant) turns the mesh's
// counter-clockwise front faces clockwise on screen, so it uses the "Mirrored" variants (frontCounterClockwise = false,
// the same back-face culling) and the shaders sign its normals by the determinant; every other draw uses
// frontCounterClockwise = true (§8.3). NVRHI requires a binding set made for the pipeline's own binding layout objects, so
// each variant has its own set-0 binding set and each material a set-1 binding set per forward variant.
//
// Per renderer: the targets (SceneDepth, SceneNormals, SceneColor, LdrColor) and their framebuffers, rebuilt by Resize; the
// ViewConstants and LightingConstants buffers, written by every Render; the material cache, one MaterialConstants buffer
// per material handle, rewritten when the material's constants change (a hot reload, a placeholder replaced by the real
// asset) and released after MaterialIdleRenders renders without a draw using it; one PassBindingCache per shared pass,
// cleared by Resize and trimmed to what the render used at the end of every Render. Every buffer is a keepInitialState
// constant buffer written through the command list, so NVRHI orders its writes after the reads of earlier submissions.

namespace Engine {

	// The startup count is the sum of the pipelines each part of the set declares (SceneRenderer.h).
	static_assert(SceneRendererPipelines::StartupPipelineCount
		== SceneRendererPipelines::MeshPipelineCount + SkyboxPass::PipelineCount + BloomPass::PipelineCount + TonemapPass::PipelineCount
			+ FxaaPass::PipelineCount + DebugRenderer::PipelineCount + TextRenderer::PipelineCount + BrdfLut::PipelineCount);

	namespace Utils {

		// Renders a cached material may go without a draw before its buffer and binding sets are released.
		constexpr uint64_t MaterialIdleRenders = 64;
		// The winding variants: index 0 for ordinary draws, 1 for mirrored ones.
		constexpr size_t WindingCount = 2;
		constexpr size_t MirroredWinding = 1;

		// NVRHI's validation counts push constants as a constant-buffer slot (DX12 root constants), so they may not share
		// one with a constant buffer of the layout; Vulkan ignores the slot. b0 to b2 of set 0 are the per-view constants of
		// §8.4, so the draw's push constants take b3.
		constexpr uint32_t DrawConstantsSlot = 3;

		constexpr std::string_view SceneProgram = "Scene";

		// The set-0 layout of the prepass: ViewConstants and the draw's push constants.
		static nvrhi::BindingLayoutDesc MakePrepassViewLayout()
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::All;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {
				nvrhi::BindingLayoutItem::ConstantBuffer(0),
				nvrhi::BindingLayoutItem::PushConstants(DrawConstantsSlot, sizeof(DrawConstants)),
			};
			return layout;
		}

		// The set-0 layout of the forward pass: ViewConstants, LightingConstants and the draw's push constants.
		static nvrhi::BindingLayoutDesc MakeForwardViewLayout()
		{
			nvrhi::BindingLayoutDesc layout = MakePrepassViewLayout();
			layout.bindings = {
				nvrhi::BindingLayoutItem::ConstantBuffer(0),
				nvrhi::BindingLayoutItem::ConstantBuffer(2),
				nvrhi::BindingLayoutItem::PushConstants(DrawConstantsSlot, sizeof(DrawConstants)),
			};
			return layout;
		}

		// The set-1 layout (per material, §8.4): MaterialConstants.
		static nvrhi::BindingLayoutDesc MakeMaterialLayout()
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Pixel;
			layout.registerSpace = 1;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = { nvrhi::BindingLayoutItem::ConstantBuffer(0) };
			return layout;
		}

		static PipelineLayoutDescription MakePrepassDescription(bool mirrored)
		{
			return {
				.Name = mirrored ? "ScenePrepassMirrored" : "ScenePrepass",
				.Program = std::string(SceneProgram),
				.Entries = { "VSMain", "PSPrepass" },
				.BindingLayouts = { MakePrepassViewLayout() },
				.ConstantBuffers = { { .Set = 0, .Register = 0, .ByteSize = sizeof(ViewConstants) } },
			};
		}

		static PipelineLayoutDescription MakeForwardDescription(bool mirrored)
		{
			return {
				.Name = mirrored ? "SceneForwardMirrored" : "SceneForward",
				.Program = std::string(SceneProgram),
				.Entries = { "VSMain", "PSForward" },
				.BindingLayouts = { MakeForwardViewLayout(), MakeMaterialLayout() },
				.ConstantBuffers = {
					{ .Set = 0, .Register = 0, .ByteSize = sizeof(ViewConstants) },
					{ .Set = 0, .Register = 2, .ByteSize = sizeof(LightingConstants) },
					{ .Set = 1, .Register = 0, .ByteSize = sizeof(MaterialConstants) },
				},
			};
		}

		// Appends `descriptions` to `all`.
		static void AppendDescriptions(std::vector<PipelineLayoutDescription>& all, std::vector<PipelineLayoutDescription> descriptions)
		{
			all.insert(all.end(), std::make_move_iterator(descriptions.begin()), std::make_move_iterator(descriptions.end()));
		}

		// MeshVertex (§6.8): the position and the normal, the attributes the Scene program reads, at locations 0 and 1.
		static std::vector<nvrhi::VertexAttributeDesc> MakeMeshVertexAttributes()
		{
			constexpr uint32_t Stride = sizeof(MeshVertex);
			return {
				nvrhi::VertexAttributeDesc()
					.setName("POSITION")
					.setFormat(nvrhi::Format::RGB32_FLOAT)
					.setOffset(static_cast<uint32_t>(offsetof(MeshVertex, Position)))
					.setElementStride(Stride),
				nvrhi::VertexAttributeDesc()
					.setName("NORMAL")
					.setFormat(nvrhi::Format::RGB32_FLOAT)
					.setOffset(static_cast<uint32_t>(offsetof(MeshVertex, Normal)))
					.setElementStride(Stride),
			};
		}

		// A mesh pipeline: back-face culling with the winding of `mirrored`, reverse-Z depth (GreaterOrEqual) written or not.
		static GraphicsPipelineSpecification MakeMeshPipelineSpecification(PipelineLayoutDescription layout, bool mirrored, bool depthWrite,
			nvrhi::Format colorFormat)
		{
			GraphicsPipelineSpecification specification;
			specification.Layout = std::move(layout);
			specification.VertexAttributes = MakeMeshVertexAttributes();
			specification.RenderState.rasterState.setCullMode(nvrhi::RasterCullMode::Back).setFrontCounterClockwise(!mirrored);
			specification.RenderState.depthStencilState.setDepthTestEnable(true)
				.setDepthWriteEnable(depthWrite)
				.setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual)
				.setStencilEnable(false);
			specification.Framebuffer.addColorFormat(colorFormat);
			specification.Framebuffer.setDepthFormat(SceneDepthFormat);
			return specification;
		}

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

		// A constant buffer of `size` bytes kept in the ConstantBuffer state between command lists.
		static Result<nvrhi::BufferHandle> CreateConstantBuffer(GraphicsDevice& device, size_t size, std::string debugName)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = size;
			desc.isConstantBuffer = true;
			desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
			desc.keepInitialState = true;
			desc.debugName = std::move(debugName);
			return device.CreateBuffer(desc);
		}

		// How a target is written: rendered into (a colour or depth attachment) or by a compute pass (a storage image).
		enum class TargetUsage : uint8_t
		{
			Attachment,
			StorageImage
		};

		// One of the renderer's targets, sampled by later passes and copied by Readback, kept between command lists in its
		// writing state (RenderTarget, DepthWrite) or, for the storage image LdrColor, in ShaderResource, the state BlitPass
		// samples it in.
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
			desc.isRenderTarget = usage == TargetUsage::Attachment;
			desc.isUAV = usage == TargetUsage::StorageImage;
			if (usage == TargetUsage::StorageImage)
				desc.initialState = nvrhi::ResourceStates::ShaderResource;
			else
				desc.initialState = isDepth ? nvrhi::ResourceStates::DepthWrite : nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			desc.debugName = std::format("SceneRenderer.{}", name);
			return device.CreateTexture(desc);
		}

		// The clip-space test of §8.3 pass 1 for a submesh's bounds: culled when all eight corners lie outside one plane of
		// the view volume (-w <= x, y <= w and 0 <= z <= w: reverse-Z, so z > w is nearer than the near plane and z < 0
		// beyond an orthographic far plane), or when every corner is farther than FarClip along the view direction (the
		// perspective far plane is infinite, so FarClip is its culling distance). Empty bounds are never culled.
		static bool IsOutsideView(const Aabb& bounds, const glm::mat4& worldViewProjection, const glm::mat4& worldView, float farClip)
		{
			if (bounds.IsEmpty())
				return false;
			std::array<bool, 7> allOutside = { true, true, true, true, true, true, true };
			for (uint32_t corner = 0; corner < 8; ++corner)
			{
				const glm::vec4 local((corner & 1U) != 0 ? bounds.Max.x : bounds.Min.x, (corner & 2U) != 0 ? bounds.Max.y : bounds.Min.y,
					(corner & 4U) != 0 ? bounds.Max.z : bounds.Min.z, 1.0f);
				const glm::vec4 clip = worldViewProjection * local;
				const float viewDistance = -(worldView * local).z;
				allOutside[0] = allOutside[0] && clip.x < -clip.w;
				allOutside[1] = allOutside[1] && clip.x > clip.w;
				allOutside[2] = allOutside[2] && clip.y < -clip.w;
				allOutside[3] = allOutside[3] && clip.y > clip.w;
				allOutside[4] = allOutside[4] && clip.z > clip.w;
				allOutside[5] = allOutside[5] && clip.z < 0.0f;
				allOutside[6] = allOutside[6] && viewDistance > farClip;
			}
			return std::ranges::any_of(allOutside, [](bool outside)
			{
				return outside;
			});
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

		// The forward pass's lighting (Shared/LightingConstants.h): the snapshot's first directional light with a finite,
		// non-zero direction and radiance, plus the ambient FallbackColor * Intensity.
		static LightingConstants MakeLightingConstants(const RenderSnapshot& snapshot, uint32_t& lightCount)
		{
			LightingConstants lighting{};
			lighting.LightDirection = glm::vec3(0.0f, 0.0f, -1.0f);
			lighting.LightRadiance = glm::vec3(0.0f);
			lightCount = 0;
			for (const LightData& light : snapshot.Lights)
			{
				if (light.Type != RenderLightType::Directional)
					continue;
				const glm::vec3 radiance = light.Color * light.Intensity;
				const float length = glm::length(light.Direction);
				if (IsFinite(radiance) && std::isfinite(length) && length > 0.0f)
				{
					lighting.LightDirection = light.Direction / length;
					lighting.LightRadiance = radiance;
					lightCount = 1;
				}
				break;
			}
			const glm::vec3 ambient = snapshot.Environment.FallbackColor * snapshot.Environment.Intensity;
			lighting.AmbientRadiance = IsFinite(ambient) ? glm::max(ambient, glm::vec3(0.0f)) : glm::vec3(0.0f);
			return lighting;
		}

		// 2^ExposureEV (§8.9); 1 for a value that is not finite.
		static float GetExposure(const PostProcessSettings& post)
		{
			if (!std::isfinite(post.ExposureEV))
				return 1.0f;
			const float exposure = std::exp2(post.ExposureEV);
			return std::isfinite(exposure) ? exposure : 1.0f;
		}

		// The material slot `slot` of `item` drawing a submesh of `mesh`: the item's slot when it is set, else the mesh's
		// default material, else the built-in Default material.
		static AssetHandle SelectMaterial(const MeshDrawItem& item, const GpuMesh& mesh, uint32_t slot)
		{
			if (slot < item.Materials.size() && item.Materials[slot].IsValid())
				return item.Materials[slot];
			if (slot < mesh.DefaultMaterials.size() && mesh.DefaultMaterials[slot].IsValid())
				return mesh.DefaultMaterials[slot];
			return BuiltinAssetHandles::DefaultMaterial;
		}

	}

	namespace {

		// One submesh draw of the frame, resolved by Prepare.
		struct SceneDraw
		{
			nvrhi::BufferHandle VertexBuffer{};
			nvrhi::BufferHandle IndexBuffer{};
			uint32_t IndexOffset = 0;
			uint32_t IndexCount = 0;
			AssetHandle Mesh{};
			AssetHandle Material{};
			size_t Winding = 0; // 0, or Utils::MirroredWinding
			DrawConstants Constants{};
		};

		// A material's GPU state: its constants and the set-1 binding set of each forward variant.
		struct MaterialEntry
		{
			nvrhi::BufferHandle Constants{};
			std::array<nvrhi::BindingSetHandle, Utils::WindingCount> BindingSets{};
			std::optional<MaterialConstants> Written{}; // what the buffer holds; nullopt before the first write
			uint64_t LastUsedRender = 0;
		};

		// The size-dependent targets of a renderer.
		struct SceneTargets
		{
			uint32_t Width = 0;
			uint32_t Height = 0;
			nvrhi::TextureHandle SceneDepth{};
			nvrhi::TextureHandle SceneNormals{};
			nvrhi::TextureHandle SceneColor{};
			nvrhi::TextureHandle LdrColor{};
			nvrhi::FramebufferHandle PrepassFramebuffer{};
			nvrhi::FramebufferHandle ForwardFramebuffer{};
		};

	}

	struct SceneRendererPipelines::State
	{
		GraphicsDevice* Device = nullptr;   // documented back-reference
		PipelineFactory* Factory = nullptr; // documented back-reference (EnsureDebugView)
		std::array<GraphicsPipeline, Utils::WindingCount> Prepass{};
		std::array<GraphicsPipeline, Utils::WindingCount> Forward{};
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
		SceneRendererPipelines::State* Pipelines = nullptr;
		GpuResourceCache* Cache = nullptr;
		AssetManager* Assets = nullptr;

		// The binding sets of the shared passes for this view (PassBindingCache.h; the other passes' caches arrive with
		// their records, stream A).
		PassBindingCache TonemapBindings{};

		SceneTargets Targets{};
		nvrhi::BufferHandle ViewConstantsBuffer{};
		nvrhi::BufferHandle LightingConstantsBuffer{};
		std::array<nvrhi::BindingSetHandle, Utils::WindingCount> PrepassBindings{};
		std::array<nvrhi::BindingSetHandle, Utils::WindingCount> ForwardBindings{};
		std::map<AssetHandle, MaterialEntry> Materials{};
		std::vector<SceneDraw> Draws{}; // reused by every Render
		uint64_t RenderCount = 0;
		SceneRenderStats Stats{};

		// The targets of `width` x `height` with their framebuffers and the tonemap binding set.
		[[nodiscard]] Result<SceneTargets> CreateTargets(uint32_t width, uint32_t height) const;
		// The cached entry of `handle` with its constants written into `commandList` when they changed; nullptr when its
		// buffer or binding sets cannot be created (reported as the material's ASSET_UPLOAD_FAILED diagnostic).
		[[nodiscard]] MaterialEntry* PrepareMaterial(nvrhi::ICommandList& commandList, AssetHandle handle);
		// Pass 1 (Prepare): the draws of `snapshot` into Draws, culled and sorted. Returns the first non-finite world
		// matrix's error, after skipping that draw.
		[[nodiscard]] Status PrepareDraws(const RenderSnapshot& snapshot, const ViewConstants& view);
		// Passes 4 and 7: the draws of Draws into the prepass or the forward framebuffer.
		void RecordMeshPass(nvrhi::ICommandList& commandList, bool forward, const std::map<AssetHandle, MaterialEntry*>& materials);
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
		for (size_t winding = 0; winding < Utils::WindingCount; ++winding)
		{
			const bool mirrored = winding == Utils::MirroredWinding;
			ENGINE_TRY_ASSIGN(state.Prepass[winding], pipelines.CreateGraphicsPipeline(Utils::MakeMeshPipelineSpecification(Utils::MakePrepassDescription(mirrored), mirrored, true, SceneNormalsFormat)));
			ENGINE_TRY_ASSIGN(state.Forward[winding], pipelines.CreateGraphicsPipeline(Utils::MakeMeshPipelineSpecification(Utils::MakeForwardDescription(mirrored), mirrored, false, SceneColorFormat)));
		}
		ENGINE_TRY_ASSIGN(state.Skybox, SkyboxPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(state.Bloom, BloomPass::Create(device, pipelines, device.GetInfo().BloomFormat));
		ENGINE_TRY_ASSIGN(state.Tonemap, TonemapPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(state.Fxaa, FxaaPass::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(state.Debug, DebugRenderer::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(state.Text, TextRenderer::Create(device, pipelines));
		ENGINE_TRY_ASSIGN(state.DfgLut, BrdfLut::Create(device, pipelines));
		state.PipelineCount = static_cast<uint32_t>(2 * Utils::WindingCount) + state.Skybox->GetPipelineCount() + state.Bloom->GetPipelineCount()
			+ state.Tonemap->GetPipelineCount() + state.Fxaa->GetPipelineCount() + state.Debug->GetPipelineCount() + state.Text->GetPipelineCount()
			+ state.DfgLut->GetPipelineCount();
		ENGINE_CORE_INFO("Created the scene renderer's {} pipelines", state.PipelineCount);
		return set;
	}

	Status SceneRendererPipelines::EnsureDebugView(RenderDebugView /*view*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "debug views are not implemented yet (M8 stream A)");
	}

	uint32_t SceneRendererPipelines::GetPipelineCount() const
	{
		return m_State->PipelineCount;
	}

	std::vector<PipelineLayoutDescription> SceneRendererPipelines::GetLayoutDescriptions(nvrhi::Format bloomFormat)
	{
		std::vector<PipelineLayoutDescription> descriptions = {
			Utils::MakePrepassDescription(false),
			Utils::MakePrepassDescription(true),
			Utils::MakeForwardDescription(false),
			Utils::MakeForwardDescription(true),
		};
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
		ENGINE_TRY_ASSIGN(targets.LdrColor, Utils::CreateTarget(device, width, height, LdrColorFormat, Utils::TargetUsage::StorageImage, "LdrColor"));

		ENGINE_TRY_ASSIGN(targets.PrepassFramebuffer,
			device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(targets.SceneNormals).setDepthAttachment(targets.SceneDepth)));
		ENGINE_TRY_ASSIGN(targets.ForwardFramebuffer,
			device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(targets.SceneColor).setDepthAttachment(targets.SceneDepth)));
		return targets;
	}

	MaterialEntry* SceneRenderer::State::PrepareMaterial(nvrhi::ICommandList& commandList, AssetHandle handle)
	{
		const AssetRef<MaterialData> material = Assets->GetOrPlaceholder<MaterialData>(handle);
		const MaterialConstants constants{ .BaseColor = material->BaseColor };

		auto found = Materials.find(handle);
		if (found == Materials.end())
		{
			const auto create = [this, handle]() -> Result<MaterialEntry>
			{
				MaterialEntry entry;
				ENGINE_TRY_ASSIGN(entry.Constants,
					Utils::CreateConstantBuffer(*Device, sizeof(MaterialConstants), std::format("SceneRenderer.Material {}", handle)));
				for (size_t winding = 0; winding < Utils::WindingCount; ++winding)
				{
					nvrhi::BindingSetDesc desc;
					desc.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, entry.Constants) };
					ENGINE_TRY_ASSIGN(entry.BindingSets[winding], Device->CreateBindingSet(desc, *Pipelines->Forward[winding].BindingLayouts[1]));
				}
				return entry;
			};
			Result<MaterialEntry> created = create();
			if (!created.has_value())
			{
				// §8.14 item 7: an asset's GPU objects that cannot be created are a diagnostic, never a crash; its draws are
				// skipped this render and the creation is tried again on the next.
				AssetDiagnostic diagnostic;
				diagnostic.Severity = DiagnosticSeverity::Error;
				diagnostic.Code = std::string(AssetUploadFailedCode);
				diagnostic.Asset = handle;
				diagnostic.Path = Assets->GetReferencePath(handle);
				diagnostic.Message = std::format("cannot create the GPU constants of material {}: {}; its draws are skipped", handle,
					created.error().ToString());
				diagnostic.Hint = "the creation is tried again every frame; when the device is out of memory, use fewer materials or textures";
				Assets->ReportDiagnostic(std::move(diagnostic));
				return nullptr;
			}
			found = Materials.emplace(handle, std::move(*created)).first;
		}

		MaterialEntry& entry = found->second;
		entry.LastUsedRender = RenderCount;
		if (!entry.Written.has_value() || entry.Written->BaseColor != constants.BaseColor)
		{
			commandList.writeBuffer(entry.Constants, &constants, sizeof(constants));
			entry.Written = constants;
		}
		return &entry;
	}

	Status SceneRenderer::State::PrepareDraws(const RenderSnapshot& snapshot, const ViewConstants& view)
	{
		Draws.clear();
		Status status;
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

			// The mirror is valid until the next upload: everything needed is copied out now.
			const GpuMesh& mesh = Cache->GetMesh(item.Mesh);
			if (mesh.VertexBuffer == nullptr || mesh.IndexBuffer == nullptr)
				continue; // not even the placeholder could be created (reported by the cache)

			const glm::mat4 worldView = view.View * item.World;
			const glm::mat4 worldViewProjection = view.Projection * worldView;
			const size_t winding = glm::determinant(glm::mat3(item.World)) < 0.0f ? Utils::MirroredWinding : 0;
			for (const GpuSubmesh& submesh : mesh.Submeshes)
			{
				if (submesh.IndexCount == 0)
					continue;
				if (Utils::IsOutsideView(submesh.Bounds, worldViewProjection, worldView, snapshot.Camera.FarClip))
				{
					++Stats.CulledSubmeshes;
					continue;
				}
				SceneDraw draw;
				draw.VertexBuffer = mesh.VertexBuffer;
				draw.IndexBuffer = mesh.IndexBuffer;
				draw.IndexOffset = submesh.IndexOffset;
				draw.IndexCount = submesh.IndexCount;
				draw.Mesh = item.Mesh;
				draw.Material = Utils::SelectMaterial(item, mesh, submesh.MaterialSlot);
				draw.Winding = winding;
				draw.Constants.World = item.World;
				draw.Constants.EntityId = static_cast<uint32_t>(index + 1);
				Draws.push_back(std::move(draw));
			}
		}

		// §8.3 pass 1: opaque draws by pipeline, then material, then mesh; the snapshot's (canonical) order breaks ties.
		std::ranges::stable_sort(Draws, [](const SceneDraw& left, const SceneDraw& right)
		{
			return std::tie(left.Winding, left.Material, left.Mesh) < std::tie(right.Winding, right.Material, right.Mesh);
		});
		return status;
	}

	void SceneRenderer::State::RecordMeshPass(nvrhi::ICommandList& commandList, bool forward, const std::map<AssetHandle, MaterialEntry*>& materials)
	{
		nvrhi::IFramebuffer* framebuffer = forward ? Targets.ForwardFramebuffer.Get() : Targets.PrepassFramebuffer.Get();
		const nvrhi::Viewport viewport(static_cast<float>(Targets.Width), static_cast<float>(Targets.Height));

		// The state of the last draw: setGraphicsState only when it changes (it also invalidates the push constants, which
		// every draw sets).
		const nvrhi::IGraphicsPipeline* lastPipeline = nullptr;
		const nvrhi::IBindingSet* lastMaterial = nullptr;
		const nvrhi::IBuffer* lastVertexBuffer = nullptr;
		for (const SceneDraw& draw : Draws)
		{
			nvrhi::IBindingSet* material = nullptr;
			if (forward)
			{
				const auto found = materials.find(draw.Material);
				if (found == materials.end() || found->second == nullptr)
					continue; // its GPU constants could not be created (reported)
				material = found->second->BindingSets[draw.Winding];
			}
			else
			{
				// The prepass draws only what the forward pass draws, so the two passes agree on depth.
				const auto found = materials.find(draw.Material);
				if (found == materials.end() || found->second == nullptr)
					continue;
			}

			const GraphicsPipeline& pipeline = forward ? Pipelines->Forward[draw.Winding] : Pipelines->Prepass[draw.Winding];
			if (pipeline.Pipeline.Get() != lastPipeline || material != lastMaterial || draw.VertexBuffer.Get() != lastVertexBuffer)
			{
				nvrhi::GraphicsState state;
				state.pipeline = pipeline.Pipeline;
				state.framebuffer = framebuffer;
				state.viewport.addViewportAndScissorRect(viewport);
				state.bindings = { forward ? ForwardBindings[draw.Winding].Get() : PrepassBindings[draw.Winding].Get() };
				if (material != nullptr)
					state.bindings.push_back(material);
				state.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(draw.VertexBuffer).setSlot(0).setOffset(0));
				state.setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(draw.IndexBuffer).setFormat(nvrhi::Format::R32_UINT).setOffset(0));
				commandList.setGraphicsState(state);
				lastPipeline = pipeline.Pipeline.Get();
				lastMaterial = material;
				lastVertexBuffer = draw.VertexBuffer.Get();
			}
			commandList.setPushConstants(&draw.Constants, sizeof(draw.Constants));
			commandList.drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.IndexCount).setStartIndexLocation(draw.IndexOffset));
			if (forward)
				++Stats.MeshDraws;
		}
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
		state.Pipelines = pipelines.m_State.get();
		state.Cache = &cache;
		state.Assets = &assets;

		ENGINE_TRY_ASSIGN(state.ViewConstantsBuffer, Utils::CreateConstantBuffer(device, sizeof(ViewConstants), "SceneRenderer.ViewConstants"));
		ENGINE_TRY_ASSIGN(state.LightingConstantsBuffer,
			Utils::CreateConstantBuffer(device, sizeof(LightingConstants), "SceneRenderer.LightingConstants"));
		for (size_t winding = 0; winding < Utils::WindingCount; ++winding)
		{
			nvrhi::BindingSetDesc prepass;
			prepass.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, state.ViewConstantsBuffer),
				nvrhi::BindingSetItem::PushConstants(Utils::DrawConstantsSlot, sizeof(DrawConstants)),
			};
			ENGINE_TRY_ASSIGN(state.PrepassBindings[winding], device.CreateBindingSet(prepass, *state.Pipelines->Prepass[winding].BindingLayouts[0]));

			nvrhi::BindingSetDesc forward;
			forward.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, state.ViewConstantsBuffer),
				nvrhi::BindingSetItem::ConstantBuffer(2, state.LightingConstantsBuffer),
				nvrhi::BindingSetItem::PushConstants(Utils::DrawConstantsSlot, sizeof(DrawConstants)),
			};
			ENGINE_TRY_ASSIGN(state.ForwardBindings[winding], device.CreateBindingSet(forward, *state.Pipelines->Forward[winding].BindingLayouts[0]));
		}
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
		state.TonemapBindings.Clear();
		return {};
	}

	Status SceneRenderer::Render(nvrhi::ICommandList& commandList, const RenderSnapshot& snapshot)
	{
		State& state = *m_State;
		++state.RenderCount;
		state.Stats = {};
		const SceneTargets& targets = state.Targets;

		// Pass 1: Prepare. A camera whose matrices are not finite renders like no camera, and is the render's error.
		Status status;
		bool hasCamera = snapshot.HasCamera;
		if (hasCamera && (!Utils::IsFinite(snapshot.Camera.View) || !Utils::IsFinite(snapshot.Camera.Projection) || !Utils::IsFinite(snapshot.Camera.Position)))
		{
			status = MakeError(ErrorCode::InvalidArgument, "the camera of the render snapshot (entity {}) has a matrix that is not finite; the view is cleared",
				snapshot.Camera.Entity);
			hasCamera = false;
		}
		const float exposure = Utils::GetExposure(snapshot.Post);
		const CameraData camera = hasCamera ? snapshot.Camera : CameraData{};
		const ViewConstants view = Utils::MakeViewConstants(camera, targets.Width, targets.Height, exposure);
		uint32_t lightCount = 0;
		const LightingConstants lighting = Utils::MakeLightingConstants(snapshot, lightCount);

		state.Draws.clear();
		if (hasCamera)
		{
			Status prepared = state.PrepareDraws(snapshot, view);
			if (!prepared.has_value() && status.has_value())
				status = std::move(prepared);
		}

		commandList.beginMarker("SceneRenderer");
		commandList.writeBuffer(state.ViewConstantsBuffer, &view, sizeof(view));
		commandList.writeBuffer(state.LightingConstantsBuffer, &lighting, sizeof(lighting));
		std::map<AssetHandle, MaterialEntry*> materials;
		for (const SceneDraw& draw : state.Draws)
		{
			if (!materials.contains(draw.Material))
				materials.emplace(draw.Material, state.PrepareMaterial(commandList, draw.Material));
		}

		const glm::vec3 clearColor = camera.ClearColor;
		commandList.clearTextureFloat(targets.SceneColor, nvrhi::AllSubresources, nvrhi::Color(clearColor.r, clearColor.g, clearColor.b, 1.0f));
		if (hasCamera)
		{
			commandList.clearDepthStencilTexture(targets.SceneDepth, nvrhi::AllSubresources, true, 0.0f, false, 0);
			commandList.clearTextureFloat(targets.SceneNormals, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));

			// Pass 4: the depth/normal prepass.
			commandList.beginMarker("Prepass");
			state.RecordMeshPass(commandList, false, materials);
			commandList.endMarker();

			// Pass 7: forward opaque.
			commandList.beginMarker("ForwardOpaque");
			state.RecordMeshPass(commandList, true, materials);
			commandList.endMarker();
			state.Stats.Lights = lightCount;
		}

		// Pass 11: tonemap and encode (the walking skeleton's Linear tonemap until stream A records the M8 pass list).
		const TonemapPassInputs tonemap{
			.ViewConstants = state.ViewConstantsBuffer,
			.SceneColor = targets.SceneColor,
			.Bloom = nullptr,
			.BloomIntensity = 0.0f,
			.BlueNoise = nullptr,
			.LdrColor = targets.LdrColor,
			.Settings = { .Tonemapper = RenderTonemapper::Linear, .Dither = false, .EncodeSrgb = true },
		};
		Status tonemapped = state.Pipelines->Tonemap->Record(commandList, state.TonemapBindings, tonemap);
		if (!tonemapped.has_value() && status.has_value())
			status = std::move(tonemapped);
		commandList.endMarker();
		// The view keeps only the sets this render bound (the recorded command list holds its own references).
		state.TonemapBindings.ReleaseUnused();

		// Materials no draw used for a while are released (NVRHI keeps them until the submissions using them complete).
		std::erase_if(state.Materials, [&state](const auto& entry)
		{
			return state.RenderCount - entry.second.LastUsedRender > Utils::MaterialIdleRenders;
		});
		return status;
	}

	nvrhi::ITexture* SceneRenderer::GetFinalTexture() const
	{
		return m_State->Targets.LdrColor.Get();
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
