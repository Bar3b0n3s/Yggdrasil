#pragma once

#include "Engine/Core/Base.h"

// Deterministic transcendental functions (Architecture §4.12). C runtime transcendentals differ between C runtimes and
// may pick CPU-specific code paths at run time, so the simulation path (Core, Scene, Physics, Scripting, Session) uses
// these instead; Lint.py bans std::sin and friends there.
//
// Contract for every function, for float and double:
//   - In-house polynomial implementations, written in the style of Jolt's Trigonometry.h (MIT, attributed in
//     LICENSES.md), built only from IEEE-exact operations: + - * /, sqrt, floor, frexp/ldexp and bit manipulation (no fused
//     multiply-add). The project compiles with precise floating point and no contraction (§2.2), so a given input gives
//     bit-identical output on every platform, compiler and configuration (Debug, Release, Dist).
//   - Accuracy: within 2 ULP of the C++ standard library result over the domains the DetMath tests sample
//     (Tests/Source/Engine/Core/DetMathTests.cpp).
//   - Special values follow the C standard (Annex F): NaN in gives NaN out; Sin/Cos/Tan of +-Inf, ASin/ACos outside
//     [-1, 1] and Log/Log10 of a negative number give NaN; Log(+-0) = Log10(+-0) = -Inf; Exp, Sinh and Cosh overflow to
//     +-Inf and Exp underflows to +0; Sinh(+-Inf) = +-Inf, Cosh(+-Inf) = +Inf, Tanh(+-Inf) = +-1; Sin, Tan, ASin, ATan,
//     Sinh and Tanh keep the sign of a zero; ATan2 and Pow handle signed zeros and infinities as Annex F specifies.
//   - Log10 is exact at every power of ten the type represents exactly (10^0 to 10^22 for double, 10^0 to 10^10 for
//     float), so floor(Log10(n)) counts the digits of such n correctly.
//   - Sinh, Cosh, Tanh and Log10 exist because the script sandbox rebinds Luau's math.sinh/cosh/tanh/log10 to DetMath
//     (§4.12, §11.1).
//   - Pure functions; safe from any thread.

namespace Engine {

	template<typename T>
	struct SinCosResult
	{
		T Sin{};
		T Cos{};
	};

	class DetMath
	{
	public:
		DetMath() = delete;

		[[nodiscard]] static float Sin(float x);
		[[nodiscard]] static double Sin(double x);
		[[nodiscard]] static float Cos(float x);
		[[nodiscard]] static double Cos(double x);
		[[nodiscard]] static float Tan(float x);
		[[nodiscard]] static double Tan(double x);

		// Both values from one range reduction; equal to Sin(x) and Cos(x) bit for bit.
		[[nodiscard]] static SinCosResult<float> SinCos(float x);
		[[nodiscard]] static SinCosResult<double> SinCos(double x);

		[[nodiscard]] static float ASin(float x);
		[[nodiscard]] static double ASin(double x);
		[[nodiscard]] static float ACos(float x);
		[[nodiscard]] static double ACos(double x);
		[[nodiscard]] static float ATan(float x);
		[[nodiscard]] static double ATan(double x);
		// The angle of (x, y) in [-pi, pi], like std::atan2(y, x).
		[[nodiscard]] static float ATan2(float y, float x);
		[[nodiscard]] static double ATan2(double y, double x);

		[[nodiscard]] static float Sinh(float x);
		[[nodiscard]] static double Sinh(double x);
		[[nodiscard]] static float Cosh(float x);
		[[nodiscard]] static double Cosh(double x);
		[[nodiscard]] static float Tanh(float x);
		[[nodiscard]] static double Tanh(double x);

		[[nodiscard]] static float Exp(float x);
		[[nodiscard]] static double Exp(double x);
		// Natural logarithm.
		[[nodiscard]] static float Log(float x);
		[[nodiscard]] static double Log(double x);
		// Base-10 logarithm, exact at the exactly representable powers of ten (see the top of this file).
		[[nodiscard]] static float Log10(float x);
		[[nodiscard]] static double Log10(double x);
		[[nodiscard]] static float Pow(float base, float exponent);
		[[nodiscard]] static double Pow(double base, double exponent);
	};

}
