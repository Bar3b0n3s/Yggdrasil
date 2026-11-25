#include "EnginePCH.h"
#include "Engine/Renderer/WaterPass.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Core/Assert.h"
#include "Engine/Graphics/CommandListRecording.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/Private/ForwardPipelines.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Engine/Renderer/WaterGeometry.h"
#include "Shared/DrawConstants.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/FogConstants.h"
#include "Shared/LightClusterConstants.h"
#include "Shared/PlanarReflectionConstants.h"
#include "Shared/ProbeConstants.h"
#include "Shared/ShadowConstants.h"
#include "Shared/ViewConstants.h"
#include "Shared/WaterConstants.h"

#include <glm/gtc/type_precision.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <string_view>
#include <utility>

// The Water program (Passes/Water.slang) draws every clipmap level of every water with one of two pipelines (the surface, and
// the surface with EntityId); a level's draw binds the view's forward set variant (set 0), its water's set 1 and its level's
// mesh, and pushes the level's corner, cell size and record index. The pipelines cull nothing (SV_IsFrontFace tells above
// from below) and test and write SceneDepth with GreaterOrEqual (reverse-Z). The Underwater pipeline (W47.3) draws one
// triangle over the viewport into SceneColor alone, blended by its alpha, with the same set 0 and a set 1 of the records and
// SceneDepth.

namespace Engine {

	static_assert(sizeof(WaterDrawConstants) == sizeof(DrawConstants), "the water's push constants take the mesh pipelines' range");
	static_assert(MaxWaterRecordWaves == MaxWaterWaves, "a record holds every wave of a snapshot water");

	namespace Utils {

		constexpr std::string_view WaterProgram = "Water";
		// Set 1 of Passes/Water.slang: t0 the view's records, t1 WaterRefraction, t2 the depth pyramid, t3 the normal map, t4
		// the foam map.
		constexpr uint32_t WaterRecordsRegister = 0;
		constexpr uint32_t WaterRefractionRegister = 1;
		constexpr uint32_t WaterViewDepthRegister = 2;
		constexpr uint32_t WaterNormalMapRegister = 3;
		constexpr uint32_t WaterFoamMapRegister = 4;
		// The Underwater pipeline's set 1: t0 the records, t5 SceneDepth.
		constexpr uint32_t UnderwaterSceneDepthRegister = 5;

		[[nodiscard]] static nvrhi::BindingLayoutDesc MakeWaterLayout()
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::All;
			layout.registerSpace = 1;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(WaterRecordsRegister),
				nvrhi::BindingLayoutItem::Texture_SRV(WaterRefractionRegister),
				nvrhi::BindingLayoutItem::Texture_SRV(WaterViewDepthRegister),
				nvrhi::BindingLayoutItem::Texture_SRV(WaterNormalMapRegister),
				nvrhi::BindingLayoutItem::Texture_SRV(WaterFoamMapRegister),
			};
			return layout;
		}

		[[nodiscard]] static nvrhi::BindingLayoutDesc MakeUnderwaterLayout()
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::All;
			layout.registerSpace = 1;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(WaterRecordsRegister),
				nvrhi::BindingLayoutItem::Texture_SRV(UnderwaterSceneDepthRegister),
			};
			return layout;
		}

		// The surfaces' description (the surface, or the surface with EntityId), or with `underwater` the Underwater pass's.
		[[nodiscard]] static PipelineLayoutDescription MakeWaterDescription(bool picking, bool underwater = false)
		{
			return {
				.Name = underwater ? "WaterUnderwater" : (picking ? "WaterPicking" : "WaterSurface"),
				.Program = std::string(WaterProgram),
				.Entries = { underwater ? "VSUnderwater" : "VSSurface", underwater ? "PSUnderwater" : (picking ? "PSPicking" : "PSSurface") },
				.BindingLayouts = { GetForwardViewLayout(), underwater ? MakeUnderwaterLayout() : MakeWaterLayout() },
				.ConstantBuffers = {
					{ .Set = 0, .Register = ViewConstantsRegister, .ByteSize = sizeof(ViewConstants) },
					{ .Set = 0, .Register = EnvironmentConstantsRegister, .ByteSize = sizeof(EnvironmentConstants) },
					{ .Set = 0, .Register = ShadowConstantsRegister, .ByteSize = sizeof(ShadowConstants) },
					{ .Set = 0, .Register = LightClusterConstantsRegister, .ByteSize = sizeof(LightClusterConstants) },
					{ .Set = 0, .Register = ProbeConstantsRegister, .ByteSize = sizeof(ProbeConstants) },
					{ .Set = 0, .Register = FogConstantsRegister, .ByteSize = sizeof(FogConstants) },
					{ .Set = 0, .Register = PlanarReflectionConstantsRegister, .ByteSize = sizeof(PlanarReflectionConstants) },
				},
			};
		}

		// A surface pipeline: the cells (RG16_SINT) at vertex binding 0, nothing culled, counter-clockwise front faces (§8.3),
		// reverse-Z depth tested with GreaterOrEqual and written, into SceneColor, SceneNormals, SceneVelocity and SceneRoughness
		// (and EntityId when picking) with SceneDepth.
		[[nodiscard]] static GraphicsPipelineSpecification MakeWaterPipelineSpecification(bool picking, nvrhi::IBindingLayout& forwardViewLayout,
			nvrhi::IBindingLayout& waterLayout)
		{
			GraphicsPipelineSpecification specification;
			specification.Layout = MakeWaterDescription(picking);
			specification.SharedBindingLayouts = { nvrhi::BindingLayoutHandle(&forwardViewLayout), nvrhi::BindingLayoutHandle(&waterLayout) };
			specification.VertexAttributes = {
				nvrhi::VertexAttributeDesc().setName("CELL").setFormat(nvrhi::Format::RG16_SINT).setOffset(0).setElementStride(sizeof(glm::i16vec2)),
			};
			specification.RenderState.rasterState.setCullNone().setFrontCounterClockwise(true);
			specification.RenderState.depthStencilState.setDepthTestEnable(true)
				.setDepthWriteEnable(true)
				.setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual)
				.setStencilEnable(false);
			specification.Framebuffer.addColorFormat(SceneColorFormat)
				.addColorFormat(SceneNormalsFormat)
				.addColorFormat(SceneVelocityFormat)
				.addColorFormat(SceneRoughnessFormat);
			if (picking)
				specification.Framebuffer.addColorFormat(EntityIdFormat);
			specification.Framebuffer.setDepthFormat(SceneDepthFormat);
			return specification;
		}

		// The Underwater pipeline: one triangle over the viewport, nothing culled, no depth, into SceneColor alone, blended by
		// its alpha (SrcAlpha, InvSrcAlpha) with SceneColor's alpha kept.
		[[nodiscard]] static GraphicsPipelineSpecification MakeUnderwaterPipelineSpecification(nvrhi::IBindingLayout& forwardViewLayout,
			nvrhi::IBindingLayout& underwaterLayout)
		{
			GraphicsPipelineSpecification specification;
			specification.Layout = MakeWaterDescription(false, true);
			specification.SharedBindingLayouts = { nvrhi::BindingLayoutHandle(&forwardViewLayout), nvrhi::BindingLayoutHandle(&underwaterLayout) };
			specification.Primitive = nvrhi::PrimitiveType::TriangleList;
			specification.RenderState.rasterState.setCullNone();
			specification.RenderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false).setStencilEnable(false);
			nvrhi::BlendState::RenderTarget& blend = specification.RenderState.blendState.targets[0];
			blend.enableBlend()
				.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
				.setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
				.setBlendOp(nvrhi::BlendOp::Add)
				.setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
				.setDestBlendAlpha(nvrhi::BlendFactor::One)
				.setBlendOpAlpha(nvrhi::BlendOp::Add);
			specification.Framebuffer.addColorFormat(SceneColorFormat);
			return specification;
		}

		// The texture a water binds for `handle`: its mirror, or `fallback`'s (the built-in) for a null handle or one whose
		// mirror is a placeholder or missing.
		[[nodiscard]] static nvrhi::TextureHandle ResolveWaterTexture(GpuResourceCache& cache, AssetHandle handle, AssetHandle fallback)
		{
			if (handle.IsValid())
			{
				const GpuTexture& texture = cache.GetTexture(handle);
				if (!texture.IsPlaceholder && texture.Texture != nullptr)
					return texture.Texture;
			}
			return cache.GetTexture(fallback).Texture;
		}

	}

	namespace {

		// One mesh on the GPU: its cells (RG16_SINT) and its triangle list (R32_UINT).
		struct WaterMeshBuffers
		{
			nvrhi::BufferHandle Cells{};
			nvrhi::BufferHandle Indices{};
			uint32_t IndexCount = 0;
		};

	}

	struct WaterPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		nvrhi::BindingLayoutHandle WaterLayout{};
		nvrhi::BindingLayoutHandle UnderwaterLayout{};
		GraphicsPipeline Surface{};
		GraphicsPipeline Picking{};
		GraphicsPipeline Underwater{};
		std::array<WaterMeshBuffers, WaterMeshCount> Meshes{};
		uint32_t PipelineCount = 0;

		// Uploads mesh `mesh` into new buffers through one command list executed at once (a CommandListRecording, as it records
		// outside the application frame). Errors: those of the buffers' and the command list's creation (Gpu).
		[[nodiscard]] Result<WaterMeshBuffers> CreateMesh(uint32_t mesh) const;
	};

	WaterPass::WaterPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	WaterPass::~WaterPass() = default;

	Result<WaterMeshBuffers> WaterPass::State::CreateMesh(uint32_t mesh) const
	{
		const WaterGridMesh geometry = BuildWaterMesh(mesh);
		const std::string name = std::format("WaterPass.Mesh{}", mesh);
		nvrhi::BufferDesc cellsDesc;
		cellsDesc.byteSize = geometry.Cells.size() * sizeof(glm::i16vec2);
		cellsDesc.isVertexBuffer = true;
		cellsDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
		cellsDesc.keepInitialState = true;
		cellsDesc.debugName = name + ".Cells";
		ENGINE_TRY_ASSIGN(nvrhi::BufferHandle cells, Device->CreateBuffer(cellsDesc));
		nvrhi::BufferDesc indexDesc;
		indexDesc.byteSize = geometry.Indices.size() * sizeof(uint32_t);
		indexDesc.isIndexBuffer = true;
		indexDesc.initialState = nvrhi::ResourceStates::IndexBuffer;
		indexDesc.keepInitialState = true;
		indexDesc.debugName = name + ".Indices";
		ENGINE_TRY_ASSIGN(nvrhi::BufferHandle indices, Device->CreateBuffer(indexDesc));
		ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle commandList, Device->CreateCommandList());
		CommandListRecording recording(*Device, *commandList);
		commandList->writeBuffer(cells, geometry.Cells.data(), cellsDesc.byteSize);
		commandList->writeBuffer(indices, geometry.Indices.data(), indexDesc.byteSize);
		recording.Execute();
		return WaterMeshBuffers{ .Cells = std::move(cells), .Indices = std::move(indices), .IndexCount = static_cast<uint32_t>(geometry.Indices.size()) };
	}

	Result<Scope<WaterPass>> WaterPass::Create(GraphicsDevice& device, PipelineFactory& pipelines, nvrhi::IBindingLayout& forwardViewLayout)
	{
		Scope<WaterPass> pass = CreateScope<WaterPass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		ENGINE_TRY_ASSIGN(const std::vector<nvrhi::BindingLayoutHandle> layouts, pipelines.CreateBindingLayouts(Utils::MakeWaterDescription(false)));
		state.WaterLayout = layouts[1];
		ENGINE_TRY_ASSIGN(state.Surface, pipelines.CreateGraphicsPipeline(Utils::MakeWaterPipelineSpecification(false, forwardViewLayout, *state.WaterLayout)));
		ENGINE_TRY_ASSIGN(state.Picking, pipelines.CreateGraphicsPipeline(Utils::MakeWaterPipelineSpecification(true, forwardViewLayout, *state.WaterLayout)));
		ENGINE_TRY_ASSIGN(const std::vector<nvrhi::BindingLayoutHandle> underwaterLayouts, pipelines.CreateBindingLayouts(Utils::MakeWaterDescription(false, true)));
		state.UnderwaterLayout = underwaterLayouts[1];
		ENGINE_TRY_ASSIGN(state.Underwater, pipelines.CreateGraphicsPipeline(Utils::MakeUnderwaterPipelineSpecification(forwardViewLayout, *state.UnderwaterLayout)));
		for (uint32_t mesh = 0; mesh < WaterMeshCount; ++mesh)
		{
			ENGINE_TRY_ASSIGN(state.Meshes[mesh], state.CreateMesh(mesh));
		}
		state.PipelineCount = PipelineCount;
		return pass;
	}

	std::vector<PipelineLayoutDescription> WaterPass::GetLayoutDescriptions()
	{
		return { Utils::MakeWaterDescription(false), Utils::MakeWaterDescription(true), Utils::MakeWaterDescription(false, true) };
	}

	uint32_t WaterPass::GetPipelineCount() const
	{
		return m_State->PipelineCount;
	}

	Status WaterPass::RecordSurface(nvrhi::ICommandList& commandList, GpuResourceCache& cache, WaterViewResources& resources, const WaterSurfaceInputs& inputs,
		RenderPassCounters& counters)
	{
		ENGINE_CORE_ASSERT(inputs.Snapshot != nullptr && inputs.Camera != nullptr && inputs.Framebuffer != nullptr && inputs.ViewSet != nullptr
				&& inputs.Refraction != nullptr && inputs.ViewDepth != nullptr,
			"WaterPass::RecordSurface needs every input");
		if (inputs.Snapshot == nullptr || inputs.Camera == nullptr || inputs.Framebuffer == nullptr || inputs.ViewSet == nullptr || inputs.Refraction == nullptr
			|| inputs.ViewDepth == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "the water pass needs a snapshot, a camera, a framebuffer, a view set, the refraction and the depth pyramid");
		State& state = *m_State;
		const RenderSnapshot& snapshot = *inputs.Snapshot;
		if (snapshot.Waters.empty())
			return {};
		const auto count = static_cast<uint32_t>(snapshot.Waters.size());

		// The view's records, around the camera's levels.
		const glm::dvec2 camera(static_cast<double>(inputs.Camera->Position.x), static_cast<double>(inputs.Camera->Position.z));
		const std::array<WaterLevelPlacement, WaterLevelCount> levels = PlaceWaterLevels(camera);
		const glm::dvec2 origin = levels[0].Corner;
		// W47.3's planar opt-in: a water whose entity holds a drawn planar reflection slot samples it.
		std::vector<WaterRecord> records;
		records.reserve(count);
		for (const WaterItem& water : snapshot.Waters)
		{
			const auto slot = std::ranges::find(inputs.PlanarReflectionSlots, water.Entity);
			const uint32_t planarSlot = slot == inputs.PlanarReflectionSlots.end() || !water.Entity.IsValid()
				? WaterNoPlanarSlot
				: static_cast<uint32_t>(slot - inputs.PlanarReflectionSlots.begin());
			records.push_back(MakeWaterRecord(water, origin, camera, count, planarSlot));
		}
		if (resources.Records == nullptr || resources.RecordCapacity < count)
		{
			const uint32_t capacity = std::max(count, resources.RecordCapacity * 2);
			nvrhi::BufferDesc desc;
			desc.byteSize = static_cast<uint64_t>(capacity) * sizeof(WaterRecord);
			desc.structStride = sizeof(WaterRecord);
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = "SceneRenderer.WaterRecords";
			ENGINE_TRY_ASSIGN(nvrhi::BufferHandle buffer, state.Device->CreateBuffer(desc));
			resources.Bindings.Clear();
			resources.Records = std::move(buffer);
			resources.RecordCapacity = capacity;
		}

		// Each water's set 1, before anything is recorded, so a failed creation draws nothing.
		std::vector<nvrhi::IBindingSet*> sets;
		sets.reserve(count);
		for (const WaterItem& water : snapshot.Waters)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::StructuredBuffer_SRV(Utils::WaterRecordsRegister, resources.Records),
				nvrhi::BindingSetItem::Texture_SRV(Utils::WaterRefractionRegister, inputs.Refraction),
				nvrhi::BindingSetItem::Texture_SRV(Utils::WaterViewDepthRegister, inputs.ViewDepth),
				nvrhi::BindingSetItem::Texture_SRV(Utils::WaterNormalMapRegister,
					Utils::ResolveWaterTexture(cache, water.NormalMap, BuiltinAssetHandles::WaterNormalTexture)),
				nvrhi::BindingSetItem::Texture_SRV(Utils::WaterFoamMapRegister,
					Utils::ResolveWaterTexture(cache, water.FoamTexture, BuiltinAssetHandles::WaterFoamTexture)),
			};
			ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, resources.Bindings.GetOrCreate(*state.Device, desc, *state.WaterLayout));
			sets.push_back(set);
		}
		commandList.writeBuffer(resources.Records, records.data(), records.size() * sizeof(WaterRecord));

		// Each water's levels, then the skirt; a Rectangle draws only those that meet its footprint.
		const GraphicsPipeline& pipeline = inputs.Picking ? state.Picking : state.Surface;
		const nvrhi::Viewport viewport = inputs.Framebuffer->getFramebufferInfo().getViewport();
		for (uint32_t water = 0; water < count; ++water)
		{
			const WaterSquare footprint = GetWaterFootprintBounds(snapshot.Waters[water]);
			for (uint32_t level = 0; level <= WaterLevelCount; ++level)
			{
				const WaterSquare square = GetWaterLevelSquare(levels, level);
				if (footprint.Max.x < square.Min.x || footprint.Min.x > square.Max.x || footprint.Max.y < square.Min.y || footprint.Min.y > square.Max.y)
					continue;
				const bool skirt = level == WaterLevelCount;
				const WaterLevelPlacement& placement = levels[skirt ? WaterLevelCount - 1 : level];
				const WaterMeshBuffers& mesh = state.Meshes[skirt ? WaterSkirtMesh : GetWaterLevelMesh(placement, level)];
				nvrhi::GraphicsState graphics;
				graphics.pipeline = pipeline.Pipeline;
				graphics.framebuffer = inputs.Framebuffer;
				graphics.viewport.addViewportAndScissorRect(viewport);
				graphics.bindings = { inputs.ViewSet, sets[water] };
				graphics.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(mesh.Cells).setSlot(0).setOffset(0));
				graphics.setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(mesh.Indices).setFormat(nvrhi::Format::R32_UINT).setOffset(0));
				commandList.setGraphicsState(graphics);
				WaterDrawConstants constants{};
				constants.LevelOrigin = glm::vec2(placement.Corner - origin);
				constants.CellSize = static_cast<float>(placement.CellSize);
				constants.Water = water;
				commandList.setPushConstants(&constants, sizeof(constants));
				commandList.drawIndexed(nvrhi::DrawArguments().setVertexCount(mesh.IndexCount));
				++counters.DrawCalls;
				counters.Triangles += mesh.IndexCount / 3;
			}
		}
		return {};
	}

	Status WaterPass::RecordUnderwater(nvrhi::ICommandList& commandList, WaterViewResources& resources, const WaterUnderwaterInputs& inputs,
		RenderPassCounters& counters)
	{
		ENGINE_CORE_ASSERT(inputs.Framebuffer != nullptr && inputs.ViewSet != nullptr && inputs.SceneDepth != nullptr, "WaterPass::RecordUnderwater needs every input");
		if (inputs.Framebuffer == nullptr || inputs.ViewSet == nullptr || inputs.SceneDepth == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "the underwater pass needs a framebuffer, a view set and SceneDepth");
		if (resources.Records == nullptr)
			return MakeError(ErrorCode::InvalidState, "the underwater pass needs the records the water's surfaces wrote this render");
		State& state = *m_State;
		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::StructuredBuffer_SRV(Utils::WaterRecordsRegister, resources.Records),
			nvrhi::BindingSetItem::Texture_SRV(Utils::UnderwaterSceneDepthRegister, inputs.SceneDepth),
		};
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, resources.Bindings.GetOrCreate(*state.Device, desc, *state.UnderwaterLayout));
		nvrhi::GraphicsState graphics;
		graphics.pipeline = state.Underwater.Pipeline;
		graphics.framebuffer = inputs.Framebuffer;
		graphics.viewport.addViewportAndScissorRect(inputs.Framebuffer->getFramebufferInfo().getViewport());
		graphics.bindings = { inputs.ViewSet, set };
		commandList.setGraphicsState(graphics);
		// The push-constant range is set 0's layout's; the pass reads none of it.
		const WaterDrawConstants constants{};
		commandList.setPushConstants(&constants, sizeof(constants));
		commandList.draw(nvrhi::DrawArguments().setVertexCount(3));
		++counters.DrawCalls;
		++counters.Triangles;
		return {};
	}

}
