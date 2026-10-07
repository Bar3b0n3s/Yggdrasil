#include "EnginePCH.h"
#include "Engine/Renderer/TrianglePass.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"

#include <glm/matrix.hpp>
#include <nvrhi/utils.h>

namespace Engine {

	namespace Utils {

		// The fixed camera of the triangle view (TrianglePass.h).
		static constexpr glm::vec3 TriangleCameraPosition = { 0.0f, 0.0f, 5.0f };
		static constexpr float TriangleNear = 0.1f;
		static constexpr float TriangleFar = 100.0f;
		static constexpr float TriangleHalfHeight = 1.0f;

		// Reverse-Z orthographic projection (§8.3): d = (far + zView) / (far - near), 1 at the near plane and 0 at the far
		// plane. Clip-space +Y is up: NVRHI's Vulkan backend sets every viewport with a negative height
		// (VKViewportWithDXCoords in vulkan-graphics.cpp), which is the Vulkan Y flip, so a projection that flipped Y as
		// well would render upside down and turn counter-clockwise front faces into back faces. glm matrices are
		// column-major: m[column][row].
		static glm::mat4 MakeReverseZOrthographic(glm::vec2 halfExtents, float nearPlane, float farPlane)
		{
			glm::mat4 projection(0.0f);
			projection[0][0] = 1.0f / halfExtents.x;
			projection[1][1] = 1.0f / halfExtents.y;
			projection[2][2] = 1.0f / (farPlane - nearPlane);
			projection[3][2] = farPlane / (farPlane - nearPlane);
			projection[3][3] = 1.0f;
			return projection;
		}

	}

	TrianglePass::TrianglePass(ConstructionKey /*key*/)
	{
	}

	TrianglePass::~TrianglePass() = default;

	Result<Scope<TrianglePass>> TrianglePass::Create(GraphicsDevice& device, PipelineFactory& pipelines,
		const TrianglePassSpecification& specification)
	{
		const bool hasDepth = specification.Framebuffer.depthFormat != nvrhi::Format::UNKNOWN;
		GraphicsPipelineSpecification pipelineSpecification;
		pipelineSpecification.Layout = GetLayoutDescription();
		pipelineSpecification.RenderState.rasterState.setCullMode(specification.CullMode).setFrontCounterClockwise(true);
		pipelineSpecification.RenderState.depthStencilState.setDepthTestEnable(hasDepth)
			.setDepthWriteEnable(hasDepth)
			.setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual)
			.setStencilEnable(false);
		pipelineSpecification.Framebuffer = specification.Framebuffer;
		ENGINE_TRY_ASSIGN(GraphicsPipeline pipeline, pipelines.CreateGraphicsPipeline(pipelineSpecification));

		nvrhi::BufferDesc constantsDesc;
		constantsDesc.byteSize = sizeof(ViewConstants);
		constantsDesc.isConstantBuffer = true;
		constantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
		constantsDesc.keepInitialState = true;
		constantsDesc.debugName = "TrianglePass.ViewConstants";
		ENGINE_TRY_ASSIGN(nvrhi::BufferHandle constants, device.CreateBuffer(constantsDesc));

		nvrhi::BindingSetDesc bindingSetDesc;
		bindingSetDesc.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, constants) };
		ENGINE_TRY_ASSIGN(nvrhi::BindingSetHandle bindingSet, device.CreateBindingSet(bindingSetDesc, *pipeline.BindingLayouts[0]));

		Scope<TrianglePass> pass = CreateScope<TrianglePass>(ConstructionKey());
		pass->m_Specification = specification;
		pass->m_Pipeline = std::move(pipeline);
		pass->m_ViewConstantsBuffer = std::move(constants);
		pass->m_BindingSet = std::move(bindingSet);
		return pass;
	}

	void TrianglePass::Render(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer)
	{
		const nvrhi::FramebufferInfoEx& info = framebuffer.getFramebufferInfo();
		ENGINE_CORE_ASSERT(static_cast<const nvrhi::FramebufferInfo&>(info) == m_Specification.Framebuffer,
			"TrianglePass: the framebuffer does not match the FramebufferInfo the pipeline was created for");

		commandList.beginMarker("Triangle");
		const nvrhi::Color clearColor(TriangleClearColor[0], TriangleClearColor[1], TriangleClearColor[2], TriangleClearColor[3]);
		nvrhi::utils::ClearColorAttachment(&commandList, &framebuffer, 0, clearColor);
		if (m_Specification.Framebuffer.depthFormat != nvrhi::Format::UNKNOWN)
			nvrhi::utils::ClearDepthStencilAttachment(&commandList, &framebuffer, 0.0f, 0);

		const ViewConstants view = MakeViewConstants(info.width, info.height);
		commandList.writeBuffer(m_ViewConstantsBuffer, &view, sizeof(view));

		nvrhi::GraphicsState state;
		state.pipeline = m_Pipeline.Pipeline;
		state.framebuffer = &framebuffer;
		state.bindings = { m_BindingSet };
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(info.width), static_cast<float>(info.height)));
		commandList.setGraphicsState(state);
		commandList.draw(nvrhi::DrawArguments().setVertexCount(3));
		commandList.endMarker();
	}

	PipelineLayoutDescription TrianglePass::GetLayoutDescription()
	{
		nvrhi::BindingLayoutDesc layout;
		layout.visibility = nvrhi::ShaderType::Vertex;
		layout.registerSpace = 0;
		layout.registerSpaceIsDescriptorSet = true;
		layout.bindings = { nvrhi::BindingLayoutItem::ConstantBuffer(0) };
		return {
			.Name = "Triangle",
			.Program = "Triangle",
			.Entries = { "VSMain", "PSMain" },
			.BindingLayouts = { layout },
			.ConstantBuffers = { { .Set = 0, .Register = 0, .ByteSize = sizeof(ViewConstants) } },
		};
	}

	ViewConstants TrianglePass::MakeViewConstants(uint32_t width, uint32_t height)
	{
		ENGINE_CORE_ASSERT(width > 0 && height > 0, "TrianglePass::MakeViewConstants needs a non-empty target ({}x{})", width, height);
		const glm::vec2 viewportSize(static_cast<float>(width), static_cast<float>(height));
		const float aspectRatio = viewportSize.x / viewportSize.y;
		const glm::vec2 halfExtents(aspectRatio * Utils::TriangleHalfHeight, Utils::TriangleHalfHeight);

		ViewConstants view{};
		// The camera looks down -Z with +Y up, so its view matrix is a translation.
		view.View = glm::mat4(1.0f);
		view.View[3] = glm::vec4(-Utils::TriangleCameraPosition, 1.0f);
		view.Projection = Utils::MakeReverseZOrthographic(halfExtents, Utils::TriangleNear, Utils::TriangleFar);
		view.ViewProjection = view.Projection * view.View;
		view.InverseProjection = glm::inverse(view.Projection);
		view.InverseViewProjection = glm::inverse(view.ViewProjection);
		view.CameraPosition = Utils::TriangleCameraPosition;
		view.ProjectionKind = ProjectionKindOrthographic;
		view.OrthoHalfExtents = halfExtents;
		view.Near = Utils::TriangleNear;
		view.Far = Utils::TriangleFar;
		view.ViewportSize = viewportSize;
		view.InverseViewportSize = 1.0f / viewportSize;
		view.TanHalfFovY = 0.0f;
		view.AspectRatio = aspectRatio;
		view.Exposure = 1.0f;
		return view;
	}

}
