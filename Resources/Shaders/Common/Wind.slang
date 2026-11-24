// The wind's displacement of a mesh vertex (M46, Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3G):
// the one function every vertex entry that draws meshes calls for an instance with DrawFlagWind (Scene.slang's VSMain,
// VSPrepass, VSPicking and VSOverdraw, Shadow.slang's VSMain and Selection.slang's VSMain), in world space after the World
// product and before the view-projection product. Included by those programs, it declares the view's WindConstants at b8
// of set 0 (Shared/WindConstants.h).
// Bit-identical positions: every product and sum is written once here with SPIR-V's NoContraction (as Scene.slang's
// MultiplyExact), and the other operations are frac, abs, min, max and conversions, all exact; no division and no
// transcendental function. So every pipeline computes the same bits, the forward pass's GreaterOrEqual test keeps exactly
// the prepass's surface, and the selection mask's depth test agrees with it. A draw without DrawFlagWind never calls it, so
// it runs exactly the arithmetic it ran before M46.
// The displacement, from the vertex's world position p, its entity's origin o (World's translation), its normal through
// World's upper 3 x 3, its _WIND weights (MeshVertexWind: x branch, y phase, z flutter, w bend mask; zeros for a mesh
// without them) and its record's WindResponse (Bend, Branch, Flutter and Stiffness, InstanceRecord.h):
//   - the gusts: at the point's coordinates a and c along and across the wind, in gust wavelengths, the along speed is
//     Strength + GustStrength G(GustPhase - a + Turbulence G(c / 2)) and the sideways one Turbulence GustStrength
//     W(HalfGustPhase - a / 2 + c + 1/4), as Scene/Wind.h's SampleWind models them (G the smoothed triangle wave in
//     [0, 1], W = 2 G - 1);
//   - the responses: x = clamp(speed (1 - Stiffness) / 10 m/s, -1, 1) and r = x (2 - |x|), so a calm wind gives exactly 0,
//     a supple plant (Stiffness 0) saturates at 10 m/s and a rigid one (Stiffness 1) never moves;
//   - the bend: at the height h = max(p.y - o.y, 0) above the origin, the vertex leans by Bend (1 - mask) h^2 times the
//     responses along and across the wind, and drops by half its lean squared over h (at most h), so its distance from the
//     origin is nearly kept;
//   - the branch sway: weights.x Branch r W(BranchPhase + weights.y - a / 2) along the wind;
//   - the flutter: weights.z Flutter r W(FlutterPhase + weights.y + c) along the normal.

#include "Shared/DrawConstants.h"
#include "Shared/WindConstants.h"

ConstantBuffer<WindConstants> Wind : register(b8, space0);

// mul(matrix, vector) with NoContraction (Scene.slang's MultiplyExact): the origin and the normal every entry passes in.
float4 WindTransform(float4x4 matrix, float4 vector)
{
	return spirv_asm {
		%product:$$float4 = OpVectorTimesMatrix $vector $matrix;
		OpDecorate %product NoContraction;
		result:$$float4 = OpCopyObject %product
	};
}

// One render's wind: a half of WindConstants.
struct WindFrame
{
	float2 Direction;
	float Strength;
	float GustStrength;
	float Turbulence;
	float InverseGustWavelength;
	float GustPhase;
	float HalfGustPhase;
	float BranchPhase;
	float FlutterPhase;
};

// The current render's wind, or with `previous` the previous render's (the motion vectors').
WindFrame GetWindFrame(bool previous)
{
	WindFrame frame;
	frame.Direction = previous ? Wind.PreviousDirection : Wind.Direction;
	frame.Strength = previous ? Wind.PreviousStrength : Wind.Strength;
	frame.GustStrength = previous ? Wind.PreviousGustStrength : Wind.GustStrength;
	frame.Turbulence = previous ? Wind.PreviousTurbulence : Wind.Turbulence;
	frame.InverseGustWavelength = previous ? Wind.PreviousInverseGustWavelength : Wind.InverseGustWavelength;
	frame.GustPhase = previous ? Wind.PreviousGustPhase : Wind.GustPhase;
	frame.HalfGustPhase = previous ? Wind.PreviousHalfGustPhase : Wind.HalfGustPhase;
	frame.BranchPhase = previous ? Wind.PreviousBranchPhase : Wind.BranchPhase;
	frame.FlutterPhase = previous ? Wind.PreviousFlutterPhase : Wind.FlutterPhase;
	return frame;
}

// Whether an instance with draw flags `flags` moves in the wind.
bool HasWind(uint flags)
{
	return (flags & DrawFlagWind) != 0;
}

// a x b, a + b and a - b with NoContraction: no implementation may fuse them with each other or with other operations.
float WindMul(float a, float b)
{
	return spirv_asm {
		%product:$$float = OpFMul $a $b;
		OpDecorate %product NoContraction;
		result:$$float = OpCopyObject %product
	};
}

float WindAdd(float a, float b)
{
	return spirv_asm {
		%sum:$$float = OpFAdd $a $b;
		OpDecorate %sum NoContraction;
		result:$$float = OpCopyObject %sum
	};
}

float WindSub(float a, float b)
{
	return spirv_asm {
		%difference:$$float = OpFSub $a $b;
		OpDecorate %difference NoContraction;
		result:$$float = OpCopyObject %difference
	};
}

// The smoothed triangle wave G(x) in [0, 1] of period 1: u = 1 - |2 frac(x) - 1|, then u^2 (3 - 2 u).
float WindGust(float x)
{
	const float u = WindSub(1.0, abs(WindSub(WindMul(2.0, frac(x)), 1.0)));
	return WindMul(WindMul(u, u), WindSub(3.0, WindMul(2.0, u)));
}

// The same wave in [-1, 1]: 2 G(x) - 1.
float WindWave(float x)
{
	return WindSub(WindMul(2.0, WindGust(x)), 1.0);
}

// The response x (2 - |x|) to a speed (m/s) of a plant whose resistance gives `scale` = (1 - Stiffness) / 10, x the
// speed times scale clamped to [-1, 1]: 0 for a calm wind, saturating at +-1.
float WindResponse(float speed, float scale)
{
	const float x = clamp(WindMul(speed, scale), -1.0, 1.0);
	return WindMul(x, WindSub(2.0, abs(x)));
}

// The world position of the vertex at world position `position` of an instance whose World has the translation `origin`,
// with its normal through World's upper 3 x 3 `normal`, its _WIND weights `weights` and its record's WindResponse
// `response`, displaced by the wind `wind` (see the file comment).
float3 ApplyWind(WindFrame wind, float3 position, float3 origin, float3 normal, float4 weights, uint response)
{
	// The material's wind block (InstanceRecord::WindResponse's unorm8 bytes over their ranges).
	const float bend = WindMul(float(response & 0xFFu), 1.0 / 255.0);
	const float branch = WindMul(float((response >> 8) & 0xFFu), 1.0 / 255.0);
	const float flutter = WindMul(float((response >> 16) & 0xFFu), WindFlutterRange / 255.0);
	const float stiffness = WindMul(float(response >> 24), 1.0 / 255.0);

	// The point's coordinates along and across the wind, in gust wavelengths, and the gusted speeds there.
	const float2 along = wind.Direction;
	const float2 across = float2(-along.y, along.x);
	const float a = WindMul(WindAdd(WindMul(position.x, along.x), WindMul(position.z, along.y)), wind.InverseGustWavelength);
	const float c = WindMul(WindAdd(WindMul(position.x, across.x), WindMul(position.z, across.y)), wind.InverseGustWavelength);
	const float gust = WindGust(WindAdd(WindSub(wind.GustPhase, a), WindMul(wind.Turbulence, WindGust(WindMul(c, 0.5)))));
	const float alongSpeed = WindAdd(wind.Strength, WindMul(wind.GustStrength, gust));
	const float sideways = WindWave(WindAdd(WindAdd(WindSub(wind.HalfGustPhase, WindMul(a, 0.5)), c), 0.25));
	const float acrossSpeed = WindMul(WindMul(wind.Turbulence, wind.GustStrength), sideways);
	const float scale = WindMul(WindSub(1.0, stiffness), 0.1);
	const float alongResponse = WindResponse(alongSpeed, scale);
	const float acrossResponse = WindResponse(acrossSpeed, scale);

	// The bend: the lean grows with the height's square, and the drop keeps the distance from the origin nearly constant.
	const float height = max(WindSub(position.y, origin.y), 0.0);
	const float bendScale = WindMul(bend, WindSub(1.0, weights.w));
	const float lean = WindMul(WindMul(bendScale, height), height);
	const float leanX = WindMul(WindAdd(WindMul(alongResponse, along.x), WindMul(acrossResponse, across.x)), lean);
	const float leanZ = WindMul(WindAdd(WindMul(alongResponse, along.y), WindMul(acrossResponse, across.y)), lean);
	const float responses = WindAdd(WindMul(alongResponse, alongResponse), WindMul(acrossResponse, acrossResponse));
	const float drop = min(WindMul(WindMul(WindMul(WindMul(WindMul(0.5, bendScale), bendScale), height), WindMul(height, height)), responses), height);

	// The branch sway along the wind and the flutter along the normal.
	const float branchWave = WindWave(WindSub(WindAdd(wind.BranchPhase, weights.y), WindMul(a, 0.5)));
	const float sway = WindMul(WindMul(WindMul(weights.x, branch), alongResponse), branchWave);
	const float flutterWave = WindWave(WindAdd(WindAdd(wind.FlutterPhase, weights.y), c));
	const float flutterScale = WindMul(WindMul(WindMul(weights.z, flutter), alongResponse), flutterWave);

	const float3 offset = float3(WindAdd(WindAdd(leanX, WindMul(sway, along.x)), WindMul(flutterScale, normal.x)),
		WindSub(WindMul(flutterScale, normal.y), drop), WindAdd(WindAdd(leanZ, WindMul(sway, along.y)), WindMul(flutterScale, normal.z)));
	return float3(WindAdd(position.x, offset.x), WindAdd(position.y, offset.y), WindAdd(position.z, offset.z));
}
