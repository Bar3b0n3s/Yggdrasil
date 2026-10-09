#include "EnginePCH.h"
#include "Engine/Renderer/GtaoPass.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/DepthPyramidPass.h"
#include "Engine/Renderer/Private/DepthGtaoValidation.h"
#include "Shared/GtaoConstants.h"

#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace Engine {

	struct GtaoPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		std::vector<ComputePipeline> Pipelines{};
	};

	GtaoPass::GtaoPass(ConstructionKey)
		: m_State(CreateScope<State>())
	{
	}

	GtaoPass::~GtaoPass() = default;

	Result<Scope<GtaoPass>> GtaoPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<GtaoPass> pass = CreateScope<GtaoPass>(ConstructionKey());
		pass->m_State->Device = &device;
		for (PipelineLayoutDescription& description : GetLayoutDescriptions())
		{
			ENGINE_TRY_ASSIGN(ComputePipeline pipeline, pipelines.CreateComputePipeline({ .Layout = std::move(description) }));
			pass->m_State->Pipelines.push_back(std::move(pipeline));
		}
		return pass;
	}

	std::vector<PipelineLayoutDescription> GtaoPass::GetLayoutDescriptions()
	{
		std::vector<PipelineLayoutDescription> descriptions;
		for (const std::string entry : { "CSMain", "CSDenoise" })
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = { nvrhi::BindingLayoutItem::PushConstants(0, sizeof(GtaoConstants)),
				nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1),
				nvrhi::BindingLayoutItem::Texture_UAV(0) };
			if (entry == "CSDenoise")
				layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(2));
			descriptions.push_back({ .Name = "Gtao." + entry, .Program = "Gtao", .Entries = { entry }, .BindingLayouts = { layout }, .StorageImages = { { .Set = 0, .Register = 0, .Format = nvrhi::Format::R8_UNORM } } });
		}
		return descriptions;
	}

	Result<float> ComputeGtaoScreenRadius(const CameraData& camera, float radius, float linearDepth)
	{
		if (!Detail::IsDepthGtaoCameraValid(camera) || !std::isfinite(radius) || radius <= 0.0f
			|| !std::isfinite(linearDepth) || linearDepth <= 0.0f)
			return std::unexpected(Error(ErrorCode::InvalidArgument, "GTAO radius needs a valid camera and positive finite radius/depth"));
		const double scale = camera.ProjectionKind == RenderProjection::Perspective
			? std::abs(static_cast<double>(camera.Projection[1][1])) / linearDepth
			: 1.0 / camera.OrthographicSize;
		const double pixels = radius * 0.5 * camera.ViewportHeight * scale;
		if (!std::isfinite(pixels) || pixels > std::numeric_limits<float>::max())
			return std::unexpected(Error(ErrorCode::InvalidArgument, "GTAO screen radius is not representable"));
		return static_cast<float>(pixels);
	}

	uint32_t GetGtaoSliceCount(RenderSsaoQuality quality)
	{
		switch (quality)
		{
			case RenderSsaoQuality::Low:    return 1;
			case RenderSsaoQuality::Medium: return 2;
			case RenderSsaoQuality::High:   return 3;
		}
		ENGINE_CORE_ASSERT(false, "Invalid GTAO quality");
		return 1;
	}

	nvrhi::TextureDesc GtaoPass::GetTargetDesc(uint32_t width, uint32_t height, bool halfResolution)
	{
		ENGINE_CORE_ASSERT(width > 0 && height > 0, "GTAO dimensions must be positive");
		nvrhi::TextureDesc desc;
		desc.width = halfResolution ? width / 2 + width % 2 : width;
		desc.height = halfResolution ? height / 2 + height % 2 : height;
		desc.format = nvrhi::Format::R8_UNORM;
		desc.isShaderResource = true;
		desc.isUAV = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.AmbientOcclusion";
		return desc;
	}

	Result<nvrhi::ITexture*> GtaoPass::Record(nvrhi::ICommandList& commandList, RenderRecordingContext& recording,
		PassBindingCache& bindings, const GtaoInputs& inputs)
	{
		const CameraData& camera = inputs.Camera;
		if (!Detail::IsDepthGtaoCameraValid(camera) || !std::isfinite(inputs.Post.SsaoIntensity) || inputs.Post.SsaoIntensity < 0.0f
			|| !std::isfinite(inputs.Post.SsaoRadius) || inputs.Post.SsaoRadius <= 0.0f
			|| std::to_underlying(inputs.Post.SsaoQuality) > std::to_underlying(RenderSsaoQuality::High)
			|| inputs.ViewDepth == nullptr || inputs.SceneNormals == nullptr || inputs.Occlusion == nullptr || inputs.Scratch == nullptr
			|| inputs.Occlusion == inputs.Scratch)
			return std::unexpected(Error(ErrorCode::InvalidArgument, "GTAO needs valid camera/settings and distinct output targets"));
		const uint32_t width = camera.ViewportWidth;
		const uint32_t height = camera.ViewportHeight;
		const bool half = inputs.HalfResolution || inputs.Post.SsaoQuality == RenderSsaoQuality::Low;
		const nvrhi::TextureDesc expected = GetTargetDesc(width, height, half);
		if (!Detail::IsDepthGtaoTarget(inputs.ViewDepth->getDesc(), DepthPyramidPass::GetTargetDesc(width, height))
			|| !Detail::IsDepthGtaoTexture2D(inputs.SceneNormals->getDesc(), width, height, nvrhi::Format::RG16_FLOAT, 1)
			|| !Detail::IsDepthGtaoTarget(inputs.Occlusion->getDesc(), expected)
			|| !Detail::IsDepthGtaoTarget(inputs.Scratch->getDesc(), expected))
			return std::unexpected(Error(ErrorCode::InvalidArgument, "GTAO texture descriptors do not match the camera and resolution setting"));
		if (!inputs.Post.SsaoEnabled)
		{
			commandList.clearTextureFloat(inputs.Occlusion, nvrhi::AllSubresources, nvrhi::Color(1.0f));
			return inputs.Occlusion;
		}

		GtaoConstants constants;
		constants.FullSize = glm::uvec2(width, height);
		constants.OutputSize = glm::uvec2(expected.width, expected.height);
		// Capture targets can have a different aspect from the frozen projection that produced their depths.
		constants.PositionScale = glm::vec2(1.0f / camera.Projection[0][0], 1.0f / camera.Projection[1][1]);
		constants.Radius = inputs.Post.SsaoRadius;
		constants.Intensity = inputs.Post.SsaoIntensity;
		constants.ProjectionKind = std::to_underlying(camera.ProjectionKind);
		constants.SliceCount = GetGtaoSliceCount(inputs.Post.SsaoQuality);
		constants.MipCount = inputs.ViewDepth->getDesc().mipLevels;
		ENGINE_TRY_ASSIGN(constants.RadiusScale, ComputeGtaoScreenRadius(camera, 1.0f, 1.0f));
		if (!std::isfinite(constants.PositionScale.x) || !std::isfinite(constants.PositionScale.y))
			return std::unexpected(Error(ErrorCode::InvalidArgument, "GTAO view position scale is not representable"));

		State& state = *m_State;
		const auto dispatch = [&](uint32_t pipelineIndex, nvrhi::ITexture* source, nvrhi::ITexture* destination) -> Status
		{
			ComputePipeline& pipeline = state.Pipelines[pipelineIndex];
			nvrhi::BindingSetDesc desc;
			desc.bindings = { nvrhi::BindingSetItem::PushConstants(0, sizeof(constants)),
				nvrhi::BindingSetItem::Texture_SRV(0, inputs.ViewDepth),
				nvrhi::BindingSetItem::Texture_SRV(1, inputs.SceneNormals),
				nvrhi::BindingSetItem::Texture_UAV(0, destination, nvrhi::Format::R8_UNORM) };
			if (source != nullptr)
				desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(2, source));
			ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, bindings.GetOrCreate(*state.Device, desc, *pipeline.BindingLayouts.front()));
			nvrhi::ComputeState compute;
			compute.pipeline = pipeline.Pipeline;
			compute.bindings = { set };
			commandList.setComputeState(compute);
			commandList.setPushConstants(&constants, sizeof(constants));
			commandList.dispatch((expected.width + 7) / 8, (expected.height + 7) / 8);
			return {};
		};
		const auto recordStep = [&](std::string_view name, uint32_t pipelineIndex, nvrhi::ITexture* source, nvrhi::ITexture* destination) -> Status
		{
			recording.BeginPass(name, &commandList);
			const Status result = dispatch(pipelineIndex, source, destination);
			recording.EndPass({ .Dispatches = result.has_value() ? 1U : 0U });
			return result;
		};
		ENGINE_TRY(recordStep("GTAO", 0, nullptr, inputs.Occlusion));
		ENGINE_TRY(recordStep("GTAODenoiseHorizontal", 1, inputs.Occlusion, inputs.Scratch));
		constants.Axis = 1;
		ENGINE_TRY(recordStep("GTAODenoiseVertical", 1, inputs.Scratch, inputs.Occlusion));
		return inputs.Occlusion;
	}

}
