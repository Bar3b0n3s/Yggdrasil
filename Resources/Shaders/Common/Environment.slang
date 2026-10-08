// Environment lookups shared by the forward passes and the skybox (Architecture §8.6; Shared/EnvironmentConstants.h and
// Asset/EnvironmentData.h hold the conventions). Frozen by the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 9),
// which implemented these formulas because streams A and B both depend on them; stream B owns the module.
module Environment;

// The direction an environment map is looked up at for the world direction `direction`, with the environment turned by its
// Rotation about +Y (EnvironmentConstants::RotationSin and RotationCos).
public float3 RotateEnvironmentDirection(float3 direction, float rotationSin, float rotationCos)
{
	return float3(direction.x * rotationCos - direction.z * rotationSin, direction.y, direction.x * rotationSin + direction.z * rotationCos);
}

// The SH9 irradiance / pi at the unit direction `normal` (EnvironmentData.h: the real SH basis in the order (0, 0), (1, -1),
// (1, 0), (1, 1), (2, -2), (2, -1), (2, 0), (2, 1), (2, 2)), clamped to 0 (§8.6 step 4: negative results are clamped in
// the shader). The coefficients are EnvironmentConstants::IrradianceSH9's xyz.
public float3 EvaluateIrradianceSH9(float4 coefficients[9], float3 normal)
{
	const float x = normal.x;
	const float y = normal.y;
	const float z = normal.z;
	float3 result = coefficients[0].xyz * 0.282095;
	result += coefficients[1].xyz * (0.488603 * y);
	result += coefficients[2].xyz * (0.488603 * z);
	result += coefficients[3].xyz * (0.488603 * x);
	result += coefficients[4].xyz * (1.092548 * x * y);
	result += coefficients[5].xyz * (1.092548 * y * z);
	result += coefficients[6].xyz * (0.315392 * (3.0 * z * z - 1.0));
	result += coefficients[7].xyz * (1.092548 * x * z);
	result += coefficients[8].xyz * (0.546274 * (x * x - y * y));
	return max(result, float3(0.0));
}
