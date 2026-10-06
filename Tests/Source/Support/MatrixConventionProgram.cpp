#include "TestsPCH.h"
#include "Support/MatrixConventionProgram.h"

namespace Engine {

	namespace Test {

		PipelineLayoutDescription MakeMatrixConventionLayoutDescription()
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = { nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0) };
			return {
				.Name = "MatrixConvention",
				.Program = "MatrixConvention",
				.Entries = { "CSMain" },
				.BindingLayouts = { layout },
			};
		}

	}

}
