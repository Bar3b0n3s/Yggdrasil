#include "EnginePCH.h"
#include "Engine/Renderer/ShadowPass.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/Private/ForwardPipelines.h"
#include "Shared/DrawConstants.h"
#include "Shared/MaterialConstants.h"
#include "Shared/ShadowCasterConstants.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>

namespace Engine {

	namespace Utils {

		[[nodiscard]] static PipelineLayoutDescription ShadowLayout(uint32_t variant)
		{
			const bool mask = variant >= 3;
			constexpr std::array<const char*, 3> Names{ "Back", "Front", "None" };
			nvrhi::BindingLayoutDesc view;
			view.visibility = nvrhi::ShaderType::All;
			view.registerSpace = 0;
			view.registerSpaceIsDescriptorSet = true;
			view.bindings = { nvrhi::BindingLayoutItem::ConstantBuffer(0),
				nvrhi::BindingLayoutItem::PushConstants(DrawConstantsSlot, sizeof(DrawConstants)) };
			if (mask)
				view.bindings.push_back(nvrhi::BindingLayoutItem::Sampler(2));
			PipelineLayoutDescription result;
			result.Name = std::format("Shadow{}Cull{}", mask ? "Mask" : "Opaque", Names[variant % 3]);
			result.Program = "Shadow";
			result.Entries = { "VSMain", "PSMain" };
			result.Permutation = { { .Key = "ALPHA_MASK", .Value = mask ? "1" : "0" } };
			result.BindingLayouts = { view };
			result.ConstantBuffers = { { .Set = 0, .Register = 0, .ByteSize = sizeof(ShadowCasterConstants) } };
			if (mask)
			{
				nvrhi::BindingLayoutDesc material;
				material.visibility = nvrhi::ShaderType::Pixel;
				material.registerSpace = 1;
				material.registerSpaceIsDescriptorSet = true;
				material.bindings = { nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(0) };
				result.BindingLayouts.push_back(material);
				result.ConstantBuffers.push_back({ .Set = 1, .Register = 0, .ByteSize = sizeof(MaterialConstants) });
			}
			return result;
		}

		[[nodiscard]] static bool IsFiniteShadowWorld(const glm::mat4& world)
		{
			for (glm::length_t column = 0; column < 4; ++column)
				for (glm::length_t row = 0; row < 4; ++row)
					if (!std::isfinite(world[column][row]))
						return false;
			return true;
		}

		[[nodiscard]] static bool IsOutsideShadow(const Aabb& bounds, const glm::mat4& worldViewProjection, bool depthClamp)
		{
			if (bounds.IsEmpty())
				return false;
			std::array<bool, 6> outside{ true, true, true, true, true, true };
			for (uint32_t index = 0; index < 8; ++index)
			{
				const glm::vec4 clip = worldViewProjection
					* glm::vec4((index & 1U) ? bounds.Max.x : bounds.Min.x, (index & 2U) ? bounds.Max.y : bounds.Min.y,
						(index & 4U) ? bounds.Max.z : bounds.Min.z, 1);
				outside[0] = outside[0] && clip.x < -clip.w;
				outside[1] = outside[1] && clip.x > clip.w;
				outside[2] = outside[2] && clip.y < -clip.w;
				outside[3] = outside[3] && clip.y > clip.w;
				outside[4] = outside[4] && clip.z < 0;
				outside[5] = outside[5] && clip.z > clip.w;
			}
			// Pancake only the near plane; far casters cannot shadow any receiver in the volume.
			return std::ranges::any_of(std::span(outside).first(depthClamp ? 5 : 6), [](bool value)
			{
				return value;
			});
		}

		struct ShadowDraw
		{
			nvrhi::BufferHandle Vertices{};
			nvrhi::BufferHandle Indices{};
			GpuSubmesh Submesh{};
			glm::mat4 World{ 1.0f };
			nvrhi::IBindingSet* Material = nullptr; // caller's binding cache, borrowed until Record returns
			uint32_t Variant = 0;
		};

		// Pair diagnostic scopes on every error path; recorded counters are updated only after a draw.
		struct ShadowRecordingScope
		{
			RenderRecordingContext& Recording;
			RenderPassCounters Counters{};
			~ShadowRecordingScope() { Recording.EndPass(Counters); }
		};

	}

	struct ShadowPass::State
	{
		GraphicsDevice* Device = nullptr; // back-reference; the device outlives the pass
		std::array<GraphicsPipeline, PipelineCount> Pipelines{};
		nvrhi::SamplerHandle AnisoWrap{};

		[[nodiscard]] Result<std::vector<Utils::ShadowDraw>> ResolveCasters(PassBindingCache& bindings, GpuResourceCache& cache,
			std::span<const MeshDrawItem> casters);
		[[nodiscard]] Status RecordView(nvrhi::ICommandList& commandList, PassBindingCache& bindings,
			std::span<const Utils::ShadowDraw> draws, nvrhi::ITexture& target, uint32_t slice,
			const nvrhi::Viewport& viewport, const ShadowCasterConstants& constants,
			RenderPassCounters& counters);
	};

	ShadowPass::ShadowPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	ShadowPass::~ShadowPass() = default;

	Result<Scope<ShadowPass>> ShadowPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<ShadowPass> pass = CreateScope<ShadowPass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		for (uint32_t variant = 0; variant < PipelineCount; ++variant)
		{
			GraphicsPipelineSpecification specification;
			specification.Layout = Utils::ShadowLayout(variant);
			if (variant % 3 != 0)
				specification.SharedBindingLayouts = state.Pipelines[variant / 3 * 3].BindingLayouts;
			constexpr uint32_t Stride = sizeof(MeshVertex);
			specification.VertexAttributes = {
				nvrhi::VertexAttributeDesc()
					.setName("POSITION")
					.setFormat(nvrhi::Format::RGB32_FLOAT)
					.setOffset(offsetof(MeshVertex, Position))
					.setElementStride(Stride),
				nvrhi::VertexAttributeDesc()
					.setName("TEXCOORD")
					.setFormat(nvrhi::Format::RG32_FLOAT)
					.setOffset(offsetof(MeshVertex, TexCoord))
					.setElementStride(Stride),
			};
			specification.Framebuffer.setDepthFormat(nvrhi::Format::D32);
			constexpr std::array<nvrhi::RasterCullMode, 3> Modes{ nvrhi::RasterCullMode::Back, nvrhi::RasterCullMode::Front,
				nvrhi::RasterCullMode::None };
			specification.RenderState.rasterState.setCullMode(Modes[variant % 3])
				.setFrontCounterClockwise(true)
				.setDepthClipEnable(true); // near pancaking is explicit in the shader: NVRHI does not expose Vulkan depth clamp
			specification.RenderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(true).setDepthFunc(
				nvrhi::ComparisonFunc::GreaterOrEqual);
			ENGINE_TRY_ASSIGN(state.Pipelines[variant], pipelines.CreateGraphicsPipeline(specification));
		}
		nvrhi::SamplerDesc sampler;
		sampler.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap).setMaxAnisotropy(16);
		ENGINE_TRY_ASSIGN(state.AnisoWrap, device.CreateSampler(sampler));
		return pass;
	}

	std::vector<PipelineLayoutDescription> ShadowPass::GetLayoutDescriptions()
	{
		std::vector<PipelineLayoutDescription> result;
		for (uint32_t variant = 0; variant < PipelineCount; ++variant)
			result.push_back(Utils::ShadowLayout(variant));
		return result;
	}

	nvrhi::TextureDesc ShadowPass::GetCascadeTargetDesc(uint32_t shadowMapSize)
	{
		ENGINE_CORE_ASSERT(shadowMapSize >= 256 && shadowMapSize <= 8192 && (shadowMapSize & (shadowMapSize - 1)) == 0,
			"Invalid shadow map size");
		nvrhi::TextureDesc desc;
		desc.width = desc.height = shadowMapSize;
		desc.arraySize = MaxShadowCascades;
		desc.dimension = nvrhi::TextureDimension::Texture2DArray;
		desc.format = nvrhi::Format::D32;
		desc.isRenderTarget = true;
		desc.isShaderResource = true;
		desc.initialState = nvrhi::ResourceStates::DepthWrite;
		desc.keepInitialState = true;
		desc.debugName = "DirectionalShadows";
		return desc;
	}

	nvrhi::TextureDesc ShadowPass::GetAtlasTargetDesc()
	{
		nvrhi::TextureDesc desc = GetCascadeTargetDesc(SpotShadowAtlasSize);
		desc.arraySize = 1;
		desc.dimension = nvrhi::TextureDimension::Texture2D;
		desc.debugName = "SpotShadows";
		return desc;
	}

	Result<std::vector<Utils::ShadowDraw>> ShadowPass::State::ResolveCasters(PassBindingCache& bindings, GpuResourceCache& cache,
		std::span<const MeshDrawItem> casters)
	{
		std::vector<Utils::ShadowDraw> draws;
		for (const MeshDrawItem& item : casters)
		{
			if (!item.CastShadows || !item.Mesh.IsValid())
				continue;
			if (!Utils::IsFiniteShadowWorld(item.World))
				return MakeError(ErrorCode::InvalidArgument, "Shadow caster {} has a non-finite world transform", item.Entity);
			// Cache uploads invalidate borrowed references; own this mesh's handles and value arrays before resolving materials.
			const GpuMesh mesh = cache.GetMesh(item.Mesh);
			if (!mesh.VertexBuffer || !mesh.IndexBuffer)
				continue;
			const bool mirrored = glm::determinant(glm::mat3(item.World)) < 0;
			for (const GpuSubmesh& submesh : mesh.Submeshes)
			{
				if (submesh.IndexCount == 0)
					continue;
				AssetHandle materialHandle = BuiltinAssetHandles::DefaultMaterial;
				if (submesh.MaterialSlot < item.Materials.size() && item.Materials[submesh.MaterialSlot].IsValid())
					materialHandle = item.Materials[submesh.MaterialSlot];
				else if (submesh.MaterialSlot < mesh.DefaultMaterials.size() && mesh.DefaultMaterials[submesh.MaterialSlot].IsValid())
					materialHandle = mesh.DefaultMaterials[submesh.MaterialSlot];
				const GpuMaterial& material = cache.GetMaterial(materialHandle);
				if (material.AlphaMode == AlphaMode::Blend || !material.Constants || !material.BaseColorMap)
					continue;
				const uint32_t variant =
					Utils::GetPrepassVariant(material.AlphaMode, Utils::SelectCullMode(mirrored, material.DoubleSided));
				nvrhi::IBindingSet* materialSet = nullptr;
				if (material.AlphaMode == AlphaMode::Mask)
				{
					nvrhi::BindingSetDesc desc;
					desc.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, material.Constants),
						nvrhi::BindingSetItem::Texture_SRV(0, material.BaseColorMap) };
					ENGINE_TRY_ASSIGN(materialSet, bindings.GetOrCreate(*Device, desc, *Pipelines[variant].BindingLayouts[1]));
				}
				draws.push_back({ .Vertices = mesh.VertexBuffer,
					.Indices = mesh.IndexBuffer,
					.Submesh = submesh,
					.World = item.World,
					.Material = materialSet,
					.Variant = variant });
			}
		}
		return draws;
	}

	Status ShadowPass::State::RecordView(nvrhi::ICommandList& commandList, PassBindingCache& bindings,
		std::span<const Utils::ShadowDraw> draws, nvrhi::ITexture& target, uint32_t slice,
		const nvrhi::Viewport& viewport, const ShadowCasterConstants& constants,
		RenderPassCounters& counters)
	{
		ENGINE_TRY_ASSIGN(
			auto framebuffer,
			Device->CreateFramebuffer(nvrhi::FramebufferDesc().setDepthAttachment(&target, nvrhi::TextureSubresourceSet(0, 1, slice, 1))));
		nvrhi::BufferDesc bufferDesc;
		bufferDesc.byteSize = sizeof(constants);
		bufferDesc.isConstantBuffer = true;
		bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
		bufferDesc.keepInitialState = true;
		bufferDesc.debugName = "ShadowCasterConstants";
		ENGINE_TRY_ASSIGN(auto buffer, Device->CreateBuffer(bufferDesc));
		commandList.writeBuffer(buffer, &constants, sizeof(constants));
		std::array<nvrhi::IBindingSet*, 2> viewSets{};
		for (uint32_t mask = 0; mask < 2; ++mask)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, buffer),
				nvrhi::BindingSetItem::PushConstants(Utils::DrawConstantsSlot, sizeof(DrawConstants)) };
			if (mask != 0)
				desc.bindings.push_back(nvrhi::BindingSetItem::Sampler(2, AnisoWrap));
			ENGINE_TRY_ASSIGN(viewSets[mask], bindings.GetOrCreate(*Device, desc, *Pipelines[mask * 3].BindingLayouts[0]));
		}
		for (const Utils::ShadowDraw& draw : draws)
		{
			if (Utils::IsOutsideShadow(draw.Submesh.Bounds, constants.ViewProjection * draw.World, constants.Bias.z != 0.0f))
				continue;
			nvrhi::GraphicsState state;
			state.pipeline = Pipelines[draw.Variant].Pipeline;
			state.framebuffer = framebuffer;
			state.viewport.addViewportAndScissorRect(viewport);
			state.bindings = { viewSets[draw.Variant / 3] };
			if (draw.Material != nullptr)
				state.bindings.push_back(draw.Material);
			state.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(draw.Vertices).setSlot(0));
			state.setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(draw.Indices).setFormat(nvrhi::Format::R32_UINT));
			commandList.setGraphicsState(state);
			DrawConstants push{};
			push.World = draw.World;
			commandList.setPushConstants(&push, sizeof(push));
			commandList.drawIndexed(
				nvrhi::DrawArguments().setVertexCount(draw.Submesh.IndexCount).setStartIndexLocation(draw.Submesh.IndexOffset));
			++counters.DrawCalls;
			counters.Triangles += draw.Submesh.IndexCount / 3;
		}
		return {};
	}

	Result<ShadowRenderResult> ShadowPass::Record(nvrhi::ICommandList& commandList, RenderRecordingContext& recording,
		PassBindingCache& bindings, GpuResourceCache& cache, AssetManager& /*assets*/,
		const ShadowRenderInputs& inputs)
	{
		const bool directional = inputs.Cascades && inputs.Cascades->Count != 0;
		const bool spots = inputs.Spots && !inputs.Spots->Tiles.empty();
		if (!directional && !spots)
			return ShadowRenderResult{};
		ENGINE_CORE_ASSERT(std::isfinite(inputs.DepthBias) && inputs.DepthBias >= 0 && std::isfinite(inputs.NormalBias)
				&& inputs.NormalBias >= 0,
			"Invalid shadow bias");
		ENGINE_TRY_ASSIGN(auto draws, m_State->ResolveCasters(bindings, cache, inputs.Casters));
		ShadowRenderResult result;
		if (directional)
		{
			ENGINE_CORE_ASSERT(inputs.CascadeTarget && inputs.Cascades->Count <= MaxShadowCascades,
				"Directional shadow target/cascade count is invalid");
			const nvrhi::TextureDesc& desc = inputs.CascadeTarget->getDesc();
			ENGINE_CORE_ASSERT(desc.format == nvrhi::Format::D32 && desc.arraySize == MaxShadowCascades && desc.width == desc.height,
				"Invalid cascade target");
			recording.BeginPass("DirectionalShadows", &commandList);
			Utils::ShadowRecordingScope scope{ recording };
			commandList.clearDepthStencilTexture(inputs.CascadeTarget, nvrhi::AllSubresources, true, 0, false, 0);
			for (uint32_t index = 0; index < inputs.Cascades->Count; ++index)
			{
				const ShadowCascade& cascade = inputs.Cascades->Cascades[index];
				ShadowCasterConstants constants{};
				constants.ViewProjection = cascade.ViewProjection;
				constants.Bias = glm::vec4(inputs.DepthBias, cascade.TexelWorldSize / (cascade.LightFar - cascade.LightNear),
					m_State->Device->GetInfo().DepthClamp ? 1.0f : 0.0f, 0);
				ENGINE_TRY(m_State->RecordView(commandList, bindings, draws, *inputs.CascadeTarget, index,
					nvrhi::Viewport(static_cast<float>(desc.width), static_cast<float>(desc.height)), constants,
					scope.Counters));
			}
			result.DrawCalls += scope.Counters.DrawCalls;
			result.Triangles += scope.Counters.Triangles;
		}
		if (spots)
		{
			ENGINE_CORE_ASSERT(inputs.AtlasTarget, "Spot shadow target is missing");
			const nvrhi::TextureDesc& desc = inputs.AtlasTarget->getDesc();
			ENGINE_CORE_ASSERT(desc.format == nvrhi::Format::D32 && desc.width == SpotShadowAtlasSize && desc.height == SpotShadowAtlasSize
					&& desc.arraySize == 1,
				"Invalid spot shadow target");
			recording.BeginPass("SpotShadows", &commandList);
			Utils::ShadowRecordingScope scope{ recording };
			commandList.clearDepthStencilTexture(inputs.AtlasTarget, nvrhi::AllSubresources, true, 0, false, 0);
			for (const SpotShadowTile& tile : inputs.Spots->Tiles)
			{
				const float left = static_cast<float>(tile.X + SpotShadowGuardTexels);
				const float top = static_cast<float>(tile.Y + SpotShadowGuardTexels);
				const float size = static_cast<float>(tile.Size - 2 * SpotShadowGuardTexels);
				ShadowCasterConstants constants{};
				constants.ViewProjection = tile.ViewProjection;
				constants.Bias = glm::vec4(1, 0, 0, 0);
				ENGINE_TRY(m_State->RecordView(commandList, bindings, draws, *inputs.AtlasTarget, 0,
					nvrhi::Viewport(left, left + size, top, top + size, 0, 1), constants, scope.Counters));
			}
			result.DrawCalls += scope.Counters.DrawCalls;
			result.Triangles += scope.Counters.Triangles;
		}
		return result;
	}

}
