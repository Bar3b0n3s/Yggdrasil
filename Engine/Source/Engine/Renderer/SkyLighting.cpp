#include "EnginePCH.h"
#include "Engine/Renderer/SkyLighting.h"

#include "Engine/Core/Assert.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/FogConstants.h"
#include "Shared/ProbeConstants.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>

namespace Engine {

	void RecordSkyResultCopies(nvrhi::ICommandList& commandList, nvrhi::IBuffer* results, nvrhi::IBuffer* environment, std::span<nvrhi::IBuffer* const> fogs,
		nvrhi::IBuffer* probes, uint32_t reflectionProbes)
	{
		ENGINE_CORE_ASSERT(results != nullptr, "RecordSkyResultCopies needs the view's SkyResults buffer");
		// writeBuffer leaves each destination in CopyDest, and NVRHI places no barrier between two writes in one state (W45.1's
		// proof found the write-after-write hazard): moving the destinations to ConstantBuffer first completes their CPU writes,
		// so each copy's own barrier back to CopyDest orders it after them. A destination a pass already read is there already.
		const auto settle = [&commandList](nvrhi::IBuffer* buffer)
		{
			if (buffer != nullptr)
				commandList.setBufferState(buffer, nvrhi::ResourceStates::ConstantBuffer);
		};
		settle(environment);
		for (nvrhi::IBuffer* fog : fogs)
			settle(fog);
		settle(probes);
		commandList.commitBarriers();
		constexpr uint64_t Element = sizeof(glm::vec4);
		if (environment != nullptr)
			commandList.copyBuffer(environment, offsetof(EnvironmentConstants, IrradianceSH9), results, SkyResultsIrradiance * Element,
				sizeof(EnvironmentConstants::IrradianceSH9));
		for (nvrhi::IBuffer* fog : fogs)
			if (fog != nullptr)
				commandList.copyBuffer(fog, offsetof(FogConstants, Color), results, SkyResultsFogColor * Element, sizeof(FogConstants::Color));
		if (probes == nullptr)
			return;
		const uint32_t count = std::min(reflectionProbes, MaxViewReflectionProbes);
		for (uint32_t index = 0; index < count; ++index)
		{
			const uint64_t destination = offsetof(ProbeConstants, Reflections) + index * sizeof(ProbeReflectionConstants) + offsetof(ProbeReflectionConstants, AxisScale)
				+ 3 * sizeof(float);
			commandList.copyBuffer(probes, destination, results, (SkyResultsReflectionScales + index) * Element, sizeof(float));
		}
	}

}
