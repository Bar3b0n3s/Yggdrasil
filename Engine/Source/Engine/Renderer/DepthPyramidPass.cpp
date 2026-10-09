#include "EnginePCH.h"
#include "Engine/Renderer/DepthPyramidPass.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/Private/DepthGtaoValidation.h"
#include "Shared/DepthPyramidConstants.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace Engine {

	namespace Detail {

		bool IsDepthGtaoCameraValid(const CameraData& camera)
		{
			if (camera.ViewportWidth == 0 || camera.ViewportHeight == 0
				|| !std::isfinite(camera.NearClip) || camera.NearClip <= 0.0f
				|| !std::isfinite(camera.FarClip) || camera.FarClip <= camera.NearClip)
				return false;
			for (int column = 0; column < 4; ++column)
			{
				for (int row = 0; row < 4; ++row)
				{
					if (!std::isfinite(camera.Projection[column][row]) || !std::isfinite(camera.View[column][row]))
						return false;
				}
			}
			if (camera.Projection[0][0] <= 0.0f || camera.Projection[1][1] <= 0.0f)
				return false;
			if (camera.ProjectionKind == RenderProjection::Perspective)
				return std::isfinite(camera.VerticalFov) && camera.VerticalFov > 0.0f && camera.VerticalFov < 180.0f;
			return camera.ProjectionKind == RenderProjection::Orthographic
				&& std::isfinite(camera.OrthographicSize) && camera.OrthographicSize > 0.0f;
		}

		bool IsDepthGtaoTexture2D(const nvrhi::TextureDesc& desc, uint32_t width, uint32_t height,
			nvrhi::Format format, uint32_t mipCount)
		{
			return desc.dimension == nvrhi::TextureDimension::Texture2D && desc.width == width && desc.height == height
				&& desc.depth == 1 && desc.arraySize == 1 && desc.sampleCount == 1 && desc.format == format
				&& desc.mipLevels == mipCount && desc.isShaderResource;
		}

		bool IsDepthGtaoTarget(const nvrhi::TextureDesc& desc, const nvrhi::TextureDesc& expected)
		{
			return IsDepthGtaoTexture2D(desc, expected.width, expected.height, expected.format, expected.mipLevels)
				&& desc.isUAV && desc.keepInitialState && desc.initialState == nvrhi::ResourceStates::ShaderResource;
		}

	}

	struct DepthPyramidPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		std::vector<ComputePipeline> Pipelines{};
	};

	DepthPyramidPass::DepthPyramidPass(ConstructionKey)
		: m_State(CreateScope<State>())
	{
	}

	DepthPyramidPass::~DepthPyramidPass() = default;

	Result<Scope<DepthPyramidPass>> DepthPyramidPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<DepthPyramidPass> pass = CreateScope<DepthPyramidPass>(ConstructionKey());
		pass->m_State->Device = &device;
		for (PipelineLayoutDescription& description : GetLayoutDescriptions())
		{
			ENGINE_TRY_ASSIGN(ComputePipeline pipeline, pipelines.CreateComputePipeline({ .Layout = std::move(description) }));
			pass->m_State->Pipelines.push_back(std::move(pipeline));
		}
		return pass;
	}

	std::vector<PipelineLayoutDescription> DepthPyramidPass::GetLayoutDescriptions()
	{
		std::vector<PipelineLayoutDescription> descriptions;
		for (const std::string entry : { "CSLinearize", "CSReduce" })
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = { nvrhi::BindingLayoutItem::PushConstants(0, sizeof(DepthPyramidConstants)),
				nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_UAV(0) };
			descriptions.push_back({ .Name = "DepthPyramid." + entry, .Program = "DepthPyramid", .Entries = { entry }, .BindingLayouts = { layout }, .StorageImages = { { .Set = 0, .Register = 0, .Format = nvrhi::Format::R16_FLOAT } } });
		}
		return descriptions;
	}

	float ReconstructLinearViewDepth(const CameraData& camera, float reverseDepth)
	{
		ENGINE_CORE_ASSERT(Detail::IsDepthGtaoCameraValid(camera), "Depth reconstruction needs a valid camera");
		ENGINE_CORE_ASSERT(std::isfinite(reverseDepth) && reverseDepth >= 0.0f && reverseDepth <= 1.0f, "Reverse depth must be in [0,1]");
		if (reverseDepth == 0.0f)
			return 65504.0f;
		const double depth = camera.ProjectionKind == RenderProjection::Perspective
			? static_cast<double>(camera.NearClip) / reverseDepth
			: static_cast<double>(camera.FarClip) - reverseDepth * (static_cast<double>(camera.FarClip) - camera.NearClip);
		return static_cast<float>(std::clamp(depth, 0.0, 65504.0));
	}

	glm::vec3 ReconstructViewPosition(const CameraData& camera, const glm::vec2& uv, float linearDepth)
	{
		ENGINE_CORE_ASSERT(Detail::IsDepthGtaoCameraValid(camera), "View reconstruction needs a valid camera");
		ENGINE_CORE_ASSERT(std::isfinite(uv.x) && std::isfinite(uv.y) && uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f
				&& std::isfinite(linearDepth) && linearDepth > 0.0f,
			"View reconstruction needs finite coordinates and positive depth");
		const glm::vec2 ndc(2.0f * uv.x - 1.0f, 1.0f - 2.0f * uv.y);
		if (camera.ProjectionKind == RenderProjection::Perspective)
			return { ndc.x * linearDepth / camera.Projection[0][0], ndc.y * linearDepth / camera.Projection[1][1], -linearDepth };
		// Captures may change the destination extent while preserving the camera's projection.
		return { ndc.x / camera.Projection[0][0], ndc.y / camera.Projection[1][1], -linearDepth };
	}

	nvrhi::TextureDesc DepthPyramidPass::GetTargetDesc(uint32_t width, uint32_t height)
	{
		ENGINE_CORE_ASSERT(width > 0 && height > 0, "Depth pyramid dimensions must be positive");
		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.mipLevels = 1;
		for (uint32_t extent = std::max(width, height); extent > 1 && desc.mipLevels < MaxMipCount; extent /= 2)
			++desc.mipLevels;
		desc.format = nvrhi::Format::R16_FLOAT;
		desc.isShaderResource = true;
		desc.isUAV = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.ViewDepth";
		return desc;
	}

	Status DepthPyramidPass::Record(nvrhi::ICommandList& commandList, RenderRecordingContext& recording,
		PassBindingCache& bindings, const DepthPyramidInputs& inputs)
	{
		if (!Detail::IsDepthGtaoCameraValid(inputs.Camera) || inputs.SceneDepth == nullptr || inputs.ViewDepth == nullptr
			|| inputs.SceneDepth == inputs.ViewDepth)
			return std::unexpected(Error(ErrorCode::InvalidArgument, "Depth pyramid needs a valid camera and distinct depth targets"));
		const uint32_t width = inputs.Camera.ViewportWidth;
		const uint32_t height = inputs.Camera.ViewportHeight;
		const nvrhi::TextureDesc expected = GetTargetDesc(width, height);
		if (!Detail::IsDepthGtaoTexture2D(inputs.SceneDepth->getDesc(), width, height, nvrhi::Format::D32, 1)
			|| !Detail::IsDepthGtaoTarget(inputs.ViewDepth->getDesc(), expected))
			return std::unexpected(Error(ErrorCode::InvalidArgument, "Depth pyramid texture descriptors do not match the camera"));

		State& state = *m_State;
		RenderPassCounters counters;
		recording.BeginPass("DepthPyramid", &commandList);
		const auto recordMips = [&]() -> Status
		{
			for (uint32_t mip = 0; mip < expected.mipLevels; ++mip)
			{
				const uint32_t sourceMip = mip == 0 ? 0 : mip - 1;
				DepthPyramidConstants constants;
				constants.SourceSize = glm::uvec2(std::max(1U, width >> sourceMip), std::max(1U, height >> sourceMip));
				constants.DestinationSize = glm::uvec2(std::max(1U, width >> mip), std::max(1U, height >> mip));
				constants.Near = inputs.Camera.NearClip;
				constants.Far = inputs.Camera.FarClip;
				constants.ProjectionKind = std::to_underlying(inputs.Camera.ProjectionKind);
				ComputePipeline& pipeline = state.Pipelines[mip == 0 ? 0 : 1];
				nvrhi::BindingSetDesc desc;
				desc.bindings = {
					nvrhi::BindingSetItem::PushConstants(0, sizeof(constants)),
					nvrhi::BindingSetItem::Texture_SRV(0, mip == 0 ? inputs.SceneDepth : inputs.ViewDepth,
						nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(sourceMip, 1, 0, 1)),
					nvrhi::BindingSetItem::Texture_UAV(0, inputs.ViewDepth, nvrhi::Format::R16_FLOAT, nvrhi::TextureSubresourceSet(mip, 1, 0, 1)),
				};
				ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, bindings.GetOrCreate(*state.Device, desc, *pipeline.BindingLayouts.front()));
				nvrhi::ComputeState compute;
				compute.pipeline = pipeline.Pipeline;
				compute.bindings = { set };
				commandList.setComputeState(compute);
				commandList.setPushConstants(&constants, sizeof(constants));
				commandList.dispatch((constants.DestinationSize.x + 7) / 8, (constants.DestinationSize.y + 7) / 8);
				++counters.Dispatches;
			}
			return {};
		};
		const Status result = recordMips();
		recording.EndPass(counters);
		return result;
	}

	Result<DepthReductionFootprint> ComputeDepthReductionFootprint(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t x, uint32_t y)
	{
		const uint64_t width = std::max(1U, sourceWidth / 2);
		const uint64_t height = std::max(1U, sourceHeight / 2);
		if (sourceWidth == 0 || sourceHeight == 0 || x >= width || y >= height)
			return std::unexpected(Error(ErrorCode::InvalidArgument, "Invalid depth-reduction dimensions or destination coordinate"));
		return DepthReductionFootprint{
			.MinX = static_cast<uint32_t>(x * static_cast<uint64_t>(sourceWidth) / width),
			.MinY = static_cast<uint32_t>(y * static_cast<uint64_t>(sourceHeight) / height),
			.MaxXExclusive = static_cast<uint32_t>(((static_cast<uint64_t>(x) + 1) * sourceWidth + width - 1) / width),
			.MaxYExclusive = static_cast<uint32_t>(((static_cast<uint64_t>(y) + 1) * sourceHeight + height - 1) / height),
		};
	}

}
