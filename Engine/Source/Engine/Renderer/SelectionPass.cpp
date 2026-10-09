#include "EnginePCH.h"
#include "Engine/Renderer/SelectionPass.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Shared/MaterialConstants.h"
#include "Shared/Private/SelectionPassConstants.h"

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
			view.bindings = {
				nvrhi::BindingLayoutItem::PushConstants(0, sizeof(SelectionMaskConstants)),
				nvrhi::BindingLayoutItem::Texture_SRV(0)
			};
			PipelineLayoutDescription description{
				.Name = std::format("SelectionMask{}", variant),
				.Program = "SelectionMask",
				.Entries = { "VSMain", "PSMask" },
				.Permutation = { { "ALPHA_MASK", masked ? "1" : "0" } },
				.BindingLayouts = { view }
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
				.StorageImages = { { .Set = 0, .Register = composite ? 1u : 0u, .Format = composite ? nvrhi::Format::RGBA8_UNORM : nvrhi::Format::R8_UNORM } }
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
			if (mask.width == 0 || mask.height == 0 || !valid(mask, nvrhi::Format::R8_UNORM) || !mask.isUAV || !mask.isRenderTarget
				|| !valid(inputs.Scratch->getDesc(), nvrhi::Format::R8_UNORM) || !inputs.Scratch->getDesc().isUAV
				|| !valid(inputs.SceneDepth->getDesc(), nvrhi::Format::D32)
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
			specification.Framebuffer.colorFormats = { nvrhi::Format::R8_UNORM };
			specification.VertexAttributes = {
				nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(offsetof(MeshVertex, Position)).setElementStride(sizeof(MeshVertex)),
				nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(MeshVertex, TexCoord)).setElementStride(sizeof(MeshVertex))
			};
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
		desc.format = nvrhi::Format::R8_UNORM;
		desc.isShaderResource = true;
		desc.isRenderTarget = true;
		desc.isUAV = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "Selection.Mask";
		return desc;
	}

	Status SelectionPass::Record(nvrhi::ICommandList& commandList, RenderRecordingContext& recording, PassBindingCache& bindings,
		GpuResourceCache& cache, AssetManager& /*assets*/, const SelectionRenderInputs& inputs)
	{
		ENGINE_TRY(Utils::SelectionValidateInputs(inputs));
		State& state = *m_State;
		const RenderSnapshot& snapshot = *inputs.Snapshot;
		const auto& target = inputs.Mask->getDesc();
		uint32_t draws = 0;
		{
			SelectionRecordingScope scope(recording, commandList, "SelectionMask");
			commandList.clearTextureFloat(inputs.Mask, nvrhi::AllSubresources, nvrhi::Color(0.0f));
			if (!snapshot.HasCamera || snapshot.SelectedEntities.empty())
				return {};
			ENGINE_TRY_ASSIGN(const nvrhi::FramebufferHandle framebuffer, state.Device->CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(inputs.Mask)));
			for (const MeshDrawItem& item : snapshot.Meshes)
			{
				if (!item.Mesh.IsValid() || !item.Entity.IsValid() || std::ranges::find(snapshot.SelectedEntities, item.Entity) == snapshot.SelectedEntities.end())
					continue;
				const GpuMesh mesh = cache.GetMesh(item.Mesh);
				if (!mesh.VertexBuffer || !mesh.IndexBuffer)
					continue;
				for (const GpuSubmesh& submesh : mesh.Submeshes)
				{
					AssetHandle materialHandle = BuiltinAssetHandles::DefaultMaterial;
					if (submesh.MaterialSlot < item.Materials.size() && item.Materials[submesh.MaterialSlot].IsValid())
						materialHandle = item.Materials[submesh.MaterialSlot];
					else if (submesh.MaterialSlot < mesh.DefaultMaterials.size() && mesh.DefaultMaterials[submesh.MaterialSlot].IsValid())
						materialHandle = mesh.DefaultMaterials[submesh.MaterialSlot];
					const GpuMaterial material = cache.GetMaterial(materialHandle);
					const bool masked = material.AlphaMode == AlphaMode::Mask;
					if (masked && (!material.Constants || !material.BaseColorMap))
						continue;
					const uint32_t variant = (masked ? 3u : 0u) + (material.DoubleSided ? 2u : glm::determinant(glm::mat3(item.World)) < 0.0f ? 1u
																																			  : 0u);
					const GraphicsPipeline& pipeline = state.Mask[variant];
					nvrhi::BindingSetDesc viewDesc;
					viewDesc.bindings = {
						nvrhi::BindingSetItem::PushConstants(0, sizeof(SelectionMaskConstants)),
						nvrhi::BindingSetItem::Texture_SRV(0, inputs.SceneDepth)
					};
					ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * viewSet, bindings.GetOrCreate(*state.Device, viewDesc, *pipeline.BindingLayouts[0]));
					nvrhi::GraphicsState graphics;
					graphics.pipeline = pipeline.Pipeline;
					graphics.framebuffer = framebuffer;
					graphics.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(target.width), static_cast<float>(target.height)));
					graphics.bindings = { viewSet };
					if (masked)
					{
						nvrhi::BindingSetDesc materialDesc;
						materialDesc.bindings = {
							nvrhi::BindingSetItem::ConstantBuffer(0, material.Constants),
							nvrhi::BindingSetItem::Texture_SRV(0, material.BaseColorMap),
							nvrhi::BindingSetItem::Sampler(0, state.Sampler)
						};
						ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * materialSet, bindings.GetOrCreate(*state.Device, materialDesc, *pipeline.BindingLayouts[1]));
						graphics.bindings.push_back(materialSet);
					}
					graphics.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(mesh.VertexBuffer).setSlot(0));
					graphics.setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(mesh.IndexBuffer).setFormat(nvrhi::Format::R32_UINT));
					commandList.setGraphicsState(graphics);
					const SelectionMaskConstants constants{ item.World, snapshot.Camera.Projection * snapshot.Camera.View };
					commandList.setPushConstants(&constants, sizeof(constants));
					commandList.drawIndexed(nvrhi::DrawArguments().setVertexCount(submesh.IndexCount).setStartIndexLocation(submesh.IndexOffset));
					++scope.Counters.DrawCalls;
					scope.Counters.Triangles += submesh.IndexCount / 3;
					++draws;
				}
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
