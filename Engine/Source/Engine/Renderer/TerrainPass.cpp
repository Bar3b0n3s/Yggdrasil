#include "EnginePCH.h"
#include "Engine/Renderer/TerrainPass.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/TerrainGeometry.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Core/Aabb.h"
#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/Private/ForwardPipelines.h"
#include "Engine/Renderer/RenderPrepare.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Engine/Renderer/TerrainLod.h"
#include "Shared/DrawConstants.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/FogConstants.h"
#include "Shared/LightClusterConstants.h"
#include "Shared/ProbeConstants.h"
#include "Shared/ShadowConstants.h"
#include "Shared/ViewConstants.h"

#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <utility>

// The Terrain program (Passes/Terrain.slang) draws every chunk of every terrain with one of three pipelines; a chunk's draw
// binds the view's set 0 (the prepass's own, or the mesh forward passes'), its terrain's set 1 and its key's chunk mesh, and
// pushes its first sample. The pipelines cull nothing (a mirroring World needs no variant of its own) and the forward one
// tests depth without writing it, as the mesh forward passes do.

namespace Engine {

	static_assert(sizeof(TerrainDrawConstants) == sizeof(DrawConstants), "the terrain's push constants take the mesh pipelines' range");

	namespace Utils {

		constexpr std::string_view TerrainProgram = "Terrain";
		// Set 1 of Passes/Terrain.slang: b0 TerrainConstants, t0 the heights, t1 the splat map, t2 to t5 the layers' base
		// colour maps, t6 to t9 their normal maps.
		constexpr uint32_t TerrainConstantsRegister = 0;
		constexpr uint32_t TerrainHeightsRegister = 0;
		constexpr uint32_t TerrainSplatMapRegister = 1;
		constexpr uint32_t TerrainBaseColorRegister = 2;
		constexpr uint32_t TerrainNormalRegister = TerrainBaseColorRegister + MaxTerrainShaderLayers;
		// The LodBias range of TerrainComponent.h, which a snapshot's value is clamped into.
		constexpr float MinTerrainLodBias = 0.25f;
		constexpr float MaxTerrainLodBias = 4.0f;

		[[nodiscard]] static nvrhi::BindingLayoutDesc MakeTerrainSetLayout(uint32_t set, std::vector<nvrhi::BindingLayoutItem> items)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::All;
			layout.registerSpace = set;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = std::move(items);
			return layout;
		}

		// The prepass's set 0: the view's constants, the DFG LUT, the two samplers and the push constants, at the registers of
		// the mesh forward passes' set 0.
		[[nodiscard]] static nvrhi::BindingLayoutDesc MakeTerrainPrepassViewLayout()
		{
			return MakeTerrainSetLayout(0, {
											   nvrhi::BindingLayoutItem::ConstantBuffer(ViewConstantsRegister),
											   nvrhi::BindingLayoutItem::Texture_SRV(BrdfLutRegister),
											   nvrhi::BindingLayoutItem::Sampler(LinearClampRegister),
											   nvrhi::BindingLayoutItem::Sampler(AnisoWrapRegister),
											   nvrhi::BindingLayoutItem::PushConstants(DrawConstantsSlot, sizeof(TerrainDrawConstants)),
										   });
		}

		[[nodiscard]] static nvrhi::BindingLayoutDesc MakeTerrainLayout()
		{
			std::vector<nvrhi::BindingLayoutItem> items = {
				nvrhi::BindingLayoutItem::ConstantBuffer(TerrainConstantsRegister),
				nvrhi::BindingLayoutItem::Texture_SRV(TerrainHeightsRegister),
				nvrhi::BindingLayoutItem::Texture_SRV(TerrainSplatMapRegister),
			};
			for (uint32_t layer = 0; layer < MaxTerrainShaderLayers; ++layer)
				items.push_back(nvrhi::BindingLayoutItem::Texture_SRV(TerrainBaseColorRegister + layer));
			for (uint32_t layer = 0; layer < MaxTerrainShaderLayers; ++layer)
				items.push_back(nvrhi::BindingLayoutItem::Texture_SRV(TerrainNormalRegister + layer));
			return MakeTerrainSetLayout(1, std::move(items));
		}

		[[nodiscard]] static PipelineLayoutDescription MakeTerrainPrepassDescription(bool picking)
		{
			return {
				.Name = picking ? "TerrainPicking" : "TerrainPrepass",
				.Program = std::string(TerrainProgram),
				.Entries = { "VSPrepass", picking ? "PSPicking" : "PSPrepass" },
				.BindingLayouts = { MakeTerrainPrepassViewLayout(), MakeTerrainLayout() },
				.ConstantBuffers = {
					{ .Set = 0, .Register = ViewConstantsRegister, .ByteSize = sizeof(ViewConstants) },
					{ .Set = 1, .Register = TerrainConstantsRegister, .ByteSize = sizeof(TerrainConstants) },
				},
			};
		}

		[[nodiscard]] static PipelineLayoutDescription MakeTerrainForwardDescription()
		{
			return {
				.Name = "TerrainForward",
				.Program = std::string(TerrainProgram),
				.Entries = { "VSForward", "PSForward" },
				.BindingLayouts = { GetForwardViewLayout(), MakeTerrainLayout() },
				.ConstantBuffers = {
					{ .Set = 0, .Register = ViewConstantsRegister, .ByteSize = sizeof(ViewConstants) },
					{ .Set = 0, .Register = EnvironmentConstantsRegister, .ByteSize = sizeof(EnvironmentConstants) },
					{ .Set = 0, .Register = ShadowConstantsRegister, .ByteSize = sizeof(ShadowConstants) },
					{ .Set = 0, .Register = LightClusterConstantsRegister, .ByteSize = sizeof(LightClusterConstants) },
					{ .Set = 0, .Register = ProbeConstantsRegister, .ByteSize = sizeof(ProbeConstants) },
					{ .Set = 0, .Register = FogConstantsRegister, .ByteSize = sizeof(FogConstants) },
					{ .Set = 1, .Register = TerrainConstantsRegister, .ByteSize = sizeof(TerrainConstants) },
				},
			};
		}

		// A terrain pipeline: the chunk-local samples (RG16_UINT) at vertex binding 0, nothing culled, counter-clockwise front
		// faces (§8.3), reverse-Z depth tested with GreaterOrEqual and written by the prepasses only.
		[[nodiscard]] static GraphicsPipelineSpecification MakeTerrainPipelineSpecification(PipelineLayoutDescription layout,
			std::vector<nvrhi::BindingLayoutHandle> sharedLayouts, bool prepass)
		{
			GraphicsPipelineSpecification specification;
			specification.Layout = std::move(layout);
			specification.SharedBindingLayouts = std::move(sharedLayouts);
			specification.VertexAttributes = {
				nvrhi::VertexAttributeDesc().setName("SAMPLE").setFormat(nvrhi::Format::RG16_UINT).setOffset(0).setElementStride(2 * sizeof(uint16_t)),
			};
			specification.RenderState.rasterState.setCullNone().setFrontCounterClockwise(true);
			specification.RenderState.depthStencilState.setDepthTestEnable(true)
				.setDepthWriteEnable(prepass)
				.setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual)
				.setStencilEnable(false);
			return specification;
		}

		// Whether a snapshot terrain can be drawn: a finite World, and a Size and HeightScale positive and finite.
		[[nodiscard]] static bool IsDrawable(const TerrainItem& terrain)
		{
			for (glm::length_t column = 0; column < 4; ++column)
			{
				for (glm::length_t row = 0; row < 4; ++row)
				{
					if (!std::isfinite(terrain.World[column][row]))
						return false;
				}
			}
			const auto positive = [](float value)
			{
				return std::isfinite(value) && value > 0.0f;
			};
			return positive(terrain.Size.x) && positive(terrain.Size.y) && positive(terrain.HeightScale);
		}

		// A chunk-local mesh's samples as RG16_UINT pairs: BuildTerrainChunkMesh's, less the chunk's first sample.
		[[nodiscard]] static std::vector<uint16_t> MakeChunkVertices(const TerrainChunkMesh& mesh, const glm::uvec2& origin)
		{
			std::vector<uint16_t> vertices;
			vertices.reserve(mesh.Samples.size() * 2);
			for (const glm::uvec2& sample : mesh.Samples)
			{
				vertices.push_back(static_cast<uint16_t>(sample.x - origin.x));
				vertices.push_back(static_cast<uint16_t>(sample.y - origin.y));
			}
			return vertices;
		}

	}

	TerrainConstants MakeTerrainConstants(const TerrainItem& terrain, uint32_t resolution, RenderDebugView debugView, bool splatMap, uint32_t twoChannel)
	{
		TerrainConstants constants{};
		constants.World = terrain.World;
		const TerrainLayerItem whiteLayer{};
		const size_t layerCount = std::min<size_t>(terrain.Layers.size(), MaxTerrainShaderLayers);
		for (uint32_t index = 0; index < MaxTerrainShaderLayers; ++index)
		{
			if (index >= std::max<size_t>(layerCount, 1))
			{
				constants.LayerTints[index] = glm::vec4(0.0f);
				constants.LayerSurfaces[index] = glm::vec4(0.0f);
				continue;
			}
			const TerrainLayerItem& layer = layerCount == 0 ? whiteLayer : terrain.Layers[index];
			// The values the registry's ranges keep, for a snapshot built by hand: a Tiling that is not positive and finite takes
			// the default, and Roughness and Metallic are clamped to [0, 1] (the defaults when not finite).
			const float tiling = std::isfinite(layer.Tiling) && layer.Tiling > 0.0f ? layer.Tiling : whiteLayer.Tiling;
			const float roughness = std::isfinite(layer.Roughness) ? std::clamp(layer.Roughness, 0.0f, 1.0f) : whiteLayer.Roughness;
			const float metallic = std::isfinite(layer.Metallic) ? std::clamp(layer.Metallic, 0.0f, 1.0f) : whiteLayer.Metallic;
			constants.LayerTints[index] = glm::vec4(layer.Tint, 1.0f / tiling);
			constants.LayerSurfaces[index] = glm::vec4(roughness, metallic, (twoChannel & (1U << index)) != 0 ? 1.0f : 0.0f, 0.0f);
		}
		const auto cells = static_cast<float>(resolution > 1 ? resolution - 1 : 1);
		constants.CellSize = terrain.Size / cells;
		constants.HeightScale = terrain.HeightScale;
		constants.Resolution = resolution;
		constants.InverseSize = 1.0f / terrain.Size;
		constants.LayerCount = static_cast<uint32_t>(std::max<size_t>(layerCount, 1));
		constants.Flags = (terrain.ReceiveShadows ? TerrainFlagReceiveShadows : 0U) | (splatMap ? TerrainFlagSplatMap : 0U)
			| (glm::determinant(glm::mat3(terrain.World)) < 0.0f ? TerrainFlagMirrored : 0U);
		constants.PickId = terrain.PickId;
		constants.DebugView = static_cast<uint32_t>(debugView);
		return constants;
	}

	struct TerrainPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		nvrhi::BindingLayoutHandle PrepassViewLayout{};
		nvrhi::BindingLayoutHandle TerrainLayout{};
		GraphicsPipeline Prepass{};
		GraphicsPipeline Picking{};
		GraphicsPipeline Forward{};
		uint32_t PipelineCount = 0;

		// The chunk mesh of `key` in `resources`, made from chunk (chunkX, chunkZ) of `layout` and uploaded through
		// `commandList` the first time; marked used. Errors: those of the buffers' creation (Gpu).
		[[nodiscard]] Status EnsureChunkMesh(nvrhi::ICommandList& commandList, TerrainViewResources& resources, const TerrainChunkMeshKey& key,
			const TerrainLodLayout& layout, uint32_t chunkX, uint32_t chunkZ, const TerrainNeighbourLods& neighbours) const;
		// Records `resources`' chunks with `pipeline` and the view's set 0.
		void RecordChunks(nvrhi::ICommandList& commandList, const TerrainViewResources& resources, const GraphicsPipeline& pipeline,
			nvrhi::IFramebuffer& framebuffer, nvrhi::IBindingSet& viewSet, RenderPassCounters& counters) const;
	};

	TerrainPass::TerrainPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	TerrainPass::~TerrainPass() = default;

	Result<Scope<TerrainPass>> TerrainPass::Create(GraphicsDevice& device, PipelineFactory& pipelines, nvrhi::IBindingLayout& forwardViewLayout)
	{
		Scope<TerrainPass> pass = CreateScope<TerrainPass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		ENGINE_TRY_ASSIGN(const std::vector<nvrhi::BindingLayoutHandle> layouts, pipelines.CreateBindingLayouts(Utils::MakeTerrainPrepassDescription(false)));
		state.PrepassViewLayout = layouts[0];
		state.TerrainLayout = layouts[1];

		GraphicsPipelineSpecification prepass = Utils::MakeTerrainPipelineSpecification(Utils::MakeTerrainPrepassDescription(false),
			{ state.PrepassViewLayout, state.TerrainLayout }, true);
		// SceneTargetFormats.h's attachment order: SceneNormals, SceneVelocity, SceneRoughness; picking adds EntityId second.
		prepass.Framebuffer.addColorFormat(SceneNormalsFormat).addColorFormat(SceneVelocityFormat).addColorFormat(SceneRoughnessFormat).setDepthFormat(SceneDepthFormat);
		ENGINE_TRY_ASSIGN(state.Prepass, pipelines.CreateGraphicsPipeline(prepass));

		GraphicsPipelineSpecification picking = Utils::MakeTerrainPipelineSpecification(Utils::MakeTerrainPrepassDescription(true),
			{ state.PrepassViewLayout, state.TerrainLayout }, true);
		picking.Framebuffer.addColorFormat(SceneNormalsFormat)
			.addColorFormat(EntityIdFormat)
			.addColorFormat(SceneVelocityFormat)
			.addColorFormat(SceneRoughnessFormat)
			.setDepthFormat(SceneDepthFormat);
		ENGINE_TRY_ASSIGN(state.Picking, pipelines.CreateGraphicsPipeline(picking));

		GraphicsPipelineSpecification forward = Utils::MakeTerrainPipelineSpecification(Utils::MakeTerrainForwardDescription(),
			{ nvrhi::BindingLayoutHandle(&forwardViewLayout), state.TerrainLayout }, false);
		forward.Framebuffer = GetSceneColorFramebufferInfo();
		ENGINE_TRY_ASSIGN(state.Forward, pipelines.CreateGraphicsPipeline(forward));
		state.PipelineCount = PipelineCount;
		return pass;
	}

	std::vector<PipelineLayoutDescription> TerrainPass::GetLayoutDescriptions()
	{
		return { Utils::MakeTerrainPrepassDescription(false), Utils::MakeTerrainPrepassDescription(true), Utils::MakeTerrainForwardDescription() };
	}

	uint32_t TerrainPass::GetPipelineCount() const
	{
		return m_State->PipelineCount;
	}

	nvrhi::IBindingLayout& TerrainPass::GetPrepassViewLayout() const
	{
		return *m_State->PrepassViewLayout;
	}

	Status TerrainPass::State::EnsureChunkMesh(nvrhi::ICommandList& commandList, TerrainViewResources& resources, const TerrainChunkMeshKey& key,
		const TerrainLodLayout& layout, uint32_t chunkX, uint32_t chunkZ, const TerrainNeighbourLods& neighbours) const
	{
		if (const auto found = resources.Meshes.find(key); found != resources.Meshes.end())
		{
			found->second.IsUsed = true;
			return {};
		}
		const TerrainChunkMesh mesh = BuildTerrainChunkMesh(layout, chunkX, chunkZ, key.Lod, neighbours);
		const std::vector<uint16_t> vertices = Utils::MakeChunkVertices(mesh, glm::uvec2(chunkX, chunkZ) * TerrainChunkCells);
		const std::string name = std::format("SceneRenderer.TerrainChunk{}x{}.Lod{}.Stitch{}", key.CellsX, key.CellsZ, key.Lod, key.Stitch);
		nvrhi::BufferDesc vertexDesc;
		vertexDesc.byteSize = vertices.size() * sizeof(uint16_t);
		vertexDesc.isVertexBuffer = true;
		vertexDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
		vertexDesc.keepInitialState = true;
		vertexDesc.debugName = name + ".Vertices";
		ENGINE_TRY_ASSIGN(nvrhi::BufferHandle vertexBuffer, Device->CreateBuffer(vertexDesc));
		nvrhi::BufferDesc indexDesc;
		indexDesc.byteSize = mesh.Indices.size() * sizeof(uint32_t);
		indexDesc.isIndexBuffer = true;
		indexDesc.initialState = nvrhi::ResourceStates::IndexBuffer;
		indexDesc.keepInitialState = true;
		indexDesc.debugName = name + ".Indices";
		ENGINE_TRY_ASSIGN(nvrhi::BufferHandle indexBuffer, Device->CreateBuffer(indexDesc));
		commandList.writeBuffer(vertexBuffer, vertices.data(), vertexDesc.byteSize);
		commandList.writeBuffer(indexBuffer, mesh.Indices.data(), indexDesc.byteSize);
		resources.Meshes.emplace(key, TerrainChunkMeshBuffers{ .Vertices = std::move(vertexBuffer), .Indices = std::move(indexBuffer), .IndexCount = static_cast<uint32_t>(mesh.Indices.size()), .IsUsed = true });
		return {};
	}

	Status TerrainPass::Prepare(nvrhi::ICommandList& commandList, GpuResourceCache& cache, TerrainViewResources& resources, const TerrainPassInputs& inputs)
	{
		ENGINE_CORE_ASSERT(inputs.Snapshot != nullptr && inputs.Camera != nullptr, "TerrainPass::Prepare needs the snapshot and the camera");
		State& state = *m_State;
		resources.Draws.clear();
		if (inputs.Snapshot == nullptr || inputs.Camera == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "the terrain pass needs a snapshot and a camera");
		const RenderSnapshot& snapshot = *inputs.Snapshot;
		const CameraData& camera = *inputs.Camera;
		Status status;
		const auto keepFirst = [&status](Status result)
		{
			if (!result && status)
				status = std::move(result);
		};

		for (size_t index = 0; index < snapshot.Terrains.size(); ++index)
		{
			const TerrainItem& terrain = snapshot.Terrains[index];
			if (!terrain.Heightfield.IsValid())
				continue; // nothing to draw (extraction lists none)
			if (!Utils::IsDrawable(terrain))
			{
				keepFirst(MakeError(ErrorCode::InvalidArgument,
					"the terrain of entity {} in the render snapshot has a world matrix that is not finite, or a Size or HeightScale that is not positive; "
					"it is not drawn",
					terrain.Entity));
				continue;
			}
			// The mirrors are valid until the next upload: their textures are taken at once.
			const GpuHeightfield* heightfield = cache.GetHeightfield(terrain.Heightfield);
			if (heightfield == nullptr || heightfield->Heights == nullptr)
				continue; // missing, of another type or failed (reported by the cache): no terrain
			const nvrhi::TextureHandle heights = heightfield->Heights;
			const uint32_t resolution = heightfield->Resolution;
			const bool splatMap = terrain.SplatMap.IsValid();
			const nvrhi::TextureHandle splat = cache.GetTexture(splatMap ? terrain.SplatMap : BuiltinAssetHandles::WhiteTexture).Texture;
			std::array<nvrhi::TextureHandle, MaxTerrainShaderLayers> baseColors{};
			std::array<nvrhi::TextureHandle, MaxTerrainShaderLayers> normals{};
			uint32_t twoChannel = 0;
			for (uint32_t layer = 0; layer < MaxTerrainShaderLayers; ++layer)
			{
				const bool present = layer < terrain.Layers.size();
				const AssetHandle baseColor = present && terrain.Layers[layer].BaseColorMap.IsValid() ? terrain.Layers[layer].BaseColorMap : BuiltinAssetHandles::WhiteTexture;
				const AssetHandle normal = present && terrain.Layers[layer].NormalMap.IsValid() ? terrain.Layers[layer].NormalMap : BuiltinAssetHandles::FlatNormalTexture;
				baseColors[layer] = cache.GetTexture(baseColor).Texture;
				const GpuTexture& normalMirror = cache.GetTexture(normal);
				normals[layer] = normalMirror.Texture;
				if (normalMirror.Format == TextureFormat::BC5Unorm)
					twoChannel |= 1U << layer;
			}
			const bool texturesExist = splat != nullptr && std::ranges::all_of(baseColors, [](const nvrhi::TextureHandle& texture)
			{
				return texture != nullptr;
			}) && std::ranges::all_of(normals, [](const nvrhi::TextureHandle& texture)
			{
				return texture != nullptr;
			});
			if (!texturesExist)
				continue; // not even a placeholder could be created (reported by the cache)

			// The terrain's constants, in a buffer of the view's that this render writes once.
			const size_t slot = resources.Draws.size();
			if (resources.Constants.size() <= slot)
			{
				nvrhi::BufferDesc desc;
				desc.byteSize = sizeof(TerrainConstants);
				desc.isConstantBuffer = true;
				desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
				desc.keepInitialState = true;
				desc.debugName = std::format("SceneRenderer.TerrainConstants{}", slot);
				Result<nvrhi::BufferHandle> created = state.Device->CreateBuffer(desc);
				if (!created.has_value())
				{
					keepFirst(std::unexpected(std::move(created).error()));
					continue;
				}
				resources.Constants.push_back(std::move(*created));
			}
			const nvrhi::BufferHandle& constantsBuffer = resources.Constants[slot];
			const TerrainConstants constants = MakeTerrainConstants(terrain, resolution, inputs.DebugView, splatMap, twoChannel);
			commandList.writeBuffer(constantsBuffer, &constants, sizeof(constants));

			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(Utils::TerrainConstantsRegister, constantsBuffer),
				nvrhi::BindingSetItem::Texture_SRV(Utils::TerrainHeightsRegister, heights),
				nvrhi::BindingSetItem::Texture_SRV(Utils::TerrainSplatMapRegister, splat),
			};
			for (uint32_t layer = 0; layer < MaxTerrainShaderLayers; ++layer)
				desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(Utils::TerrainBaseColorRegister + layer, baseColors[layer]));
			for (uint32_t layer = 0; layer < MaxTerrainShaderLayers; ++layer)
				desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(Utils::TerrainNormalRegister + layer, normals[layer]));
			Result<nvrhi::IBindingSet*> set = resources.Bindings.GetOrCreate(*state.Device, desc, *state.TerrainLayout);
			if (!set.has_value())
			{
				keepFirst(std::unexpected(std::move(set).error()));
				continue;
			}

			// The chunks' levels at the camera's position in the terrain's local space, and the chunks that meet the view.
			const TerrainLodLayout layout = ComputeTerrainLodLayout(resolution);
			const TerrainExtent extent{ .Size = terrain.Size, .HeightScale = terrain.HeightScale };
			const glm::vec3 cameraLocal = glm::vec3(glm::inverse(terrain.World) * glm::vec4(camera.Position, 1.0f));
			const float lodBias = std::isfinite(terrain.LodBias) ? std::clamp(terrain.LodBias, Utils::MinTerrainLodBias, Utils::MaxTerrainLodBias) : 1.0f;
			const std::vector<uint32_t> lods = SelectTerrainLods(layout, extent, cameraLocal, lodBias);
			const uint32_t chunks = layout.ChunksPerSide;
			const uint32_t cells = resolution - 1;
			const glm::vec2 cellSize = terrain.Size / static_cast<float>(cells);
			TerrainDraw draw{ .SnapshotIndex = index, .Set = *set };
			for (uint32_t chunkZ = 0; chunkZ < chunks; ++chunkZ)
			{
				for (uint32_t chunkX = 0; chunkX < chunks; ++chunkX)
				{
					const glm::uvec2 first = glm::uvec2(chunkX, chunkZ) * TerrainChunkCells;
					const glm::uvec2 last = glm::min(first + TerrainChunkCells, glm::uvec2(cells));
					const Aabb bounds{ .Min = glm::vec3(static_cast<float>(first.x) * cellSize.x, 0.0f, static_cast<float>(first.y) * cellSize.y),
						.Max = glm::vec3(static_cast<float>(last.x) * cellSize.x, terrain.HeightScale, static_cast<float>(last.y) * cellSize.y) };
					if (IsOutsideView(bounds, terrain.World, camera))
						continue;
					const uint32_t lod = lods[static_cast<size_t>(chunkZ) * chunks + chunkX];
					const auto neighbourLod = [&lods, chunks, lod](bool exists, uint32_t x, uint32_t z)
					{
						return exists ? lods[static_cast<size_t>(z) * chunks + x] : lod;
					};
					const TerrainNeighbourLods neighbours = {
						neighbourLod(chunkX > 0, chunkX - 1, chunkZ),
						neighbourLod(chunkX + 1 < chunks, chunkX + 1, chunkZ),
						neighbourLod(chunkZ > 0, chunkX, chunkZ - 1),
						neighbourLod(chunkZ + 1 < chunks, chunkX, chunkZ + 1),
					};
					uint32_t stitch = 0;
					for (uint32_t side = 0; side < neighbours.size(); ++side)
						stitch |= neighbours[side] > lod ? 1U << side : 0U;
					const TerrainChunkMeshKey key{ .CellsX = last.x - first.x, .CellsZ = last.y - first.y, .Lod = lod, .Stitch = stitch };
					const Status ensured = state.EnsureChunkMesh(commandList, resources, key, layout, chunkX, chunkZ, neighbours);
					if (!ensured.has_value())
					{
						keepFirst(ensured);
						continue;
					}
					draw.Chunks.push_back({ .Mesh = key, .Origin = first });
				}
			}
			// Each key's chunks together (fewer state changes), row-major within a key.
			std::ranges::stable_sort(draw.Chunks, [](const TerrainChunkDraw& left, const TerrainChunkDraw& right)
			{
				return left.Mesh < right.Mesh;
			});
			resources.Draws.push_back(std::move(draw));
		}
		return status;
	}

	void TerrainPass::State::RecordChunks(nvrhi::ICommandList& commandList, const TerrainViewResources& resources, const GraphicsPipeline& pipeline,
		nvrhi::IFramebuffer& framebuffer, nvrhi::IBindingSet& viewSet, RenderPassCounters& counters) const
	{
		const nvrhi::Viewport viewport = framebuffer.getFramebufferInfo().getViewport();
		for (const TerrainDraw& draw : resources.Draws)
		{
			const TerrainChunkMeshBuffers* bound = nullptr;
			for (const TerrainChunkDraw& chunk : draw.Chunks)
			{
				const auto found = resources.Meshes.find(chunk.Mesh);
				ENGINE_CORE_ASSERT(found != resources.Meshes.end(), "a prepared chunk's mesh exists until the render ends");
				if (found == resources.Meshes.end())
					continue;
				const TerrainChunkMeshBuffers& mesh = found->second;
				if (&mesh != bound)
				{
					// setGraphicsState also invalidates the push constants, which every draw sets.
					nvrhi::GraphicsState graphics;
					graphics.pipeline = pipeline.Pipeline;
					graphics.framebuffer = &framebuffer;
					graphics.viewport.addViewportAndScissorRect(viewport);
					graphics.bindings = { &viewSet, draw.Set };
					graphics.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(mesh.Vertices).setSlot(0).setOffset(0));
					graphics.setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(mesh.Indices).setFormat(nvrhi::Format::R32_UINT).setOffset(0));
					commandList.setGraphicsState(graphics);
					bound = &mesh;
				}
				TerrainDrawConstants constants{};
				constants.ChunkOrigin = chunk.Origin;
				commandList.setPushConstants(&constants, sizeof(constants));
				commandList.drawIndexed(nvrhi::DrawArguments().setVertexCount(mesh.IndexCount));
				++counters.DrawCalls;
				counters.Triangles += mesh.IndexCount / 3;
			}
		}
	}

	void TerrainPass::RecordDepthNormal(nvrhi::ICommandList& commandList, const TerrainViewResources& resources, nvrhi::IFramebuffer& framebuffer, bool picking,
		nvrhi::IBindingSet& viewSet, RenderPassCounters& counters) const
	{
		const State& state = *m_State;
		state.RecordChunks(commandList, resources, picking ? state.Picking : state.Prepass, framebuffer, viewSet, counters);
	}

	void TerrainPass::RecordForward(nvrhi::ICommandList& commandList, const TerrainViewResources& resources, nvrhi::IFramebuffer& framebuffer,
		nvrhi::IBindingSet& forwardSet, RenderPassCounters& counters) const
	{
		const State& state = *m_State;
		state.RecordChunks(commandList, resources, state.Forward, framebuffer, forwardSet, counters);
	}

	void TerrainPass::ReleaseUnused(TerrainViewResources& resources)
	{
		std::erase_if(resources.Meshes, [](const auto& item)
		{
			return !item.second.IsUsed;
		});
		for (auto& [key, mesh] : resources.Meshes)
			mesh.IsUsed = false;
		resources.Bindings.ReleaseUnused();
		// The constant buffers of the terrains this render drew stay for the next one; the others go.
		if (resources.Constants.size() > resources.Draws.size())
			resources.Constants.resize(resources.Draws.size());
		resources.Draws.clear();
	}

}
