#include "EnginePCH.h"
#include "Engine/Renderer/SelectionPass.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Graphics/CommandListRecording.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/MeshLod.h"
#include "Engine/Renderer/MeshWind.h"
#include "Engine/Renderer/Private/ForwardPipelines.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/DrawConstants.h"
#include "Shared/MaterialConstants.h"
#include "Shared/Private/SelectionPassConstants.h"
#include "Shared/WindConstants.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Engine {

	namespace {

		struct SelectionRecordingScope
		{
			// Stack-only borrowed context; never retained past Record.
			RenderRecordingContext& Recording;
			RenderPassCounters Counters{};
			SelectionRecordingScope(RenderRecordingContext& recording, nvrhi::ICommandList& list, std::string_view name)
				: Recording(recording)
			{
				Recording.BeginPass(name, &list);
			}
			~SelectionRecordingScope() { Recording.EndPass(Counters); }
			SelectionRecordingScope(const SelectionRecordingScope&) = delete;
			SelectionRecordingScope& operator=(const SelectionRecordingScope&) = delete;
		};

	}

	namespace Utils {

		static bool SelectionFiniteMatrix(const glm::mat4& matrix)
		{
			for (int column = 0; column < 4; ++column)
				for (int row = 0; row < 4; ++row)
					if (!std::isfinite(matrix[column][row]))
						return false;
			return true;
		}

		static PipelineLayoutDescription SelectionMaskLayout(uint32_t variant)
		{
			const bool masked = variant >= 3;
			nvrhi::BindingLayoutDesc view;
			view.visibility = nvrhi::ShaderType::AllGraphics;
			view.registerSpaceIsDescriptorSet = true;
			// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3G): the wind at b8.
			view.bindings = {
				nvrhi::BindingLayoutItem::PushConstants(0, sizeof(SelectionMaskConstants)),
				nvrhi::BindingLayoutItem::Texture_SRV(0),
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(InstancesRegister),
				nvrhi::BindingLayoutItem::ConstantBuffer(WindConstantsRegister)
			};
			PipelineLayoutDescription description{
				.Name = std::format("SelectionMask{}", variant),
				.Program = "SelectionMask",
				.Entries = { "VSMain", "PSMask" },
				.Permutation = { { "ALPHA_MASK", masked ? "1" : "0" } },
				.BindingLayouts = { view },
				.ConstantBuffers = { { .Set = 0, .Register = WindConstantsRegister, .ByteSize = sizeof(WindConstants) } }
			};
			if (masked)
			{
				nvrhi::BindingLayoutDesc material;
				material.visibility = nvrhi::ShaderType::Pixel;
				material.registerSpace = 1;
				material.registerSpaceIsDescriptorSet = true;
				material.bindings = { nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(0) };
				description.BindingLayouts.push_back(material);
				description.ConstantBuffers.push_back({ .Set = 1, .Register = 0, .ByteSize = sizeof(MaterialConstants) });
			}
			return description;
		}

		static PipelineLayoutDescription SelectionFilterLayout(bool composite)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {
				nvrhi::BindingLayoutItem::PushConstants(0, sizeof(SelectionFilterConstants)),
				nvrhi::BindingLayoutItem::Texture_SRV(0)
			};
			if (composite)
			{
				layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(1));
				layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(1));
			}
			else
				layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(0));
			return {
				.Name = composite ? "SelectionComposite" : "SelectionDilate",
				.Program = composite ? "SelectionComposite" : "SelectionDilate",
				.Entries = { composite ? "CSComposite" : "CSDilate" },
				.BindingLayouts = { layout },
				.StorageImages = { { .Set = 0, .Register = composite ? 1u : 0u, .Format = composite ? LdrColorFormat : SelectionMaskFormat } }
			};
		}

		static Status SelectionValidateInputs(const SelectionRenderInputs& inputs)
		{
			if (!inputs.Snapshot || !inputs.SceneDepth || !inputs.Mask || !inputs.Scratch || !inputs.LdrColor
				|| inputs.Radius < 1 || inputs.Radius > 8 || inputs.Mask == inputs.Scratch)
				return MakeError(ErrorCode::InvalidArgument, "Selection needs distinct valid targets, a snapshot and a radius in [1,8]");
			const auto& mask = inputs.Mask->getDesc();
			const auto valid = [&mask](const nvrhi::TextureDesc& desc, nvrhi::Format format)
			{
				return desc.format == format && desc.dimension == nvrhi::TextureDimension::Texture2D && desc.sampleCount == 1
					&& desc.arraySize == 1 && desc.width == mask.width && desc.height == mask.height && desc.isShaderResource;
			};
			if (mask.width == 0 || mask.height == 0 || !valid(mask, SelectionMaskFormat) || !mask.isUAV || !mask.isRenderTarget
				|| !valid(inputs.Scratch->getDesc(), SelectionMaskFormat) || !inputs.Scratch->getDesc().isUAV
				|| !valid(inputs.SceneDepth->getDesc(), SceneDepthFormat)
				|| !valid(inputs.LdrColor->getDesc(), nvrhi::Format::RGBA8_UNORM) || !inputs.LdrColor->getDesc().isUAV)
				return MakeError(ErrorCode::InvalidArgument, "Selection target formats, extents or usage flags do not match");
			for (int component = 0; component < 4; ++component)
				if (!std::isfinite(inputs.Color[component]) || inputs.Color[component] < 0.0f || (component == 3 && inputs.Color[component] > 1.0f))
					return MakeError(ErrorCode::InvalidArgument, "Selection color must be finite nonnegative linear RGB and alpha in [0,1]");
			if (inputs.Snapshot->HasCamera && (!SelectionFiniteMatrix(inputs.Snapshot->Camera.View) || !SelectionFiniteMatrix(inputs.Snapshot->Camera.Projection)))
				return MakeError(ErrorCode::InvalidArgument, "Selection camera matrices must be finite");
			for (const MeshDrawItem& item : inputs.Snapshot->Meshes)
				if (!SelectionFiniteMatrix(item.World))
					return MakeError(ErrorCode::InvalidArgument, "Selection mesh transforms must be finite");
			if (inputs.Snapshot->HasCamera && !inputs.Snapshot->SelectedEntities.empty() && (!inputs.Draws || !inputs.Instances))
				return MakeError(ErrorCode::InvalidArgument, "Selection needs its prepared draws and the view's instance buffer");
			return {};
		}

	}

	struct SelectionPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference; device outlives the pass
		std::array<GraphicsPipeline, 6> Mask{};
		ComputePipeline Dilate{};
		ComputePipeline Composite{};
		nvrhi::SamplerHandle Sampler{};
		// M46 (decision 3G): the calm WindConstants the mask's set binds without SelectionRenderInputs::Wind.
		nvrhi::BufferHandle CalmWind{};
	};

	SelectionPass::SelectionPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	SelectionPass::~SelectionPass() = default;

	Result<Scope<SelectionPass>> SelectionPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		auto result = CreateScope<SelectionPass>(ConstructionKey());
		State& state = *result->m_State;
		state.Device = &device;
		for (uint32_t variant = 0; variant < state.Mask.size(); ++variant)
		{
			GraphicsPipelineSpecification specification;
			specification.Layout = Utils::SelectionMaskLayout(variant);
			specification.Framebuffer.colorFormats = { SelectionMaskFormat };
			specification.VertexAttributes = {
				nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(offsetof(MeshVertex, Position)).setElementStride(sizeof(MeshVertex)),
				nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(MeshVertex, TexCoord)).setElementStride(sizeof(MeshVertex))
			};
			// M23 (C-MESH2): the attribute stream at locations 2 and 3, which a Mask material's alpha test reads; M46: MeshVertex's
			// normal and the wind stream at 4 and 5, which the wind's displacement reads.
			const std::array<nvrhi::VertexAttributeDesc, 2> stream = Utils::MakeMeshAttributeStreamAttributes();
			specification.VertexAttributes.insert(specification.VertexAttributes.end(), stream.begin(), stream.end());
			specification.VertexAttributes.push_back(Utils::MakeMeshNormalAttribute());
			specification.VertexAttributes.push_back(Utils::MakeMeshWindStreamAttribute());
			specification.RenderState.rasterState.frontCounterClockwise = true;
			specification.RenderState.rasterState.cullMode = variant % 3 == 0 ? nvrhi::RasterCullMode::Back
				: variant % 3 == 1                                            ? nvrhi::RasterCullMode::Front
																			  : nvrhi::RasterCullMode::None;
			specification.RenderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
			specification.RenderState.blendState.targets[0].setBlendEnable(true).setBlendOp(nvrhi::BlendOp::Max).setColorWriteMask(nvrhi::ColorMask::Red);
			ENGINE_TRY_ASSIGN(state.Mask[variant], pipelines.CreateGraphicsPipeline(specification));
		}
		ENGINE_TRY_ASSIGN(state.Dilate, pipelines.CreateComputePipeline({ .Layout = Utils::SelectionFilterLayout(false) }));
		ENGINE_TRY_ASSIGN(state.Composite, pipelines.CreateComputePipeline({ .Layout = Utils::SelectionFilterLayout(true) }));
		nvrhi::SamplerDesc sampler;
		sampler.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap).setMaxAnisotropy(16.0f);
		ENGINE_TRY_ASSIGN(state.Sampler, device.CreateSampler(sampler));
		nvrhi::BufferDesc calm;
		calm.byteSize = sizeof(WindConstants);
		calm.isConstantBuffer = true;
		calm.initialState = nvrhi::ResourceStates::ConstantBuffer;
		calm.keepInitialState = true;
		calm.debugName = "SelectionPass.CalmWind";
		ENGINE_TRY_ASSIGN(state.CalmWind, device.CreateBuffer(calm));
		ENGINE_TRY_ASSIGN(const nvrhi::CommandListHandle list, device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
		CommandListRecording recording(device, *list);
		const WindConstants calmConstants{};
		list->writeBuffer(state.CalmWind, &calmConstants, sizeof(calmConstants));
		recording.Execute();
		return result;
	}

	std::vector<PipelineLayoutDescription> SelectionPass::GetLayoutDescriptions()
	{
		std::vector<PipelineLayoutDescription> result;
		for (uint32_t variant = 0; variant < 6; ++variant)
			result.push_back(Utils::SelectionMaskLayout(variant));
		result.push_back(Utils::SelectionFilterLayout(false));
		result.push_back(Utils::SelectionFilterLayout(true));
		return result;
	}

	nvrhi::TextureDesc SelectionPass::GetMaskDesc(uint32_t width, uint32_t height)
	{
		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.format = SelectionMaskFormat;
		desc.isShaderResource = true;
		desc.isRenderTarget = true;
		desc.isUAV = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "Selection.Mask";
		return desc;
	}

	SelectionDrawSet SelectionPass::PrepareDraws(GpuResourceCache& cache, const RenderSnapshot& snapshot, bool instancing,
		std::vector<InstanceRecord>& instances, std::span<const DeformedVertexSource> sources, std::span<const MeshDrawItem> staticItems, bool wind)
	{
		ENGINE_CORE_ASSERT(sources.empty() || sources.size() == snapshot.Meshes.size(), "the items' vertex sources are none or one per item");
		SelectionDrawSet set;
		if (!snapshot.HasCamera || snapshot.SelectedEntities.empty())
			return set;
		std::vector<MeshInstanceKey> keys;
		// M43: the snapshot's items, then the selected static items, whose copies' records join the view's Instances.
		for (size_t itemIndex = 0; itemIndex < snapshot.Meshes.size() + staticItems.size(); ++itemIndex)
		{
			const MeshDrawItem& item = itemIndex < snapshot.Meshes.size() ? snapshot.Meshes[itemIndex] : staticItems[itemIndex - snapshot.Meshes.size()];
			// M23 (C23.3): a deformed item's vertices (SkinningPass.h).
			const DeformedVertexSource source = itemIndex < sources.size() ? sources[itemIndex] : DeformedVertexSource{};
			if (!item.Mesh.IsValid() || !item.Entity.IsValid() || std::ranges::find(snapshot.SelectedEntities, item.Entity) == snapshot.SelectedEntities.end()
				|| !Utils::SelectionFiniteMatrix(item.World) || source.Skipped)
				continue;
			// Cache uploads invalidate borrowed references; own this mesh's handles and value arrays before resolving materials.
			const GpuMesh mesh = cache.GetMesh(item.Mesh);
			if (!mesh.VertexBuffer || !mesh.IndexBuffer || !mesh.AttributeBuffer || !mesh.WindBuffer)
				continue;
			// M46 (decision 3E): the level the camera's view draws most of the item's pixels with; nothing beyond its CullDistance.
			const MeshLodSelection lods = SelectViewMeshLod(mesh.LodScreenSizes, mesh.Bounds, item.World, item.Distance, snapshot.Camera);
			if (lods.DrawCount == 0)
				continue;
			const uint32_t level = lods.Level;
			const bool mirrored = glm::determinant(glm::mat3(item.World)) < 0.0f;
			for (uint32_t submeshIndex = 0; submeshIndex < mesh.Submeshes.size(); ++submeshIndex)
			{
				const GpuSubmesh& submesh = mesh.Submeshes[submeshIndex];
				const uint32_t indexOffset = level == 0 ? submesh.IndexOffset : submesh.Lods[level - 1].IndexOffset;
				const uint32_t indexCount = level == 0 ? submesh.IndexCount : submesh.Lods[level - 1].IndexCount;
				if (indexCount == 0)
					continue;
				AssetHandle materialHandle = BuiltinAssetHandles::DefaultMaterial;
				if (submesh.MaterialSlot < item.Materials.size() && item.Materials[submesh.MaterialSlot].IsValid())
					materialHandle = item.Materials[submesh.MaterialSlot];
				else if (submesh.MaterialSlot < mesh.DefaultMaterials.size() && mesh.DefaultMaterials[submesh.MaterialSlot].IsValid())
					materialHandle = mesh.DefaultMaterials[submesh.MaterialSlot];
				const GpuMaterial& material = cache.GetMaterial(materialHandle);
				const bool masked = material.AlphaMode == AlphaMode::Mask;
				if (masked && (!material.Constants || !material.BaseColorMap))
					continue;
				const uint32_t variant = (masked ? 3U : 0U) + (material.DoubleSided ? 2U : (mirrored ? 1U : 0U));
				set.Draws.push_back({ .Vertices = source.Vertices != nullptr ? nvrhi::BufferHandle(source.Vertices) : mesh.VertexBuffer,
					.Attributes = mesh.AttributeBuffer,
					.Wind = mesh.WindBuffer,
					.Indices = mesh.IndexBuffer,
					.IndexOffset = indexOffset,
					.IndexCount = indexCount,
					.Variant = variant,
					.MaterialConstants = masked ? material.Constants : nvrhi::BufferHandle(),
					.BaseColorMap = masked ? material.BaseColorMap : nvrhi::TextureHandle(),
					.Instance = static_cast<uint32_t>(instances.size()) });
				// M46 (decision 3G): a draw whose material has wind sways in a scene with a Wind.
				const bool swaying = wind && material.Wind.Enabled;
				InstanceRecord record = MakeMeshInstanceRecord(item, swaying ? DrawFlagWind : 0U);
				record.WindResponse = swaying ? PackWindResponse(material.Wind) : 0U;
				instances.push_back(record);
				// Only a Mask draw binds its material: the other draws of one mesh batch whatever their materials.
				keys.push_back({ .Pipeline = variant,
					.Material = masked ? materialHandle : AssetHandle(),
					.Mesh = item.Mesh,
					.Submesh = submeshIndex,
					.VertexStream = source.VertexStream,
					.Lod = level });
			}
		}
		if (instancing)
			set.Batches = GroupMeshInstances(keys);
		else
		{
			set.Batches.reserve(set.Draws.size());
			for (uint32_t index = 0; index < set.Draws.size(); ++index)
				set.Batches.push_back({ .First = index, .Count = 1 });
		}
		return set;
	}

	Status SelectionPass::Record(nvrhi::ICommandList& commandList, RenderRecordingContext& recording, PassBindingCache& bindings,
		const SelectionRenderInputs& inputs)
	{
		ENGINE_TRY(Utils::SelectionValidateInputs(inputs));
		State& state = *m_State;
		const RenderSnapshot& snapshot = *inputs.Snapshot;
		const auto& target = inputs.Mask->getDesc();
		uint32_t draws = 0;
		{
			SelectionRecordingScope scope(recording, commandList, "SelectionMask");
			commandList.clearTextureFloat(inputs.Mask, nvrhi::AllSubresources, nvrhi::Color(0.0f));
			if (!snapshot.HasCamera || snapshot.SelectedEntities.empty() || inputs.Draws->Draws.empty())
				return {};
			const SelectionDrawSet& prepared = *inputs.Draws;
			ENGINE_CORE_ASSERT(inputs.Instances->getDesc().structStride == sizeof(InstanceRecord)
					&& inputs.Instances->getDesc().byteSize >= (static_cast<uint64_t>(prepared.Draws.back().Instance) + 1) * sizeof(InstanceRecord),
				"the selection's draws need the view's Instances buffer holding their records");
			ENGINE_TRY_ASSIGN(const nvrhi::FramebufferHandle framebuffer, state.Device->CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(inputs.Mask)));
			const glm::mat4 viewProjection = snapshot.Camera.Projection * snapshot.Camera.View;
			for (const MeshInstanceBatch& batch : prepared.Batches)
			{
				const SelectionDraw& draw = prepared.Draws[batch.First];
				ENGINE_CORE_ASSERT(batch.Count >= 1 && prepared.Draws[batch.First + batch.Count - 1].Instance == draw.Instance + batch.Count - 1,
					"the instance records of a batch's draws must be adjacent");
				const GraphicsPipeline& pipeline = state.Mask[draw.Variant];
				nvrhi::BindingSetDesc viewDesc;
				viewDesc.bindings = {
					nvrhi::BindingSetItem::PushConstants(0, sizeof(SelectionMaskConstants)),
					nvrhi::BindingSetItem::Texture_SRV(0, inputs.SceneDepth),
					nvrhi::BindingSetItem::StructuredBuffer_SRV(Utils::InstancesRegister, inputs.Instances),
					nvrhi::BindingSetItem::ConstantBuffer(Utils::WindConstantsRegister, inputs.Wind != nullptr ? inputs.Wind : state.CalmWind.Get())
				};
				ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * viewSet, bindings.GetOrCreate(*state.Device, viewDesc, *pipeline.BindingLayouts[0]));
				nvrhi::GraphicsState graphics;
				graphics.pipeline = pipeline.Pipeline;
				graphics.framebuffer = framebuffer;
				graphics.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(target.width), static_cast<float>(target.height)));
				graphics.bindings = { viewSet };
				if (draw.MaterialConstants != nullptr)
				{
					nvrhi::BindingSetDesc materialDesc;
					materialDesc.bindings = {
						nvrhi::BindingSetItem::ConstantBuffer(0, draw.MaterialConstants),
						nvrhi::BindingSetItem::Texture_SRV(0, draw.BaseColorMap),
						nvrhi::BindingSetItem::Sampler(0, state.Sampler)
					};
					ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * materialSet, bindings.GetOrCreate(*state.Device, materialDesc, *pipeline.BindingLayouts[1]));
					graphics.bindings.push_back(materialSet);
				}
				graphics.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(draw.Vertices).setSlot(0));
				graphics.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(draw.Attributes).setSlot(Utils::MeshAttributeStreamSlot));
				// M46: slot 2 (no attribute reads it) holds the vertices again, since NVRHI binds the slots as one contiguous range.
				graphics.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(draw.Vertices).setSlot(Utils::PreviousPositionSlot));
				graphics.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(draw.Wind).setSlot(Utils::MeshWindStreamSlot));
				graphics.setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(draw.Indices).setFormat(nvrhi::Format::R32_UINT));
				commandList.setGraphicsState(graphics);
				SelectionMaskConstants constants{};
				constants.ViewProjection = viewProjection;
				constants.FirstInstance = draw.Instance;
				commandList.setPushConstants(&constants, sizeof(constants));
				commandList.drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.IndexCount).setInstanceCount(batch.Count).setStartIndexLocation(draw.IndexOffset));
				++scope.Counters.DrawCalls;
				scope.Counters.Triangles += draw.IndexCount / 3 * batch.Count;
				++draws;
			}
		}
		if (draws == 0)
			return {};
		for (uint32_t stage = 0; stage < 3; ++stage)
		{
			SelectionRecordingScope scope(recording, commandList, stage == 0 ? "SelectionDilateHorizontal" : stage == 1 ? "SelectionDilateVertical"
																														: "SelectionComposite");
			const bool composite = stage == 2;
			const ComputePipeline& pipeline = composite ? state.Composite : state.Dilate;
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::PushConstants(0, sizeof(SelectionFilterConstants)),
				nvrhi::BindingSetItem::Texture_SRV(0, stage == 1 ? inputs.Scratch : inputs.Mask)
			};
			if (composite)
			{
				desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(1, inputs.Scratch));
				desc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(1, inputs.LdrColor));
			}
			else
				desc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(0, stage == 0 ? inputs.Scratch : inputs.Mask));
			ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, bindings.GetOrCreate(*state.Device, desc, *pipeline.BindingLayouts[0]));
			nvrhi::ComputeState compute;
			compute.pipeline = pipeline.Pipeline;
			compute.bindings = { set };
			commandList.setComputeState(compute);
			const SelectionFilterConstants constants{ inputs.Color, inputs.Radius, stage == 1 ? 1u : 0u, {} };
			commandList.setPushConstants(&constants, sizeof(constants));
			commandList.dispatch((target.width + 7) / 8, (target.height + 7) / 8);
			++scope.Counters.Dispatches;
		}
		return {};
	}

}
