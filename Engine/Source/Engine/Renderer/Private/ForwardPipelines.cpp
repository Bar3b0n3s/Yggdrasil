#include "EnginePCH.h"
#include "Engine/Renderer/Private/ForwardPipelines.h"

#include "Engine/Asset/MeshData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/DrawConstants.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/MaterialConstants.h"
#include "Shared/Private/SceneDataViewConstants.h"
#include "Shared/ShadowConstants.h"
#include "Shared/ViewConstants.h"

#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace Engine {

	namespace Utils {

		constexpr std::string_view SceneProgram = "Scene";
		constexpr uint32_t BlendVariantOffset = 2 * MeshCullModeCount;
		constexpr std::array<std::string_view, MeshCullModeCount> CullModeNames = { "CullBack", "CullFront", "CullNone" };

		[[nodiscard]] static nvrhi::BindingLayoutDesc MakeSetLayout(uint32_t set, std::vector<nvrhi::BindingLayoutItem> items)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::All;
			layout.registerSpace = set;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = std::move(items);
			return layout;
		}

		[[nodiscard]] static nvrhi::BindingLayoutDesc MakePrepassViewLayout()
		{
			return MakeSetLayout(0, {
										nvrhi::BindingLayoutItem::ConstantBuffer(ViewConstantsRegister),
										nvrhi::BindingLayoutItem::Sampler(AnisoWrapRegister),
										nvrhi::BindingLayoutItem::PushConstants(DrawConstantsSlot, sizeof(DrawConstants)),
									});
		}

		[[nodiscard]] static nvrhi::BindingLayoutDesc MakeForwardViewLayout()
		{
			return MakeSetLayout(0, {
										nvrhi::BindingLayoutItem::ConstantBuffer(ViewConstantsRegister),
										nvrhi::BindingLayoutItem::ConstantBuffer(ShadowConstantsRegister),
										nvrhi::BindingLayoutItem::ConstantBuffer(EnvironmentConstantsRegister),
										nvrhi::BindingLayoutItem::StructuredBuffer_SRV(LightsRegister),
										nvrhi::BindingLayoutItem::Texture_SRV(ShadowCascadesRegister),
										nvrhi::BindingLayoutItem::Texture_SRV(ShadowAtlasRegister),
										nvrhi::BindingLayoutItem::Texture_SRV(EnvSpecularRegister),
										nvrhi::BindingLayoutItem::Texture_SRV(BrdfLutRegister),
										nvrhi::BindingLayoutItem::Texture_SRV(AmbientOcclusionRegister),
										nvrhi::BindingLayoutItem::Texture_SRV(ViewDepthRegister),
										nvrhi::BindingLayoutItem::Texture_SRV(SceneNormalsRegister),
										nvrhi::BindingLayoutItem::Sampler(LinearClampRegister),
										nvrhi::BindingLayoutItem::Sampler(ShadowCompareRegister),
										nvrhi::BindingLayoutItem::Sampler(AnisoWrapRegister),
										nvrhi::BindingLayoutItem::PushConstants(DrawConstantsSlot, sizeof(DrawConstants)),
									});
		}

		[[nodiscard]] static nvrhi::BindingLayoutDesc MakeMaterialLayout()
		{
			std::vector<nvrhi::BindingLayoutItem> items = { nvrhi::BindingLayoutItem::ConstantBuffer(MaterialConstantsRegister) };
			for (uint32_t map = 0; map < MaterialMapCount; ++map)
				items.push_back(nvrhi::BindingLayoutItem::Texture_SRV(map));
			return MakeSetLayout(1, std::move(items));
		}

		[[nodiscard]] static std::vector<ShaderDefine> MakeAlphaMaskPermutation(bool mask)
		{
			return { { .Key = "ALPHA_MASK", .Value = mask ? "1" : "0" } };
		}

		[[nodiscard]] static PipelineLayoutDescription MakePrepassDescription(uint32_t variant, bool picking = false)
		{
			const bool mask = variant >= MeshCullModeCount;
			return {
				.Name = std::format("Scene{}{}{}", picking ? "Picking" : "Prepass", mask ? "Mask" : "Opaque", CullModeNames[variant % MeshCullModeCount]),
				.Program = std::string(SceneProgram),
				.Entries = { "VSMain", picking ? "PSPicking" : "PSPrepass" },
				.Permutation = MakeAlphaMaskPermutation(mask),
				.BindingLayouts = { MakePrepassViewLayout(), MakeMaterialLayout() },
				.ConstantBuffers = {
					{ .Set = 0, .Register = ViewConstantsRegister, .ByteSize = sizeof(ViewConstants) },
					{ .Set = 1, .Register = MaterialConstantsRegister, .ByteSize = sizeof(MaterialConstants) },
				},
			};
		}

		[[nodiscard]] static PipelineLayoutDescription MakeForwardDescription(uint32_t variant)
		{
			const bool blend = variant >= BlendVariantOffset;
			const bool mask = !blend && variant >= MeshCullModeCount;
			const std::string_view mode = blend ? "Blend" : (mask ? "Mask" : "Opaque");
			return {
				.Name = std::format("{}{}{}", blend ? "SceneTransparent" : "SceneForward", mode, CullModeNames[variant % MeshCullModeCount]),
				.Program = std::string(SceneProgram),
				.Entries = { "VSMain", "PSForward" },
				.Permutation = MakeAlphaMaskPermutation(mask),
				.BindingLayouts = { MakeForwardViewLayout(), MakeMaterialLayout() },
				.ConstantBuffers = {
					{ .Set = 0, .Register = ViewConstantsRegister, .ByteSize = sizeof(ViewConstants) },
					{ .Set = 0, .Register = EnvironmentConstantsRegister, .ByteSize = sizeof(EnvironmentConstants) },
					{ .Set = 0, .Register = ShadowConstantsRegister, .ByteSize = sizeof(ShadowConstants) },
					{ .Set = 1, .Register = MaterialConstantsRegister, .ByteSize = sizeof(MaterialConstants) },
				},
			};
		}

		// MeshVertex (§6.8) at locations 0 to 3, the order of the Scene program's VertexInput.
		[[nodiscard]] static std::vector<nvrhi::VertexAttributeDesc> MakeMeshVertexAttributes()
		{
			constexpr uint32_t Stride = sizeof(MeshVertex);
			const auto attribute = [](const char* name, nvrhi::Format format, size_t offset)
			{
				return nvrhi::VertexAttributeDesc().setName(name).setFormat(format).setOffset(static_cast<uint32_t>(offset)).setElementStride(Stride);
			};
			return {
				attribute("POSITION", nvrhi::Format::RGB32_FLOAT, offsetof(MeshVertex, Position)),
				attribute("NORMAL", nvrhi::Format::RGB32_FLOAT, offsetof(MeshVertex, Normal)),
				attribute("TANGENT", nvrhi::Format::RGBA32_FLOAT, offsetof(MeshVertex, Tangent)),
				attribute("TEXCOORD", nvrhi::Format::RG32_FLOAT, offsetof(MeshVertex, TexCoord)),
			};
		}

		[[nodiscard]] static nvrhi::RasterCullMode ToRasterCullMode(uint32_t variant)
		{
			switch (static_cast<MeshCullMode>(variant % MeshCullModeCount))
			{
				case MeshCullMode::Back:  return nvrhi::RasterCullMode::Back;
				case MeshCullMode::Front: return nvrhi::RasterCullMode::Front;
				case MeshCullMode::None:  return nvrhi::RasterCullMode::None;
			}
			return nvrhi::RasterCullMode::Back;
		}

		// A mesh pipeline: the variant's cull mode with counter-clockwise front faces (§8.3), reverse-Z depth tested with
		// GreaterOrEqual and written or not, one colour target.
		[[nodiscard]] static GraphicsPipelineSpecification MakeMeshPipelineSpecification(PipelineLayoutDescription layout, const MeshBindingLayouts& layouts,
			bool prepass, uint32_t variant)
		{
			GraphicsPipelineSpecification specification;
			specification.Layout = std::move(layout);
			specification.SharedBindingLayouts = { prepass ? layouts.PrepassView : layouts.ForwardView, layouts.Material };
			specification.VertexAttributes = MakeMeshVertexAttributes();
			specification.RenderState.rasterState.setCullMode(ToRasterCullMode(variant)).setFrontCounterClockwise(true);
			specification.RenderState.depthStencilState.setDepthTestEnable(true)
				.setDepthWriteEnable(prepass)
				.setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual)
				.setStencilEnable(false);
			if (!prepass && variant >= BlendVariantOffset)
			{
				// Straight alpha: colour = src * a + dst * (1 - a); the target's alpha accumulates coverage.
				specification.RenderState.blendState.targets[0]
					.setBlendEnable(true)
					.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
					.setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
					.setBlendOp(nvrhi::BlendOp::Add)
					.setSrcBlendAlpha(nvrhi::BlendFactor::One)
					.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
					.setBlendOpAlpha(nvrhi::BlendOp::Add);
			}
			specification.Framebuffer.addColorFormat(prepass ? SceneNormalsFormat : SceneColorFormat);
			specification.Framebuffer.setDepthFormat(SceneDepthFormat);
			return specification;
		}

		MeshCullMode SelectCullMode(bool mirrored, bool doubleSided)
		{
			if (doubleSided)
				return MeshCullMode::None;
			return mirrored ? MeshCullMode::Front : MeshCullMode::Back;
		}

		uint32_t GetPrepassVariant(AlphaMode alphaMode, MeshCullMode cullMode)
		{
			ENGINE_CORE_ASSERT(alphaMode != AlphaMode::Blend, "Blend draws are not in the prepass");
			return (alphaMode == AlphaMode::Mask ? MeshCullModeCount : 0) + static_cast<uint32_t>(cullMode);
		}

		uint32_t GetForwardVariant(AlphaMode alphaMode, MeshCullMode cullMode)
		{
			const uint32_t cull = static_cast<uint32_t>(cullMode);
			switch (alphaMode)
			{
				case AlphaMode::Opaque: return cull;
				case AlphaMode::Mask:   return MeshCullModeCount + cull;
				case AlphaMode::Blend:  return BlendVariantOffset + cull;
			}
			return cull;
		}

		std::vector<PipelineLayoutDescription> GetMeshLayoutDescriptions()
		{
			std::vector<PipelineLayoutDescription> descriptions;
			descriptions.reserve(PrepassVariantCount + ForwardVariantCount);
			for (uint32_t variant = 0; variant < PrepassVariantCount; ++variant)
				descriptions.push_back(MakePrepassDescription(variant));
			for (uint32_t variant = 0; variant < ForwardVariantCount; ++variant)
				descriptions.push_back(MakeForwardDescription(variant));
			for (uint32_t variant = 0; variant < PrepassVariantCount; ++variant)
				descriptions.push_back(MakePrepassDescription(variant, true));
			for (uint32_t variant = 0; variant < MeshCullModeCount; ++variant)
			{
				auto description = MakePrepassDescription(variant);
				description.Name = std::format("SceneOverdraw{}", CullModeNames[variant]);
				description.Entries = { "VSMain", "PSOverdraw" };
				descriptions.push_back(std::move(description));
			}
			return descriptions;
		}

		PipelineLayoutDescription GetSceneDataViewLayout()
		{
			return {
				.Name = "SceneDataView",
				.Program = "SceneDataView",
				.Entries = { "CSMain" },
				.BindingLayouts = { MakeSetLayout(0, { nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::ConstantBuffer(1), nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Texture_SRV(2), nvrhi::BindingLayoutItem::Texture_SRV(3), nvrhi::BindingLayoutItem::Texture_SRV(4), nvrhi::BindingLayoutItem::Texture_UAV(0), nvrhi::BindingLayoutItem::PushConstants(3, sizeof(SceneDataViewConstants)) }) },
				.StorageImages = { { .Set = 0, .Register = 0, .Format = nvrhi::Format::RGBA8_UNORM } },
				.ConstantBuffers = { { .Set = 0, .Register = 0, .ByteSize = sizeof(ViewConstants) },
					{ .Set = 0, .Register = 1, .ByteSize = sizeof(ShadowConstants) } },
			};
		}

		Result<MeshBindingLayouts> CreateMeshBindingLayouts(PipelineFactory& pipelines)
		{
			MeshBindingLayouts layouts;
			ENGINE_TRY_ASSIGN(const std::vector<nvrhi::BindingLayoutHandle> forward, pipelines.CreateBindingLayouts(MakeForwardDescription(0)));
			ENGINE_TRY_ASSIGN(const std::vector<nvrhi::BindingLayoutHandle> prepass, pipelines.CreateBindingLayouts(MakePrepassDescription(0)));
			layouts.ForwardView = forward[0];
			layouts.Material = forward[1];
			layouts.PrepassView = prepass[0];
			return layouts;
		}

		Result<GraphicsPipeline> CreatePrepassPipeline(PipelineFactory& pipelines, const MeshBindingLayouts& layouts, uint32_t variant, bool picking)
		{
			ENGINE_CORE_ASSERT(variant < PrepassVariantCount, "prepass variant {} out of range", variant);
			auto specification = MakeMeshPipelineSpecification(MakePrepassDescription(variant, picking), layouts, true, variant);
			if (picking)
				specification.Framebuffer.addColorFormat(nvrhi::Format::R32_UINT);
			return pipelines.CreateGraphicsPipeline(specification);
		}

		Result<GraphicsPipeline> CreateOverdrawPipeline(PipelineFactory& pipelines, const MeshBindingLayouts& layouts, uint32_t cullMode)
		{
			auto layout = MakePrepassDescription(cullMode);
			layout.Name = std::format("SceneOverdraw{}", CullModeNames[cullMode]);
			layout.Entries = { "VSMain", "PSOverdraw" };
			auto specification = MakeMeshPipelineSpecification(std::move(layout), layouts, true, cullMode);
			specification.Framebuffer = nvrhi::FramebufferInfo().addColorFormat(nvrhi::Format::RGBA16_FLOAT);
			specification.RenderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
			specification.RenderState.blendState.targets[0].setBlendEnable(true).setSrcBlend(nvrhi::BlendFactor::One).setDestBlend(nvrhi::BlendFactor::One).setBlendOp(nvrhi::BlendOp::Add).setSrcBlendAlpha(nvrhi::BlendFactor::One).setDestBlendAlpha(nvrhi::BlendFactor::One).setBlendOpAlpha(nvrhi::BlendOp::Add);
			return pipelines.CreateGraphicsPipeline(specification);
		}

		Result<GraphicsPipeline> CreateForwardPipeline(PipelineFactory& pipelines, const MeshBindingLayouts& layouts, uint32_t variant, RenderDebugView view)
		{
			ENGINE_CORE_ASSERT(variant < ForwardVariantCount, "forward variant {} out of range", variant);
			GraphicsPipelineSpecification specification = MakeMeshPipelineSpecification(MakeForwardDescription(variant), layouts, false, variant);
			if (view != RenderDebugView::Lit)
			{
				specification.Specializations = { nvrhi::ShaderSpecialization::UInt32(SceneDebugViewConstantId, static_cast<uint32_t>(view)) };
				specification.Layout.Name += std::format(".{}", RenderDebugViewToString(view));
			}
			return pipelines.CreateGraphicsPipeline(specification);
		}

	}

}
