#include "EnginePCH.h"
#include "Engine/Renderer/UpscalePass.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/Private/UpscaleConstants.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

// Both pipelines share one binding layout (the source texture and the push constants), so one binding set per source
// serves either. The colour draw and the depth draw use the same viewport (the scaled scene rectangle, partly outside the
// view for a cropped scene) and the same scissor (that rectangle clipped to the view, never a negative offset).

namespace Engine {

	namespace Utils {

		constexpr std::string_view UpscaleProgram = "Upscale";
		// Indices of the two pipelines (GetLayoutDescriptions' order).
		constexpr size_t UpscaleColorPipeline = 0;
		constexpr size_t UpscaleDepthPipeline = 1;

		static PipelineLayoutDescription MakeUpscaleDescription(std::string name, std::string fragmentEntry)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::All;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {
				nvrhi::BindingLayoutItem::Texture_SRV(0),
				nvrhi::BindingLayoutItem::PushConstants(0, sizeof(UpscaleConstants)),
			};
			return {
				.Name = std::move(name),
				.Program = std::string(UpscaleProgram),
				.Entries = { "VSMain", std::move(fragmentEntry) },
				.BindingLayouts = { layout },
			};
		}

		static std::vector<PipelineLayoutDescription> MakeUpscaleDescriptions()
		{
			return { MakeUpscaleDescription("UpscaleColor", "PSColor"), MakeUpscaleDescription("UpscaleDepth", "PSDepth") };
		}

		// The colour pipeline writes OutputColor without blending or depth; the depth pipeline writes every fragment's depth
		// (the test always passes; Vulkan writes depth only with the test enabled).
		static GraphicsPipelineSpecification MakeUpscaleSpecification(PipelineLayoutDescription layout, bool depth,
			const std::vector<nvrhi::BindingLayoutHandle>& sharedLayouts)
		{
			GraphicsPipelineSpecification specification;
			specification.Layout = std::move(layout);
			specification.SharedBindingLayouts = sharedLayouts;
			specification.Primitive = nvrhi::PrimitiveType::TriangleList;
			specification.RenderState.rasterState.setCullNone().enableDepthClip();
			specification.RenderState.depthStencilState.setDepthTestEnable(depth)
				.setDepthWriteEnable(depth)
				.setDepthFunc(nvrhi::ComparisonFunc::Always)
				.setStencilEnable(false);
			specification.Framebuffer = depth ? UpscalePass::GetDepthFramebufferInfo() : UpscalePass::GetColorFramebufferInfo();
			return specification;
		}

		// The sRGB OETF (IEC 61966-2-1) of a linear value in [0, 1], as Common/Color.slang's LinearToSrgb: the letterbox is
		// a linear colour and OutputColor holds display-encoded values.
		static float EncodeLetterboxChannel(float linear)
		{
			const float clamped = std::isfinite(linear) ? std::clamp(linear, 0.0f, 1.0f) : 0.0f;
			return clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
		}

		// The scaled scene rectangle of `mapping` in view pixels, [MinX, MaxX) x [MinY, MaxY), partly outside the view for a
		// cropped scene.
		struct ScaledSceneRectangle
		{
			int64_t MinX = 0;
			int64_t MinY = 0;
			int64_t MaxX = 0;
			int64_t MaxY = 0;
		};

		static ScaledSceneRectangle GetScaledSceneRectangle(const RenderOutputMapping& mapping)
		{
			ScaledSceneRectangle rectangle;
			rectangle.MinX = mapping.OffsetX;
			rectangle.MinY = mapping.OffsetY;
			rectangle.MaxX = rectangle.MinX + static_cast<int64_t>(mapping.Scale) * mapping.SceneWidth;
			rectangle.MaxY = rectangle.MinY + static_cast<int64_t>(mapping.Scale) * mapping.SceneHeight;
			return rectangle;
		}

		// The scaled scene rectangle of `mapping` intersected with the view, [0, ViewWidth) x [0, ViewHeight): never a
		// negative offset.
		static nvrhi::Rect GetClippedSceneRectangle(const RenderOutputMapping& mapping)
		{
			const ScaledSceneRectangle rectangle = GetScaledSceneRectangle(mapping);
			return nvrhi::Rect(static_cast<int>(std::clamp<int64_t>(rectangle.MinX, 0, mapping.ViewWidth)),
				static_cast<int>(std::clamp<int64_t>(rectangle.MaxX, 0, mapping.ViewWidth)), static_cast<int>(std::clamp<int64_t>(rectangle.MinY, 0, mapping.ViewHeight)),
				static_cast<int>(std::clamp<int64_t>(rectangle.MaxY, 0, mapping.ViewHeight)));
		}

		// The viewport (the scaled scene rectangle) and the scissor (that rectangle clipped to the view) of `mapping`.
		static nvrhi::ViewportState MakeUpscaleViewport(const RenderOutputMapping& mapping)
		{
			const ScaledSceneRectangle rectangle = GetScaledSceneRectangle(mapping);
			nvrhi::ViewportState viewport;
			viewport.addViewport(nvrhi::Viewport(static_cast<float>(rectangle.MinX), static_cast<float>(rectangle.MaxX), static_cast<float>(rectangle.MinY),
				static_cast<float>(rectangle.MaxY), 0.0f, 1.0f));
			viewport.addScissorRect(GetClippedSceneRectangle(mapping));
			return viewport;
		}

		// The letterbox bars of `mapping`, the view minus its clipped scene rectangle: the full-width rows above and below
		// it, then the columns left and right of it between those rows; empty bars are left out.
		static std::vector<nvrhi::Rect> GetLetterboxBars(const RenderOutputMapping& mapping)
		{
			const nvrhi::Rect scene = GetClippedSceneRectangle(mapping);
			const int width = static_cast<int>(mapping.ViewWidth);
			const int height = static_cast<int>(mapping.ViewHeight);
			const std::array<nvrhi::Rect, 4> bars = {
				nvrhi::Rect(0, width, 0, scene.minY),
				nvrhi::Rect(0, width, scene.maxY, height),
				nvrhi::Rect(0, scene.minX, scene.minY, scene.maxY),
				nvrhi::Rect(scene.maxX, width, scene.minY, scene.maxY),
			};
			std::vector<nvrhi::Rect> result;
			for (const nvrhi::Rect& bar : bars)
			{
				if (bar.maxX > bar.minX && bar.maxY > bar.minY)
					result.push_back(bar);
			}
			return result;
		}

	}

	struct UpscalePass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		std::array<GraphicsPipeline, PipelineCount> Pipelines{};
	};

	UpscalePass::UpscalePass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	UpscalePass::~UpscalePass() = default;

	Result<Scope<UpscalePass>> UpscalePass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<UpscalePass> pass = CreateScope<UpscalePass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		std::vector<PipelineLayoutDescription> descriptions = Utils::MakeUpscaleDescriptions();
		ENGINE_TRY_ASSIGN(const std::vector<nvrhi::BindingLayoutHandle> sharedLayouts,
			WithContext(pipelines.CreateBindingLayouts(descriptions[Utils::UpscaleColorPipeline]), "while creating the upscale pipelines"));
		for (size_t index = 0; index < descriptions.size(); ++index)
		{
			ENGINE_TRY_ASSIGN(state.Pipelines[index],
				WithContext(pipelines.CreateGraphicsPipeline(
								Utils::MakeUpscaleSpecification(std::move(descriptions[index]), index == Utils::UpscaleDepthPipeline, sharedLayouts)),
					"while creating the upscale pipelines"));
		}
		return pass;
	}

	std::vector<PipelineLayoutDescription> UpscalePass::GetLayoutDescriptions()
	{
		return Utils::MakeUpscaleDescriptions();
	}

	nvrhi::FramebufferInfo UpscalePass::GetColorFramebufferInfo()
	{
		nvrhi::FramebufferInfo info;
		info.addColorFormat(LdrColorFormat);
		return info;
	}

	nvrhi::FramebufferInfo UpscalePass::GetDepthFramebufferInfo()
	{
		nvrhi::FramebufferInfo info;
		info.setDepthFormat(SceneDepthFormat);
		return info;
	}

	uint32_t UpscalePass::GetPipelineCount() const
	{
		return static_cast<uint32_t>(std::ranges::count_if(m_State->Pipelines, [](const GraphicsPipeline& pipeline)
		{
			return pipeline.Pipeline != nullptr;
		}));
	}

	Status UpscalePass::Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const UpscalePassInputs& inputs, RenderPassCounters& counters)
	{
		const RenderOutputMapping& mapping = inputs.Mapping;
		ENGINE_CORE_ASSERT(inputs.Source != nullptr && inputs.Color != nullptr, "UpscalePass::Record needs a source and a colour framebuffer");
		ENGINE_CORE_ASSERT((inputs.SourceDepth == nullptr) == (inputs.Depth == nullptr), "UpscalePass::Record takes a depth source and a depth framebuffer together");
		ENGINE_CORE_ASSERT(!mapping.IsIdentity() && mapping.Scale >= 1, "UpscalePass::Record needs a mapping other than the identity");
		ENGINE_CORE_ASSERT(inputs.Source == nullptr
				|| (inputs.Source->getDesc().width == mapping.SceneWidth && inputs.Source->getDesc().height == mapping.SceneHeight),
			"the upscale's source must have the mapping's scene size");
		ENGINE_CORE_ASSERT(inputs.Color == nullptr
				|| (inputs.Color->getFramebufferInfo().width == mapping.ViewWidth && inputs.Color->getFramebufferInfo().height == mapping.ViewHeight),
			"the upscale's colour target must have the mapping's view size");
		if (inputs.Source == nullptr || inputs.Color == nullptr || (inputs.SourceDepth == nullptr) != (inputs.Depth == nullptr) || mapping.Scale == 0)
			return MakeError(ErrorCode::InvalidArgument, "the upscale needs a source, a colour target and a mapping with a scale of at least 1");
		State& state = *m_State;
		nvrhi::IBindingLayout& layout = *state.Pipelines[Utils::UpscaleColorPipeline].BindingLayouts[0];
		const auto bindingSet = [&state, &bindings, &layout](nvrhi::ITexture* source) -> Result<nvrhi::IBindingSet*>
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::Texture_SRV(0, source),
				nvrhi::BindingSetItem::PushConstants(0, sizeof(UpscaleConstants)),
			};
			return WithContext(bindings.GetOrCreate(*state.Device, desc, layout), "while recording the upscale");
		};
		// Everything that can fail, before the first command.
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * colorSet, bindingSet(inputs.Source));
		nvrhi::IBindingSet* depthSet = nullptr;
		if (inputs.SourceDepth != nullptr)
		{
			ENGINE_TRY_ASSIGN(depthSet, bindingSet(inputs.SourceDepth));
		}

		const UpscaleConstants constants{ .Mapping = glm::uvec4(std::bit_cast<uint32_t>(mapping.OffsetX), std::bit_cast<uint32_t>(mapping.OffsetY), mapping.Scale, 0U) };
		const nvrhi::ViewportState viewport = Utils::MakeUpscaleViewport(mapping);
		const auto draw = [&commandList, &state, &viewport, &constants, &counters](size_t pipeline, nvrhi::IFramebuffer* framebuffer, nvrhi::IBindingSet* set)
		{
			nvrhi::GraphicsState graphics;
			graphics.pipeline = state.Pipelines[pipeline].Pipeline;
			graphics.framebuffer = framebuffer;
			graphics.viewport = viewport;
			graphics.bindings = { set };
			commandList.setGraphicsState(graphics);
			commandList.setPushConstants(&constants, sizeof(constants));
			commandList.draw(nvrhi::DrawArguments().setVertexCount(3));
			++counters.DrawCalls;
			++counters.Triangles;
		};

		commandList.beginMarker("Upscale");
		const glm::vec3 letterbox = inputs.LetterboxColor;
		nvrhi::ITexture* color = inputs.Color->getDesc().colorAttachments[0].texture;
		commandList.clearTextureFloat(color, nvrhi::AllSubresources,
			nvrhi::Color(Utils::EncodeLetterboxChannel(letterbox.r), Utils::EncodeLetterboxChannel(letterbox.g), Utils::EncodeLetterboxChannel(letterbox.b), 1.0f));
		draw(Utils::UpscaleColorPipeline, inputs.Color, colorSet);
		if (depthSet != nullptr)
		{
			// The letterbox is at the far plane (reverse-Z), where no scene pixel covers the view.
			commandList.clearDepthStencilTexture(inputs.Depth->getDesc().depthAttachment.texture, nvrhi::AllSubresources, true, 0.0f, false, 0);
			draw(Utils::UpscaleDepthPipeline, inputs.Depth, depthSet);
		}
		commandList.endMarker();
		return {};
	}

	bool UpscalePass::HasLetterbox(const RenderOutputMapping& mapping)
	{
		return !Utils::GetLetterboxBars(mapping).empty();
	}

	Status UpscalePass::RecordLetterbox(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const UpscaleLetterboxInputs& inputs,
		RenderPassCounters& counters)
	{
		const RenderOutputMapping& mapping = inputs.Mapping;
		ENGINE_CORE_ASSERT(inputs.Texel != nullptr && inputs.Color != nullptr, "UpscalePass::RecordLetterbox needs a texel and a colour framebuffer");
		ENGINE_CORE_ASSERT(!mapping.IsIdentity() && mapping.Scale >= 1, "UpscalePass::RecordLetterbox needs a mapping other than the identity");
		ENGINE_CORE_ASSERT(inputs.Texel == nullptr || (inputs.Texel->getDesc().width == 1 && inputs.Texel->getDesc().height == 1),
			"the letterbox texel must be 1 x 1");
		ENGINE_CORE_ASSERT(inputs.Color == nullptr
				|| (inputs.Color->getFramebufferInfo().width == mapping.ViewWidth && inputs.Color->getFramebufferInfo().height == mapping.ViewHeight),
			"the letterbox repaint's colour target must have the mapping's view size");
		if (inputs.Texel == nullptr || inputs.Color == nullptr || mapping.Scale == 0)
			return MakeError(ErrorCode::InvalidArgument, "the letterbox repaint needs a texel, a colour target and a mapping with a scale of at least 1");
		const std::vector<nvrhi::Rect> bars = Utils::GetLetterboxBars(mapping);
		if (bars.empty())
			return {};
		State& state = *m_State;
		nvrhi::IBindingLayout& layout = *state.Pipelines[Utils::UpscaleColorPipeline].BindingLayouts[0];
		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::Texture_SRV(0, inputs.Texel),
			nvrhi::BindingSetItem::PushConstants(0, sizeof(UpscaleConstants)),
		};
		// Everything that can fail, before the first command.
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, WithContext(bindings.GetOrCreate(*state.Device, desc, layout), "while recording the letterbox"));

		// The one texel upscaled over the whole view: floor((pixel - 0) / scale) is texel 0 for every view pixel.
		const uint32_t scale = std::max({ mapping.ViewWidth, mapping.ViewHeight, 1U });
		const UpscaleConstants constants{ .Mapping = glm::uvec4(0U, 0U, scale, 0U) };
		commandList.beginMarker("Letterbox");
		const glm::vec3 letterbox = inputs.LetterboxColor;
		commandList.clearTextureFloat(inputs.Texel, nvrhi::AllSubresources,
			nvrhi::Color(Utils::EncodeLetterboxChannel(letterbox.r), Utils::EncodeLetterboxChannel(letterbox.g), Utils::EncodeLetterboxChannel(letterbox.b), 1.0f));
		for (const nvrhi::Rect& bar : bars)
		{
			nvrhi::GraphicsState graphics;
			graphics.pipeline = state.Pipelines[Utils::UpscaleColorPipeline].Pipeline;
			graphics.framebuffer = inputs.Color;
			graphics.viewport.addViewport(nvrhi::Viewport(static_cast<float>(mapping.ViewWidth), static_cast<float>(mapping.ViewHeight)));
			graphics.viewport.addScissorRect(bar);
			graphics.bindings = { set };
			commandList.setGraphicsState(graphics);
			commandList.setPushConstants(&constants, sizeof(constants));
			commandList.draw(nvrhi::DrawArguments().setVertexCount(3));
			++counters.DrawCalls;
			++counters.Triangles;
		}
		commandList.endMarker();
		return {};
	}

}
