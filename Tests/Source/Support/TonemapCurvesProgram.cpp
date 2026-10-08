#include "TestsPCH.h"
#include "Support/TonemapCurvesProgram.h"

#include "Engine/Core/Error.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Shared/TonemapConstants.h"
#include "Support/HeadlessGpuFixture.h"

#include <cstring>
#include <utility>

namespace Engine {

	namespace Test {

		namespace {

			// The program's [numthreads(64, 1, 1)].
			constexpr uint32_t TonemapCurvesGroupSize = 64;

		}

		PipelineLayoutDescription MakeTonemapCurvesLayoutDescription()
		{
			// Set 0: the push constants (TonemapConstants, of which the program reads Tonemapper) and u0 the
			// RWStructuredBuffer<float4> Values, tonemapped in place.
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {
				nvrhi::BindingLayoutItem::PushConstants(0, sizeof(TonemapConstants)),
				nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),
			};
			return { .Name = "TonemapCurves", .Program = "TonemapCurves", .Entries = { "CSMain" }, .BindingLayouts = { layout } };
		}

		Result<std::vector<glm::vec3>> RunTonemapCurves(HeadlessGpuFixture& gpu, RenderTonemapper tonemapper, std::span<const glm::dvec3> points)
		{
			if (points.empty())
				return std::vector<glm::vec3>{};
			GraphicsDevice& device = gpu.GetDevice();
			ENGINE_TRY_ASSIGN(const ComputePipeline pipeline, gpu.GetPipelines().CreateComputePipeline({ .Layout = MakeTonemapCurvesLayoutDescription() }));

			std::vector<glm::vec4> values;
			values.reserve(points.size());
			for (const glm::dvec3& point : points)
				values.emplace_back(glm::vec3(point), 0.0f);
			const size_t byteSize = values.size() * sizeof(glm::vec4);

			nvrhi::BufferDesc valuesDesc;
			valuesDesc.byteSize = byteSize;
			valuesDesc.structStride = sizeof(glm::vec4);
			valuesDesc.canHaveUAVs = true;
			valuesDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
			valuesDesc.keepInitialState = true;
			valuesDesc.debugName = "TonemapCurves.Values";
			ENGINE_TRY_ASSIGN(const nvrhi::BufferHandle valuesBuffer, device.CreateBuffer(valuesDesc));

			nvrhi::BufferDesc readbackDesc;
			readbackDesc.byteSize = byteSize;
			readbackDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
			readbackDesc.initialState = nvrhi::ResourceStates::CopyDest;
			readbackDesc.keepInitialState = true;
			readbackDesc.debugName = "TonemapCurves.Readback";
			ENGINE_TRY_ASSIGN(const nvrhi::BufferHandle readbackBuffer, device.CreateBuffer(readbackDesc));

			nvrhi::BindingSetDesc bindings;
			bindings.bindings = {
				nvrhi::BindingSetItem::PushConstants(0, sizeof(TonemapConstants)),
				nvrhi::BindingSetItem::StructuredBuffer_UAV(0, valuesBuffer),
			};
			ENGINE_TRY_ASSIGN(const nvrhi::BindingSetHandle bindingSet, device.CreateBindingSet(bindings, *pipeline.BindingLayouts[0]));
			ENGINE_TRY_ASSIGN(const nvrhi::CommandListHandle commandList, device.CreateCommandList());

			TonemapConstants constants;
			constants.Tonemapper = std::to_underlying(tonemapper);
			commandList->open();
			commandList->writeBuffer(valuesBuffer, values.data(), byteSize);
			nvrhi::ComputeState state;
			state.pipeline = pipeline.Pipeline;
			state.bindings = { bindingSet };
			commandList->setComputeState(state);
			commandList->setPushConstants(&constants, sizeof(constants));
			commandList->dispatch(static_cast<uint32_t>((values.size() + TonemapCurvesGroupSize - 1) / TonemapCurvesGroupSize));
			commandList->copyBuffer(readbackBuffer, 0, valuesBuffer, 0, byteSize);
			commandList->close();
			WaitForSubmission(device, device.ExecuteCommandList(*commandList), "the tonemapper curves");

			const void* mapped = device.GetNvrhiDevice()->mapBuffer(readbackBuffer, nvrhi::CpuAccessMode::Read);
			if (mapped == nullptr)
				return MakeError(ErrorCode::Gpu, "could not map the tonemapper curves' readback buffer");
			std::memcpy(values.data(), mapped, byteSize);
			device.GetNvrhiDevice()->unmapBuffer(readbackBuffer);

			std::vector<glm::vec3> results;
			results.reserve(values.size());
			for (const glm::vec4& value : values)
				results.emplace_back(value);
			return results;
		}

	}

}
