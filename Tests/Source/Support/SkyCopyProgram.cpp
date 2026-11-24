#include "TestsPCH.h"
#include "Support/SkyCopyProgram.h"

#include "Shared/EnvironmentConstants.h"
#include "Shared/FogConstants.h"
#include "Shared/ProbeConstants.h"
#include "Shared/SkyConstants.h"

namespace Engine {

	namespace Test {

		static_assert(SkyCopyReadbackCount == 14 + 2 * MaxViewReflectionProbes, "CSTestCopyRead writes each reflection probe's two vectors");

		PipelineLayoutDescription MakeSkyCopyWriteLayoutDescription()
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			// Every entry's reflection lists the program's push constants (SkyDispatchConstants), read or not.
			layout.bindings = { nvrhi::BindingLayoutItem::ConstantBuffer(1), nvrhi::BindingLayoutItem::StructuredBuffer_UAV(6),
				nvrhi::BindingLayoutItem::PushConstants(0, sizeof(SkyDispatchConstants)) };
			return {
				.Name = "SkyCopyWrite",
				.Program = "Sky",
				.Entries = { "CSTestCopyWrite" },
				.BindingLayouts = { layout },
				.ConstantBuffers = { { .Set = 0, .Register = 1, .ByteSize = sizeof(SkyConstants) } },
			};
		}

		PipelineLayoutDescription MakeSkyCopyReadLayoutDescription()
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = { nvrhi::BindingLayoutItem::ConstantBuffer(2), nvrhi::BindingLayoutItem::ConstantBuffer(4), nvrhi::BindingLayoutItem::ConstantBuffer(5),
				nvrhi::BindingLayoutItem::StructuredBuffer_UAV(7), nvrhi::BindingLayoutItem::PushConstants(0, sizeof(SkyDispatchConstants)) };
			return {
				.Name = "SkyCopyRead",
				.Program = "Sky",
				.Entries = { "CSTestCopyRead" },
				.BindingLayouts = { layout },
				.ConstantBuffers = { { .Set = 0, .Register = 2, .ByteSize = sizeof(EnvironmentConstants) },
					{ .Set = 0, .Register = 4, .ByteSize = sizeof(ProbeConstants) }, { .Set = 0, .Register = 5, .ByteSize = sizeof(FogConstants) } },
			};
		}

	}

}
