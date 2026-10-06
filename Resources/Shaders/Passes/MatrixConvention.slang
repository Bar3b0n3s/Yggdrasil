// MatrixConvention: the GPU check of the matrix convention of Architecture §8.4 (column-major glm on the C++ side,
// -matrix-layout-column-major, mul(M, v) in Slang). It transforms the known vectors of the buffer by the ViewProjection
// matrix of ViewConstants, the shared struct every pass reads its matrices from, and "Shaders: matrix convention transforms
// known vectors" compares the results with glm on the CPU. No engine pass uses it; its pipeline layout lives in
// Tests/Source/Support/MatrixConventionProgram.h.

#include "Shared/ViewConstants.h"

ConstantBuffer<ViewConstants> View : register(b0, space0);
RWStructuredBuffer<float4> Vectors : register(u0, space0);

[shader("compute")]
[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint count = 0;
	uint stride = 0;
	Vectors.GetDimensions(count, stride);
	const uint index = dispatchThreadID.x;
	if (index >= count)
		return;

	Vectors[index] = mul(View.ViewProjection, Vectors[index]);
}
