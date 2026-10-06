#include "TestsPCH.h"
#include "Support/SmokeProgram.h"

namespace Engine {

	namespace Test {

		PipelineLayoutDescription MakeSmokeLayoutDescription(std::string_view saturate)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = { nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0) };
			return {
				.Name = std::format("Smoke (SMOKE_SATURATE={})", saturate),
				.Program = "Smoke",
				.Entries = { "CSMain" },
				.Permutation = { { .Key = "SMOKE_SATURATE", .Value = std::string(saturate) } },
				.BindingLayouts = { layout },
			};
		}

	}

}
