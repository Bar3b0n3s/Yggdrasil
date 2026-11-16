#include "EnginePCH.h"
#include "Engine/Renderer/SsrPass.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/FogConstants.h"
#include "Shared/ProbeConstants.h"
#include "Shared/ViewConstants.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Ssr program (Passes/Ssr.slang) runs the three steps of SsrPass.h; its settings are push constants (SsrConstants) and its
// per-view constants the view's buffers, so one pipeline per step serves every view.

namespace Engine {

	namespace Utils {

		constexpr std::string_view SsrProgram = "Ssr";
		// Set 0 of Passes/Ssr.slang (SsrPass::GetLayoutDescriptions): the push constants (NVRHI slot 1: NVRHI refuses push
		// constants and a constant buffer at one slot, and Vulkan ignores the push constants' slot), b0 ViewConstants, b2
		// EnvironmentConstants, b4 ProbeConstants, b5 FogConstants (M32), t0 SceneDepth, t1 SceneNormals, t2 SceneRoughness, t3
		// ViewDepth, t4 SceneColor, t5 SsrColor, t6 the occlusion, t7 its depth, t8 the environment's specular cube, t14 the
		// reflection probe cubes, t15 the tile list (M32), s0 LinearClamp, u0 the traced SsrColor, u1 the tile list and u2 the
		// arguments (M32).
		constexpr uint32_t SsrPushConstantsSlot = 1;
		constexpr uint32_t SsrViewConstantsSlot = 0;
		constexpr uint32_t SsrEnvironmentConstantsSlot = 2;
		constexpr uint32_t SsrProbeConstantsSlot = 4;
		constexpr uint32_t SsrFogConstantsSlot = 5; // M32: the forward view layout's register (Private/ForwardPipelines.h)
		constexpr uint32_t SsrSceneDepthSlot = 0;
		constexpr uint32_t SsrSceneNormalsSlot = 1;
		constexpr uint32_t SsrSceneRoughnessSlot = 2;
		constexpr uint32_t SsrViewDepthSlot = 3;
		constexpr uint32_t SsrSceneColorSlot = 4;
		constexpr uint32_t SsrColorSlot = 5;
		constexpr uint32_t SsrOcclusionSlot = 6;
		constexpr uint32_t SsrOcclusionDepthSlot = 7;
		constexpr uint32_t SsrEnvironmentSpecularSlot = 8;
		constexpr uint32_t SsrReflectionProbeCubesSlot = 14;
		constexpr uint32_t SsrTilesSlot = 15;
		constexpr uint32_t SsrSamplerSlot = 0;
		constexpr uint32_t SsrTracedColorSlot = 0;
		constexpr uint32_t SsrTileListSlot = 1;
		constexpr uint32_t SsrArgumentsSlot = 2;
		// The composite draws each listed tile as two triangles.
		constexpr uint32_t SsrCompositeVerticesPerTile = 6;
		// PostProcessComponent's registered ranges (Scene/Registration/RenderingRegistration.cpp).
		constexpr float SsrMinMaxDistance = 0.1f;
		constexpr float SsrMaxMaxDistance = 1000.0f;
		constexpr float SsrMinThickness = 0.001f;
		constexpr float SsrMaxThickness = 10.0f;

		static nvrhi::BindingLayoutDesc MakeSsrLayout(nvrhi::ShaderType visibility, std::vector<nvrhi::BindingLayoutItem> items)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = visibility;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = std::move(items);
			return layout;
		}

		static PipelineLayoutDescription MakeClassifyDescription()
		{
			std::vector<nvrhi::BindingLayoutItem> items = {
				nvrhi::BindingLayoutItem::PushConstants(SsrPushConstantsSlot, sizeof(SsrConstants)),
				nvrhi::BindingLayoutItem::Texture_SRV(SsrSceneDepthSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(SsrSceneRoughnessSlot),
				nvrhi::BindingLayoutItem::StructuredBuffer_UAV(SsrTileListSlot),
				nvrhi::BindingLayoutItem::RawBuffer_UAV(SsrArgumentsSlot),
			};
			return {
				.Name = "SsrClassify",
				.Program = std::string(SsrProgram),
				.Entries = { "CSClassify" },
				.BindingLayouts = { MakeSsrLayout(nvrhi::ShaderType::Compute, std::move(items)) },
			};
		}

		static PipelineLayoutDescription MakeTraceDescription()
		{
			std::vector<nvrhi::BindingLayoutItem> items = {
				nvrhi::BindingLayoutItem::PushConstants(SsrPushConstantsSlot, sizeof(SsrConstants)),
				nvrhi::BindingLayoutItem::ConstantBuffer(SsrViewConstantsSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(SsrSceneDepthSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(SsrSceneNormalsSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(SsrSceneRoughnessSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(SsrViewDepthSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(SsrSceneColorSlot),
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(SsrTilesSlot),
				nvrhi::BindingLayoutItem::Texture_UAV(SsrTracedColorSlot),
			};
			return {
				.Name = "SsrTrace",
				.Program = std::string(SsrProgram),
				.Entries = { "CSTrace" },
				.BindingLayouts = { MakeSsrLayout(nvrhi::ShaderType::Compute, std::move(items)) },
				.StorageImages = { { .Set = 0, .Register = SsrTracedColorSlot, .Format = SsrColorFormat } },
				.ConstantBuffers = { { .Set = 0, .Register = SsrViewConstantsSlot, .ByteSize = sizeof(ViewConstants) } },
			};
		}

		static PipelineLayoutDescription MakeCompositeDescription()
		{
			std::vector<nvrhi::BindingLayoutItem> items = {
				nvrhi::BindingLayoutItem::PushConstants(SsrPushConstantsSlot, sizeof(SsrConstants)),
				nvrhi::BindingLayoutItem::ConstantBuffer(SsrViewConstantsSlot),
				nvrhi::BindingLayoutItem::ConstantBuffer(SsrEnvironmentConstantsSlot),
				nvrhi::BindingLayoutItem::ConstantBuffer(SsrProbeConstantsSlot),
				nvrhi::BindingLayoutItem::ConstantBuffer(SsrFogConstantsSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(SsrSceneDepthSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(SsrSceneNormalsSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(SsrSceneRoughnessSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(SsrViewDepthSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(SsrColorSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(SsrOcclusionSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(SsrOcclusionDepthSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(SsrEnvironmentSpecularSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(SsrReflectionProbeCubesSlot),
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(SsrTilesSlot),
				nvrhi::BindingLayoutItem::Sampler(SsrSamplerSlot),
			};
			return {
				.Name = "SsrComposite",
				.Program = std::string(SsrProgram),
				.Entries = { "VSComposite", "PSComposite" },
				// Both stages declare the push constants (PipelineFactory.cpp's rule): the vertex stage reads the tile size, the
				// fragment stage the settings.
				.BindingLayouts = { MakeSsrLayout(nvrhi::ShaderType::AllGraphics, std::move(items)) },
				.ConstantBuffers = {
					{ .Set = 0, .Register = SsrViewConstantsSlot, .ByteSize = sizeof(ViewConstants) },
					{ .Set = 0, .Register = SsrEnvironmentConstantsSlot, .ByteSize = sizeof(EnvironmentConstants) },
					{ .Set = 0, .Register = SsrProbeConstantsSlot, .ByteSize = sizeof(ProbeConstants) },
					{ .Set = 0, .Register = SsrFogConstantsSlot, .ByteSize = sizeof(FogConstants) },
				},
			};
		}

		// `value` clamped to [low, high]; `fallback` when it is not finite.
		static float ClampFinite(float value, float low, float high, float fallback)
		{
			return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
		}

		// Whether `texture` is a 2D texture of `width` x `height` at its mip 0.
		static bool HasSize(const nvrhi::ITexture& texture, uint32_t width, uint32_t height)
		{
			const nvrhi::TextureDesc& desc = texture.getDesc();
			return desc.width == width && desc.height == height;
		}

		// The 32-bit word of `bytes` at byte `offset`.
		static uint32_t ReadWord(std::span<const std::byte> bytes, uint32_t offset)
		{
			uint32_t word = 0;
			std::memcpy(&word, bytes.data() + offset, sizeof(word));
			return word;
		}

	}

	SsrConstants MakeSsrConstants(const PostProcessSettings& post, uint32_t width, uint32_t height, uint32_t pyramidLevels, bool halfResolution)
	{
		const PostProcessSettings defaults;
		SsrConstants constants;
		constants.ImageSize = UInt2(width, height);
		constants.InverseImageSize = 1.0f / glm::vec2(static_cast<float>(std::max(width, 1U)), static_cast<float>(std::max(height, 1U)));
		constants.MaxDistance = Utils::ClampFinite(post.SsrMaxDistance, Utils::SsrMinMaxDistance, Utils::SsrMaxMaxDistance, defaults.SsrMaxDistance);
		constants.Thickness = Utils::ClampFinite(post.SsrThickness, Utils::SsrMinThickness, Utils::SsrMaxThickness, defaults.SsrThickness);
		constants.MaxRoughness = Utils::ClampFinite(post.SsrMaxRoughness, 0.0f, 1.0f, 0.0f);
		constants.Intensity = Utils::ClampFinite(post.SsrIntensity, 0.0f, 1.0f, 0.0f);
		constants.PyramidLevels = std::max(pyramidLevels, 1U);
		constants.HalfResolution = halfResolution ? 1U : 0U;
		return constants;
	}

	SsrArea ReadSsrArea(std::span<const std::byte> arguments)
	{
		ENGINE_CORE_ASSERT(arguments.size() >= SsrArgumentsLayout::ByteSize, "ReadSsrArea needs the arguments buffer's whole layout");
		if (arguments.size() < SsrArgumentsLayout::ByteSize)
			return {};
		return {
			.Tiles = Utils::ReadWord(arguments, SsrArgumentsLayout::TilesOffset),
			.TracedTiles = Utils::ReadWord(arguments, SsrArgumentsLayout::CompositeInstancesOffset),
			.TracedPixels = Utils::ReadWord(arguments, SsrArgumentsLayout::TracedPixelsOffset),
			.TraceGroups = Utils::ReadWord(arguments, SsrArgumentsLayout::TraceGroupsOffset),
			.TraceRows = Utils::ReadWord(arguments, SsrArgumentsLayout::TraceRowsOffset),
		};
	}

	struct SsrPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		ComputePipeline Classify{};
		ComputePipeline Trace{};
		GraphicsPipeline Composite{};
		nvrhi::SamplerHandle LinearClamp{};
		uint32_t PipelineCount = 0;
	};

	SsrPass::SsrPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	SsrPass::~SsrPass() = default;

	Result<Scope<SsrPass>> SsrPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<SsrPass> pass = CreateScope<SsrPass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		ENGINE_TRY_ASSIGN(state.Classify, pipelines.CreateComputePipeline({ .Layout = Utils::MakeClassifyDescription() }));
		ENGINE_TRY_ASSIGN(state.Trace, pipelines.CreateComputePipeline({ .Layout = Utils::MakeTraceDescription() }));

		// The composite: the listed tiles add their colour to SceneColor's rgb (One + One; a float target clamps neither operand,
		// so a negative difference subtracts) and keep its alpha; no depth.
		GraphicsPipelineSpecification composite;
		composite.Layout = Utils::MakeCompositeDescription();
		composite.Framebuffer = GetCompositeFramebufferInfo();
		composite.Primitive = nvrhi::PrimitiveType::TriangleList;
		composite.RenderState.rasterState.setCullNone();
		composite.RenderState.rasterState.frontCounterClockwise = true;
		composite.RenderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false).setStencilEnable(false);
		composite.RenderState.blendState.targets[0]
			.enableBlend()
			.setSrcBlend(nvrhi::BlendFactor::One)
			.setDestBlend(nvrhi::BlendFactor::One)
			.setBlendOp(nvrhi::BlendOp::Add)
			.setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
			.setDestBlendAlpha(nvrhi::BlendFactor::One)
			.setBlendOpAlpha(nvrhi::BlendOp::Add)
			.setColorWriteMask(nvrhi::ColorMask::Red | nvrhi::ColorMask::Green | nvrhi::ColorMask::Blue);
		ENGINE_TRY_ASSIGN(state.Composite, pipelines.CreateGraphicsPipeline(composite));
		ENGINE_TRY_ASSIGN(state.LinearClamp,
			device.CreateSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp)));
		state.PipelineCount = PipelineCount;
		return pass;
	}

	std::vector<PipelineLayoutDescription> SsrPass::GetLayoutDescriptions()
	{
		return { Utils::MakeClassifyDescription(), Utils::MakeTraceDescription(), Utils::MakeCompositeDescription() };
	}

	glm::uvec2 SsrPass::GetTraceSize(uint32_t width, uint32_t height, bool halfResolution)
	{
		return halfResolution ? glm::uvec2((width + 1) / 2, (height + 1) / 2) : glm::uvec2(width, height);
	}

	uint32_t SsrPass::GetTileCount(glm::uvec2 traceSize)
	{
		return ((traceSize.x + TileSize - 1) / TileSize) * ((traceSize.y + TileSize - 1) / TileSize);
	}

	glm::uvec2 SsrPass::GetTraceGroups(uint32_t listedTiles)
	{
		return { std::min(listedTiles, SsrTraceRowGroups), listedTiles / SsrTraceRowGroups + (listedTiles % SsrTraceRowGroups != 0 ? 1U : 0U) };
	}

	nvrhi::TextureDesc SsrPass::GetTargetDesc(uint32_t width, uint32_t height)
	{
		ENGINE_CORE_ASSERT(width > 0 && height > 0, "SsrColor's dimensions must be positive");
		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.format = SsrColorFormat;
		desc.dimension = nvrhi::TextureDimension::Texture2D;
		desc.isShaderResource = true;
		desc.isUAV = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.SsrColor";
		return desc;
	}

	nvrhi::BufferDesc SsrPass::GetTileBufferDesc(uint32_t tileCount)
	{
		nvrhi::BufferDesc desc;
		// The listed count, then the tiles.
		desc.byteSize = (static_cast<uint64_t>(std::max(tileCount, 1U)) + 1) * sizeof(uint32_t);
		desc.structStride = sizeof(uint32_t);
		desc.canHaveUAVs = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.SsrTiles";
		return desc;
	}

	nvrhi::BufferDesc SsrPass::GetArgumentsBufferDesc()
	{
		nvrhi::BufferDesc desc;
		desc.byteSize = SsrArgumentsLayout::ByteSize;
		desc.canHaveUAVs = true;
		desc.canHaveRawViews = true;
		desc.isDrawIndirectArgs = true;
		desc.initialState = nvrhi::ResourceStates::IndirectArgument;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.SsrArguments";
		return desc;
	}

	nvrhi::BufferDesc SsrPass::GetAreaReadbackDesc()
	{
		nvrhi::BufferDesc desc;
		desc.byteSize = SsrArgumentsLayout::ByteSize;
		desc.cpuAccess = nvrhi::CpuAccessMode::Read;
		desc.initialState = nvrhi::ResourceStates::CopyDest;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.SsrAreaReadback";
		return desc;
	}

	nvrhi::FramebufferInfo SsrPass::GetCompositeFramebufferInfo()
	{
		nvrhi::FramebufferInfo info;
		info.addColorFormat(SceneColorFormat);
		return info;
	}

	uint32_t SsrPass::GetPipelineCount() const
	{
		return m_State->PipelineCount;
	}

	Status SsrPass::Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SsrPassInputs& inputs, RenderPassCounters& counters)
	{
		if (inputs.ViewConstants == nullptr || inputs.EnvironmentConstants == nullptr || inputs.ProbeConstants == nullptr || inputs.FogConstants == nullptr
			|| inputs.SceneDepth == nullptr || inputs.SceneNormals == nullptr || inputs.SceneRoughness == nullptr || inputs.ViewDepth == nullptr
			|| inputs.SceneColor == nullptr || inputs.Framebuffer == nullptr || inputs.SsrColor == nullptr || inputs.Tiles == nullptr
			|| inputs.Arguments == nullptr || inputs.AmbientOcclusion == nullptr || inputs.LinearViewDepth == nullptr || inputs.EnvironmentSpecular == nullptr
			|| inputs.ReflectionProbeCubes == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "the SSR pass needs the view's constants, its prepass targets, the depth pyramid, SceneColor with its "
														 "framebuffer, SsrColor, the tile list, the arguments and the forward pass's specular inputs");
		const nvrhi::TextureDesc& image = inputs.SceneColor->getDesc();
		const nvrhi::FramebufferDesc& framebuffer = inputs.Framebuffer->getDesc();
		const glm::uvec2 trace = GetTraceSize(image.width, image.height, inputs.HalfResolution);
		const uint32_t tileCount = GetTileCount(trace);
		if (!Utils::HasSize(*inputs.SceneDepth, image.width, image.height) || !Utils::HasSize(*inputs.SceneNormals, image.width, image.height)
			|| !Utils::HasSize(*inputs.SceneRoughness, image.width, image.height) || !Utils::HasSize(*inputs.ViewDepth, image.width, image.height)
			|| !Utils::HasSize(*inputs.SsrColor, trace.x, trace.y) || inputs.Tiles->getDesc().byteSize < (static_cast<uint64_t>(tileCount) + 1) * sizeof(uint32_t)
			|| inputs.Arguments->getDesc().byteSize < SsrArgumentsLayout::ByteSize || framebuffer.colorAttachments.size() != 1
			|| framebuffer.colorAttachments[0].texture != inputs.SceneColor || framebuffer.depthAttachment.valid())
			return MakeError(ErrorCode::InvalidArgument, "the SSR pass's targets must be of SceneColor's size, SsrColor of the trace image's, its tile list and "
														 "arguments of their sizes, and its framebuffer SceneColor alone");
		State& state = *m_State;
		const SsrConstants constants = MakeSsrConstants(inputs.Post, image.width, image.height, inputs.ViewDepth->getDesc().mipLevels, inputs.HalfResolution);

		nvrhi::BindingSetDesc classifyDesc;
		classifyDesc.bindings = {
			nvrhi::BindingSetItem::PushConstants(Utils::SsrPushConstantsSlot, sizeof(SsrConstants)),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SsrSceneDepthSlot, inputs.SceneDepth),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SsrSceneRoughnessSlot, inputs.SceneRoughness),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(Utils::SsrTileListSlot, inputs.Tiles),
			nvrhi::BindingSetItem::RawBuffer_UAV(Utils::SsrArgumentsSlot, inputs.Arguments),
		};
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * classifySet, bindings.GetOrCreate(*state.Device, classifyDesc, *state.Classify.BindingLayouts[0]));
		nvrhi::BindingSetDesc traceDesc;
		traceDesc.bindings = {
			nvrhi::BindingSetItem::PushConstants(Utils::SsrPushConstantsSlot, sizeof(SsrConstants)),
			nvrhi::BindingSetItem::ConstantBuffer(Utils::SsrViewConstantsSlot, inputs.ViewConstants),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SsrSceneDepthSlot, inputs.SceneDepth),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SsrSceneNormalsSlot, inputs.SceneNormals),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SsrSceneRoughnessSlot, inputs.SceneRoughness),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SsrViewDepthSlot, inputs.ViewDepth),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SsrSceneColorSlot, inputs.SceneColor),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(Utils::SsrTilesSlot, inputs.Tiles),
			nvrhi::BindingSetItem::Texture_UAV(Utils::SsrTracedColorSlot, inputs.SsrColor, SsrColorFormat),
		};
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * traceSet, bindings.GetOrCreate(*state.Device, traceDesc, *state.Trace.BindingLayouts[0]));
		nvrhi::BindingSetDesc compositeDesc;
		compositeDesc.bindings = {
			nvrhi::BindingSetItem::PushConstants(Utils::SsrPushConstantsSlot, sizeof(SsrConstants)),
			nvrhi::BindingSetItem::ConstantBuffer(Utils::SsrViewConstantsSlot, inputs.ViewConstants),
			nvrhi::BindingSetItem::ConstantBuffer(Utils::SsrEnvironmentConstantsSlot, inputs.EnvironmentConstants),
			nvrhi::BindingSetItem::ConstantBuffer(Utils::SsrProbeConstantsSlot, inputs.ProbeConstants),
			nvrhi::BindingSetItem::ConstantBuffer(Utils::SsrFogConstantsSlot, inputs.FogConstants),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SsrSceneDepthSlot, inputs.SceneDepth),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SsrSceneNormalsSlot, inputs.SceneNormals),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SsrSceneRoughnessSlot, inputs.SceneRoughness),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SsrViewDepthSlot, inputs.ViewDepth),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SsrColorSlot, inputs.SsrColor),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SsrOcclusionSlot, inputs.AmbientOcclusion),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SsrOcclusionDepthSlot, inputs.LinearViewDepth),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SsrEnvironmentSpecularSlot, inputs.EnvironmentSpecular),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SsrReflectionProbeCubesSlot, inputs.ReflectionProbeCubes),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(Utils::SsrTilesSlot, inputs.Tiles),
			nvrhi::BindingSetItem::Sampler(Utils::SsrSamplerSlot, state.LinearClamp),
		};
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * compositeSet, bindings.GetOrCreate(*state.Device, compositeDesc, *state.Composite.BindingLayouts[0]));

		commandList.beginMarker("SSR");
		// The counts start at 0 and the indirect arguments at their constant words: (0, 0, 1) for the trace (CSClassify raises x
		// and y to its rows), (6, 0, 0, 0) for the composite; then the traced pixels and the tile count. The tile list's count
		// starts at 0.
		const std::array<uint32_t, SsrArgumentsLayout::ByteSize / sizeof(uint32_t)> initial = { 0, 0, 1, Utils::SsrCompositeVerticesPerTile, 0, 0, 0, 0, tileCount };
		commandList.writeBuffer(inputs.Arguments, initial.data(), sizeof(initial));
		constexpr uint32_t NoTiles = 0;
		commandList.writeBuffer(inputs.Tiles, &NoTiles, sizeof(NoTiles));
		// At half resolution the upsample reads the trace image across tile edges, so the unlisted tiles hold 0 (no reflection).
		if (inputs.HalfResolution)
			commandList.clearTextureFloat(inputs.SsrColor, nvrhi::AllSubresources, nvrhi::Color(0.0f));

		nvrhi::ComputeState classify;
		classify.pipeline = state.Classify.Pipeline;
		classify.bindings = { classifySet };
		commandList.setComputeState(classify);
		commandList.setPushConstants(&constants, sizeof(constants));
		commandList.dispatch((trace.x + TileSize - 1) / TileSize, (trace.y + TileSize - 1) / TileSize);

		nvrhi::ComputeState traced;
		traced.pipeline = state.Trace.Pipeline;
		traced.bindings = { traceSet };
		traced.indirectParams = inputs.Arguments;
		commandList.setComputeState(traced);
		commandList.setPushConstants(&constants, sizeof(constants));
		commandList.dispatchIndirect(SsrArgumentsLayout::TraceGroupsOffset);

		nvrhi::GraphicsState graphics;
		graphics.pipeline = state.Composite.Pipeline;
		graphics.framebuffer = inputs.Framebuffer;
		graphics.viewport.addViewportAndScissorRect(inputs.Framebuffer->getFramebufferInfo().getViewport());
		graphics.bindings = { compositeSet };
		graphics.indirectParams = inputs.Arguments;
		commandList.setGraphicsState(graphics);
		commandList.setPushConstants(&constants, sizeof(constants));
		commandList.drawIndirect(SsrArgumentsLayout::CompositeDrawOffset);
		commandList.endMarker();
		counters.Dispatches += 2;
		counters.DrawCalls += 1;
		return {};
	}

}
