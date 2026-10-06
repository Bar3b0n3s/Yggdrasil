#include "TestsPCH.h"

#include "Engine/Graphics/PipelineFactory.h"

#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/TrianglePass.h"
#include "Shared/SmokeConstants.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/SmokeProgram.h"

#include <cstring>

namespace Engine {

	TEST_SUITE("Graphics")
	{
		TEST_CASE("PipelineFactory: creates the Triangle pipeline after the reflection check"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsPipelineSpecification specification;
			specification.Layout = TrianglePass::GetLayoutDescription();
			specification.RenderState.rasterState.setCullBack().setFrontCounterClockwise(true);
			specification.RenderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
			specification.Framebuffer.addColorFormat(nvrhi::Format::RGBA8_UNORM);
			const Result<GraphicsPipeline> pipeline = gpu.GetPipelines().CreateGraphicsPipeline(specification);
			REQUIRE_MESSAGE(pipeline.has_value(), pipeline.error().ToString());
			CHECK(pipeline->Pipeline != nullptr);
			CHECK(pipeline->BindingLayouts.size() == specification.Layout.BindingLayouts.size());

			// A layout the reflection disagrees with is refused before anything is created.
			GraphicsPipelineSpecification broken = specification;
			broken.Layout.BindingLayouts[0].bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(7));
			const Result<GraphicsPipeline> refused = gpu.GetPipelines().CreateGraphicsPipeline(broken);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Validation);

			// Entries must fit the stages.
			GraphicsPipelineSpecification wrongEntries = specification;
			wrongEntries.Layout.Entries = { "VSMain" };
			const Result<GraphicsPipeline> rejected = gpu.GetPipelines().CreateGraphicsPipeline(wrongEntries);
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("Compute: Smoke scales a buffer exactly" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// §15.3 "compute arithmetic": a power-of-two scale is exact in floating point, and saturation clamps to [0, 1].
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			for (const char* saturate : { "0", "1" })
			{
				CAPTURE(std::string(saturate));
				const Result<ComputePipeline> pipeline = gpu.GetPipelines().CreateComputePipeline({ .Layout = Test::MakeSmokeLayoutDescription(saturate) });
				REQUIRE_MESSAGE(pipeline.has_value(), pipeline.error().ToString());

				const std::array<float, 8> input = { -1.5f, -0.25f, 0.0f, 0.125f, 0.25f, 0.5f, 1.0f, 3.0f };
				nvrhi::BufferDesc valuesDesc;
				valuesDesc.byteSize = sizeof(input);
				valuesDesc.structStride = sizeof(float);
				valuesDesc.canHaveUAVs = true;
				valuesDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
				valuesDesc.keepInitialState = true;
				valuesDesc.debugName = "SmokeValues";
				Result<nvrhi::BufferHandle> values = device.CreateBuffer(valuesDesc);
				REQUIRE_MESSAGE(values.has_value(), values.error().ToString());
				nvrhi::BufferDesc constantsDesc;
				constantsDesc.byteSize = sizeof(SmokeConstants);
				constantsDesc.isConstantBuffer = true;
				constantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
				constantsDesc.keepInitialState = true;
				constantsDesc.debugName = "SmokeConstants";
				Result<nvrhi::BufferHandle> constants = device.CreateBuffer(constantsDesc);
				REQUIRE_MESSAGE(constants.has_value(), constants.error().ToString());
				nvrhi::BufferDesc readbackDesc;
				readbackDesc.byteSize = sizeof(input);
				readbackDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
				readbackDesc.debugName = "SmokeReadback";
				Result<nvrhi::BufferHandle> readback = device.CreateBuffer(readbackDesc);
				REQUIRE_MESSAGE(readback.has_value(), readback.error().ToString());

				nvrhi::BindingSetDesc setDesc;
				setDesc.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, *constants), nvrhi::BindingSetItem::StructuredBuffer_UAV(0, *values) };
				Result<nvrhi::BindingSetHandle> set = device.CreateBindingSet(setDesc, *pipeline->BindingLayouts[0]);
				REQUIRE_MESSAGE(set.has_value(), set.error().ToString());

				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
				const SmokeConstants smoke{ .ElementCount = static_cast<uint32_t>(input.size()), .Scale = 2.0f };
				(*commandList)->open();
				(*commandList)->writeBuffer(*constants, &smoke, sizeof(smoke));
				(*commandList)->writeBuffer(*values, input.data(), sizeof(input));
				nvrhi::ComputeState state;
				state.pipeline = pipeline->Pipeline;
				state.bindings = { *set };
				(*commandList)->setComputeState(state);
				(*commandList)->dispatch(1);
				(*commandList)->copyBuffer(*readback, 0, *values, 0, sizeof(input));
				(*commandList)->close();
				WaitForSubmission(device, device.ExecuteCommandList(**commandList), "Smoke compute test");

				std::array<float, 8> output{};
				const void* mapped = device.GetNvrhiDevice()->mapBuffer(*readback, nvrhi::CpuAccessMode::Read);
				REQUIRE(mapped != nullptr);
				std::memcpy(output.data(), mapped, sizeof(output));
				device.GetNvrhiDevice()->unmapBuffer(*readback);
				for (size_t index = 0; index < input.size(); ++index)
				{
					const float scaled = input[index] * 2.0f;
					const float expected = std::string_view(saturate) == "1" ? std::clamp(scaled, 0.0f, 1.0f) : scaled;
					CHECK(output[index] == expected);
				}
			}
		}
	}

}
