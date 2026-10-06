#include "EnginePCH.h"
#include "Engine/Core/DetMath.h"

#include <bit>
#include <cmath>

// How the functions stay deterministic and accurate (Architecture §4.12, DetMath.h):
//   - Only IEEE-754 operations whose result is exactly specified are used: + - * / and sqrt in round-to-nearest, floor,
//     integer conversions and bit manipulation. The project compiles without FMA contraction (§2.2), so every
//     expression evaluates to the same bits on every compiler, platform and configuration.
//   - Double-double arithmetic (an unevaluated sum Hi + Lo, about 106 bits) carries the leading terms, so every double
//     result is rounded once at the end with an error close to 0.5 ULP. Error-free transformations: TwoSum (Knuth),
//     FastTwoSum and TwoProduct with Veltkamp's split (Dekker), which needs no fused multiply-add.
//   - Arguments are reduced to small intervals around table points; the remaining polynomials are truncated Taylor series
//     whose first omitted term is below 2^-60 of the result, so their coefficients are exact rationals.
//   - float functions evaluate the double function and round once more; the double result is accurate to far more than
//     the 24 bits a float keeps.
//
// The tables are mathematical constants rounded to doubles: for a double-double entry of the exact value v,
// Hi = RN(v) and Lo = RN(v - Hi) (RN: round to nearest even). They were computed with exact integer and 140-digit decimal
// arithmetic; any arbitrary-precision library reproduces them from the definitions next to each table.

namespace Engine {

	namespace {

		// The unevaluated sum Hi + Lo; normalized when |Lo| <= ulp(Hi) / 2.
		struct DoubleDouble
		{
			double Hi = 0.0;
			double Lo = 0.0;
		};

		// Value * 2^Exponent, with Value in [0.98, 2.03]: lets exponentials scale once, past the overflow threshold of an
		// intermediate double.
		struct ScaledDoubleDouble
		{
			DoubleDouble Value;
			int Exponent = 0;
		};

		struct LogTableEntry
		{
			double InverseC = 0.0; // RN(1 / c) for c = 1 + j / 128; exactly 1 for j = 0
			DoubleDouble LogC;     // -log(InverseC): the logarithm of the exact reciprocal of the stored InverseC
		};

		struct SinCosTableEntry
		{
			DoubleDouble Sin;
			DoubleDouble Cos;
		};

		// |x| = Quadrant * pi/2 + R (modulo 2 pi), with |R| <= pi/4 plus rounding.
		struct ReducedAngle
		{
			DoubleDouble R;
			uint32_t Quadrant = 0;
		};

		// R = c + Offset (Offset a double-double, |Offset| <= 1/64 plus rounding) for the table point c = Index / 32 nearest
		// to |R|; Negative when R < 0.
		struct KernelArgument
		{
			size_t Index = 0;
			DoubleDouble Offset;
			bool Negative = false;
		};

		// A finite |x| >= 2^-27 prepared for Sin, Cos, Tan and SinCos: one reduction shared by all of them, so SinCos is
		// bit-identical to Sin and Cos.
		struct SinCosArgument
		{
			KernelArgument Kernel;
			uint32_t Quadrant = 0;
			bool Negative = false; // x < 0: Sin and Tan change sign
		};

		struct UInt128
		{
			uint64_t High = 0;
			uint64_t Low = 0;
		};

	}

	namespace Utils {

		static constexpr double QuietNaN = std::numeric_limits<double>::quiet_NaN();
		static constexpr double Infinity = std::numeric_limits<double>::infinity();

		static constexpr uint64_t ExponentMask = 0x7ff0000000000000ull;
		static constexpr uint64_t MantissaMask = 0x000fffffffffffffull;
		static constexpr uint64_t SmallestNormalBits = 0x0010000000000000ull;

		// Below this magnitude sin x, tan x, asin x, atan x, sinh x and tanh x round to x, and cos x to 1: the next Taylor
		// term is below 2^-54 relative. Returning x keeps the sign of a zero (C Annex F).
		static constexpr double TinyArgument = 0x1p-27;

		// pi, pi/2 and 3 pi/4.
		static constexpr DoubleDouble Pi = { 0x1.921fb54442d18p+1, 0x1.1a62633145c07p-53 };
		static constexpr DoubleDouble PiOver2 = { 0x1.921fb54442d18p+0, 0x1.1a62633145c07p-54 };
		static constexpr double PiOver4 = 0x1.921fb54442d18p-1;      // RN(pi/4)
		static constexpr double ThreePiOver4 = 0x1.2d97c7f3321d2p+1; // RN(3 pi/4)

		// Exact building blocks

		// 2^exponent for -1022 <= exponent <= 1023, from its bit pattern.
		static double PowerOfTwo(int exponent)
		{
			return std::bit_cast<double>(static_cast<uint64_t>(exponent + 1023) << 52);
		}

		// value * 2^exponent with a single rounding, for value in [0.5, 2.1) and |exponent| <= 1100: the first
		// multiplication is exact, so a subnormal result is rounded once and an overflow gives infinity.
		static double ScaleByPowerOfTwo(double value, int exponent)
		{
			if (exponent > 1000)
				return (value * PowerOfTwo(1000)) * PowerOfTwo(std::min(exponent - 1000, 1023));
			if (exponent < -1000)
				return (value * PowerOfTwo(exponent + 200)) * PowerOfTwo(-200);
			return value * PowerOfTwo(exponent);
		}

		// Hi + Lo == a + b exactly, for any finite a and b (Knuth).
		static DoubleDouble TwoSum(double a, double b)
		{
			const double sum = a + b;
			const double bPart = sum - a;
			const double aPart = sum - bPart;
			return { sum, (a - aPart) + (b - bPart) };
		}

		// Hi + Lo == a + b exactly, when a == 0 or |a| >= |b| (Dekker).
		static DoubleDouble FastTwoSum(double a, double b)
		{
			const double sum = a + b;
			return { sum, b - (sum - a) };
		}

		// a == Hi + Lo, each part with at most 26 significant bits (Veltkamp); |a| < 2^995.
		static DoubleDouble Split(double a)
		{
			const double scaled = 134217729.0 * a; // 2^27 + 1
			const double high = scaled - (scaled - a);
			return { high, a - high };
		}

		// Hi + Lo == a * b exactly (Dekker), when no partial product overflows or underflows.
		static DoubleDouble TwoProduct(double a, double b)
		{
			const double product = a * b;
			const DoubleDouble aParts = Split(a);
			const DoubleDouble bParts = Split(b);
			const double error =
				((aParts.Hi * bParts.Hi - product) + aParts.Hi * bParts.Lo + aParts.Lo * bParts.Hi) + aParts.Lo * bParts.Lo;
			return { product, error };
		}

		static DoubleDouble Negate(DoubleDouble value)
		{
			return { -value.Hi, -value.Lo };
		}

		static DoubleDouble Add(DoubleDouble a, DoubleDouble b)
		{
			const DoubleDouble sum = TwoSum(a.Hi, b.Hi);
			return TwoSum(sum.Hi, sum.Lo + (a.Lo + b.Lo));
		}

		static DoubleDouble Subtract(DoubleDouble a, DoubleDouble b)
		{
			return Add(a, Negate(b));
		}

		// a / b; b.Hi != 0 and every value of moderate magnitude (the split in TwoProduct must not overflow).
		static DoubleDouble Divide(DoubleDouble a, DoubleDouble b)
		{
			const double quotient = a.Hi / b.Hi;
			const DoubleDouble product = TwoProduct(quotient, b.Hi);
			const double remainder = (((a.Hi - product.Hi) - product.Lo) + a.Lo) - quotient * b.Lo;
			return FastTwoSum(quotient, remainder / b.Hi);
		}

		// sqrt(a) for a.Hi > 0.
		static DoubleDouble Sqrt(DoubleDouble a)
		{
			const double root = std::sqrt(a.Hi);
			const DoubleDouble square = TwoProduct(root, root);
			return FastTwoSum(root, (((a.Hi - square.Hi) - square.Lo) + a.Lo) / (2.0 * root));
		}

		// The integer nearest to value (|value| < 2^51), except that the rounding of value + 0.5 may pick the other neighbour
		// within 2^-53 of a half-integer. Callers only need |value - result| <= 1/2 plus such a rounding error.
		static double RoundToInteger(double value)
		{
			return std::floor(value + 0.5);
		}

		// coefficients[0] + z * (coefficients[1] + z * (... + z * coefficients[N - 1])).
		template<size_t N>
		static double Horner(double z, const std::array<double, N>& coefficients)
		{
			double sum = coefficients[N - 1];
			for (size_t index = N - 1; index-- > 0;)
				sum = coefficients[index] + z * sum;
			return sum;
		}

		// Exponential: e^x = 2^m * 2^(j/32) * e^r, n = 32 m + j = round(x * 32 / ln 2), r = x - n ln2/32, |r| <= ln2/64

		static constexpr double InverseLn2Times32 = 0x1.71547652b82fep+5; // RN(32 / ln 2)
		// ln2/32 = Ln2Over32Hi + Ln2Over32Lo; Hi has 37 significant bits, so n * Ln2Over32Hi is exact for |n| < 2^16.
		static constexpr double Ln2Over32Hi = 0x1.62e42fefa0000p-6;
		static constexpr double Ln2Over32Lo = 0x1.cf79abc9e3b3ap-45;
		// (e^r - 1 - r) / r^2 through r^7: 1/2!, 1/3!, ... 1/7! (the omitted r^8 / 8! is below 2^-67).
		static constexpr std::array<double, 6> ExpTaylor = { 1.0 / 2.0, 1.0 / 6.0, 1.0 / 24.0, 1.0 / 120.0, 1.0 / 720.0, 1.0 / 5040.0 };
		// Largest |x| ExpKernel accepts: beyond it e^x overflows or underflows in every format (|n| < 2^16).
		static constexpr double ExpKernelLimit = 746.0;

		// 2^(j/32) for j = 0 ... 31.
		static constexpr std::array<DoubleDouble, 32> ExpTable = { {
			{ 0x1.0000000000000p+0, 0x0p+0 },                 // 2^(0/32)
			{ 0x1.059b0d3158574p+0, 0x1.d73e2a475b465p-55 },  // 2^(1/32)
			{ 0x1.0b5586cf9890fp+0, 0x1.8a62e4adc610bp-54 },  // 2^(2/32)
			{ 0x1.11301d0125b51p+0, -0x1.6c51039449b3ap-54 }, // 2^(3/32)
			{ 0x1.172b83c7d517bp+0, -0x1.19041b9d78a76p-55 }, // 2^(4/32)
			{ 0x1.1d4873168b9aap+0, 0x1.e016e00a2643cp-54 },  // 2^(5/32)
			{ 0x1.2387a6e756238p+0, 0x1.9b07eb6c70573p-54 },  // 2^(6/32)
			{ 0x1.29e9df51fdee1p+0, 0x1.612e8afad1255p-55 },  // 2^(7/32)
			{ 0x1.306fe0a31b715p+0, 0x1.6f46ad23182e4p-55 },  // 2^(8/32)
			{ 0x1.371a7373aa9cbp+0, -0x1.63aeabf42eae2p-54 }, // 2^(9/32)
			{ 0x1.3dea64c123422p+0, 0x1.ada0911f09ebcp-55 },  // 2^(10/32)
			{ 0x1.44e086061892dp+0, 0x1.89b7a04ef80d0p-59 },  // 2^(11/32)
			{ 0x1.4bfdad5362a27p+0, 0x1.d4397afec42e2p-56 },  // 2^(12/32)
			{ 0x1.5342b569d4f82p+0, -0x1.07abe1db13cadp-55 }, // 2^(13/32)
			{ 0x1.5ab07dd485429p+0, 0x1.6324c054647adp-54 },  // 2^(14/32)
			{ 0x1.6247eb03a5585p+0, -0x1.383c17e40b497p-54 }, // 2^(15/32)
			{ 0x1.6a09e667f3bcdp+0, -0x1.bdd3413b26456p-54 }, // 2^(16/32)
			{ 0x1.71f75e8ec5f74p+0, -0x1.16e4786887a99p-55 }, // 2^(17/32)
			{ 0x1.7a11473eb0187p+0, -0x1.41577ee04992fp-55 }, // 2^(18/32)
			{ 0x1.82589994cce13p+0, -0x1.d4c1dd41532d8p-54 }, // 2^(19/32)
			{ 0x1.8ace5422aa0dbp+0, 0x1.6e9f156864b27p-54 },  // 2^(20/32)
			{ 0x1.93737b0cdc5e5p+0, -0x1.75fc781b57ebcp-57 }, // 2^(21/32)
			{ 0x1.9c49182a3f090p+0, 0x1.c7c46b071f2bep-56 },  // 2^(22/32)
			{ 0x1.a5503b23e255dp+0, -0x1.d2f6edb8d41e1p-54 }, // 2^(23/32)
			{ 0x1.ae89f995ad3adp+0, 0x1.7a1cd345dcc81p-54 },  // 2^(24/32)
			{ 0x1.b7f76f2fb5e47p+0, -0x1.5584f7e54ac3bp-56 }, // 2^(25/32)
			{ 0x1.c199bdd85529cp+0, 0x1.11065895048ddp-55 },  // 2^(26/32)
			{ 0x1.cb720dcef9069p+0, 0x1.503cbd1e949dbp-56 },  // 2^(27/32)
			{ 0x1.d5818dcfba487p+0, 0x1.2ed02d75b3707p-55 },  // 2^(28/32)
			{ 0x1.dfc97337b9b5fp+0, -0x1.1a5cd4f184b5cp-54 }, // 2^(29/32)
			{ 0x1.ea4afa2a490dap+0, -0x1.e9c23179c2893p-54 }, // 2^(30/32)
			{ 0x1.f50765b6e4540p+0, 0x1.9d3e12dd8a18bp-54 },  // 2^(31/32)
		} };

		// e^(xHi + xLo) for |xHi| <= ExpKernelLimit and |xLo| <= ulp(xHi): Value accurate to about 2^-66 relative.
		static ScaledDoubleDouble ExpKernel(double xHi, double xLo)
		{
			const double nearest = RoundToInteger(xHi * InverseLn2Times32);
			const int n = static_cast<int>(nearest);
			const int index = n & 31;
			// xHi - nearest * Ln2Over32Hi is exact (the product is exact and the difference is Sterbenz-exact).
			const DoubleDouble reduced = TwoSum(xHi - nearest * Ln2Over32Hi, -(nearest * Ln2Over32Lo));
			const double r = reduced.Hi;
			const double rLo = reduced.Lo + xLo;
			// e^(r + rLo) - 1 - r = quadratic + rLo * e^r to first order in rLo. Pow passes |xLo| up to about 2^-43, so the
			// rLo * r^2/2 part of rLo * e^r matters; rLo^2 does not.
			const double quadratic = r * r * Horner(r, ExpTaylor);
			const double small = quadratic + rLo * (1.0 + r + quadratic);
			// 2^(j/32) * (1 + r + small), with the large product entry.Hi * r kept exact.
			const DoubleDouble& entry = ExpTable[static_cast<size_t>(index)];
			const DoubleDouble scaledR = TwoProduct(entry.Hi, r);
			const DoubleDouble head = TwoSum(entry.Hi, scaledR.Hi);
			const double lo = head.Lo + (scaledR.Lo + (entry.Hi * small + entry.Lo * (1.0 + r)));
			return { FastTwoSum(head.Hi, lo), (n - index) / 32 };
		}

		// e^x as a plain double-double for |x| <= 44 (2^Exponent is folded in exactly).
		static DoubleDouble ExpDoubleDouble(double x)
		{
			const ScaledDoubleDouble exponential = ExpKernel(x, 0.0);
			const double scale = PowerOfTwo(exponential.Exponent);
			return { exponential.Value.Hi * scale, exponential.Value.Lo * scale };
		}

		// Logarithm: x = 2^e * m, m in [0.75, 1.5); log x = e ln2 + log(1/InverseC) + log1p(m * InverseC - 1)

		// ln2 = Ln2Hi + Ln2Lo; Hi has 42 significant bits, so e * Ln2Hi is exact for every exponent e.
		static constexpr double Ln2Hi = 0x1.62e42fefa3800p-1;
		static constexpr double Ln2Lo = 0x1.ef35793c76730p-45;
		// log10(e) = 1 / ln 10.
		static constexpr DoubleDouble Log10OfE = { 0x1.bcb7b1526e50ep-2, 0x1.95355baaafad3p-57 };

		// (log1p(r) - r + r^2/2) / r^3 through r^10: 1/3, -1/4, ... -1/10 (for |r| <= 2^-7.5 the omitted r^11 / 11 is below
		// 2^-78 of r).
		static constexpr std::array<double, 8> Log1pTaylor = {
			1.0 / 3.0,
			-1.0 / 4.0,
			1.0 / 5.0,
			-1.0 / 6.0,
			1.0 / 7.0,
			-1.0 / 8.0,
			1.0 / 9.0,
			-1.0 / 10.0,
		};

		// For c = 1 + j / 128, j = -32 ... 64: InverseC = RN(1 / c) (1 for j = 0) and -log(InverseC).
		static constexpr std::array<LogTableEntry, 97> LogTable = { {
			{ 0x1.5555555555555p+0, { -0x1.269621134db91p-2, -0x1.e0efadd9db02ap-56 } }, // c = 1 + (-32) / 128
			{ 0x1.51d07eae2f815p+0, { -0x1.1bf99635a6b95p-2, 0x1.e9575c2124912p-56 } },  // c = 1 + (-31) / 128
			{ 0x1.4e5e0a72f0539p+0, { -0x1.1178e8227e47ap-2, -0x1.b8ce2d07f1cb7p-56 } }, // c = 1 + (-30) / 128
			{ 0x1.4afd6a052bf5bp+0, { -0x1.07138604d5864p-2, 0x1.24e912b16ec8bp-60 } },  // c = 1 + (-29) / 128
			{ 0x1.47ae147ae147bp+0, { -0x1.f991c6cb3b37ap-3, -0x1.ecca0cdf30143p-58 } }, // c = 1 + (-28) / 128
			{ 0x1.446f86562d9fbp+0, { -0x1.e530effe71013p-3, 0x1.f7627ef82f3f0p-57 } },  // c = 1 + (-27) / 128
			{ 0x1.4141414141414p+0, { -0x1.d1037f2655e7bp-3, 0x1.3f3adb7b71cbcp-58 } },  // c = 1 + (-26) / 128
			{ 0x1.3e22cbce4a902p+0, { -0x1.bd087383bd8aap-3, 0x1.1165504ad749ep-59 } },  // c = 1 + (-25) / 128
			{ 0x1.3b13b13b13b14p+0, { -0x1.a93ed3c8ad9e5p-3, -0x1.bcafa9de97202p-57 } }, // c = 1 + (-24) / 128
			{ 0x1.3813813813814p+0, { -0x1.95a5adcf70182p-3, -0x1.8a16283fdbd1cp-57 } }, // c = 1 + (-23) / 128
			{ 0x1.3521cfb2b78c1p+0, { -0x1.823c16551a3c0p-3, -0x1.6dcd318f4187ep-57 } }, // c = 1 + (-22) / 128
			{ 0x1.323e34a2b10bfp+0, { -0x1.6f0128b756ab9p-3, 0x1.37967087859b9p-59 } },  // c = 1 + (-21) / 128
			{ 0x1.2f684bda12f68p+0, { -0x1.5bf406b543db0p-3, 0x1.1f5b44c0df7f7p-61 } },  // c = 1 + (-20) / 128
			{ 0x1.2c9fb4d812ca0p+0, { -0x1.4913d8333b563p-3, 0x1.0d5604930f137p-58 } },  // c = 1 + (-19) / 128
			{ 0x1.29e4129e4129ep+0, { -0x1.365fcb0159014p-3, -0x1.bea08d2dca256p-57 } }, // c = 1 + (-18) / 128
			{ 0x1.27350b8812735p+0, { -0x1.23d712a49c201p-3, -0x1.51c7e9efae297p-57 } }, // c = 1 + (-17) / 128
			{ 0x1.2492492492492p+0, { -0x1.1178e8227e47ap-3, 0x1.0e63a5f01c693p-58 } },  // c = 1 + (-16) / 128
			{ 0x1.21fb78121fb78p+0, { -0x1.fe89139dbd565p-4, 0x1.ac9f4215f9394p-58 } },  // c = 1 + (-15) / 128
			{ 0x1.1f7047dc11f70p+0, { -0x1.da7276384469ep-4, -0x1.401fa71733017p-58 } }, // c = 1 + (-14) / 128
			{ 0x1.1cf06ada2811dp+0, { -0x1.b6ac88dad5b1dp-4, 0x1.002bf768e52d0p-58 } },  // c = 1 + (-13) / 128
			{ 0x1.1a7b9611a7b96p+0, { -0x1.9335e5d594988p-4, 0x1.478a85704ccb7p-58 } },  // c = 1 + (-12) / 128
			{ 0x1.1811811811812p+0, { -0x1.700d30aeac0e8p-4, -0x1.a36a677b4c8b2p-59 } }, // c = 1 + (-11) / 128
			{ 0x1.15b1e5f75270dp+0, { -0x1.4d3115d207eacp-4, -0x1.da7d0b1e10b2fp-60 } }, // c = 1 + (-10) / 128
			{ 0x1.135c81135c811p+0, { -0x1.2aa04a44717a1p-4, -0x1.aea2c72d05c08p-58 } }, // c = 1 + (-9) / 128
			{ 0x1.1111111111111p+0, { -0x1.08598b59e3a06p-4, 0x1.dd7009902bf32p-58 } },  // c = 1 + (-8) / 128
			{ 0x1.0ecf56be69c90p+0, { -0x1.ccb73cdddb2d0p-5, 0x1.e48fb0500efd5p-59 } },  // c = 1 + (-7) / 128
			{ 0x1.0c9714fbcda3bp+0, { -0x1.894aa149fb34bp-5, 0x1.2ba0b44cfaee5p-59 } },  // c = 1 + (-6) / 128
			{ 0x1.0a6810a6810a7p+0, { -0x1.466aed42de3f9p-5, 0x1.9badefe942718p-60 } },  // c = 1 + (-5) / 128
			{ 0x1.0842108421084p+0, { -0x1.0415d89e74440p-5, -0x1.c05cf1d753621p-59 } }, // c = 1 + (-4) / 128
			{ 0x1.0624dd2f1a9fcp+0, { -0x1.8492528c8cac5p-6, 0x1.d192d0619fa68p-60 } },  // c = 1 + (-3) / 128
			{ 0x1.0410410410410p+0, { -0x1.0205658935837p-6, -0x1.27c8e8416e717p-60 } }, // c = 1 + (-2) / 128
			{ 0x1.0204081020408p+0, { -0x1.010157588de69p-7, -0x1.46662d417cecep-62 } }, // c = 1 + (-1) / 128
			{ 0x1.0000000000000p+0, { 0x0p+0, 0x0p+0 } },                                // c = 1 + (0) / 128
			{ 0x1.fc07f01fc07f0p-1, { 0x1.fe02a6b106799p-8, -0x1.e44b7e3711e7fp-67 } },  // c = 1 + (1) / 128
			{ 0x1.f81f81f81f820p-1, { 0x1.fc0a8b0fc03c4p-7, -0x1.83092c5964281p-62 } },  // c = 1 + (2) / 128
			{ 0x1.f44659e4a4271p-1, { 0x1.7b91b07d5b126p-6, -0x1.6d80ab38e9430p-62 } },  // c = 1 + (3) / 128
			{ 0x1.f07c1f07c1f08p-1, { 0x1.f829b0e7832f8p-6, 0x1.33e3f04f1ef25p-60 } },   // c = 1 + (4) / 128
			{ 0x1.ecc07b301ecc0p-1, { 0x1.39e87b9febd68p-5, -0x1.5bfa937f551b7p-59 } },  // c = 1 + (5) / 128
			{ 0x1.e9131abf0b767p-1, { 0x1.77458f632dcffp-5, 0x1.8d3ca87b92968p-63 } },   // c = 1 + (6) / 128
			{ 0x1.e573ac901e574p-1, { 0x1.b42dd711971b9p-5, 0x1.0a34531f67db5p-59 } },   // c = 1 + (7) / 128
			{ 0x1.e1e1e1e1e1e1ep-1, { 0x1.f0a30c01162a8p-5, 0x1.85f325c5bbacdp-59 } },   // c = 1 + (8) / 128
			{ 0x1.de5d6e3f8868ap-1, { 0x1.16536eea37ae3p-4, 0x1.2189705cf74cap-58 } },   // c = 1 + (9) / 128
			{ 0x1.dae6076b981dbp-1, { 0x1.341d7961bd1d0p-4, -0x1.3599f227becbbp-58 } },  // c = 1 + (10) / 128
			{ 0x1.d77b654b82c34p-1, { 0x1.51b073f06183cp-4, -0x1.5b61c65e5741ap-58 } },  // c = 1 + (11) / 128
			{ 0x1.d41d41d41d41dp-1, { 0x1.6f0d28ae56b4ep-4, -0x1.20db323097324p-59 } },  // c = 1 + (12) / 128
			{ 0x1.d0cb58f6ec074p-1, { 0x1.8c345d6319b23p-4, -0x1.294d2f5668495p-58 } },  // c = 1 + (13) / 128
			{ 0x1.cd85689039b0bp-1, { 0x1.a926d3a4ad562p-4, -0x1.d7a16eab1e2adp-59 } },  // c = 1 + (14) / 128
			{ 0x1.ca4b3055ee191p-1, { 0x1.c5e548f5bc743p-4, 0x1.2eb0bf7c0b0d9p-59 } },   // c = 1 + (15) / 128
			{ 0x1.c71c71c71c71cp-1, { 0x1.e27076e2af2eap-4, -0x1.61578001e015ap-60 } },  // c = 1 + (16) / 128
			{ 0x1.c3f8f01c3f8f0p-1, { 0x1.fec9131dbeabcp-4, -0x1.5746b9981b36cp-58 } },  // c = 1 + (17) / 128
			{ 0x1.c0e070381c0e0p-1, { 0x1.0d77e7cd08e5bp-3, 0x1.9a5dc5e9030adp-57 } },   // c = 1 + (18) / 128
			{ 0x1.bdd2b899406f7p-1, { 0x1.1b72ad52f67a2p-3, -0x1.fbe7ee5c69946p-57 } },  // c = 1 + (19) / 128
			{ 0x1.bacf914c1bad0p-1, { 0x1.29552f81ff521p-3, 0x1.301771c407dc0p-57 } },   // c = 1 + (20) / 128
			{ 0x1.b7d6c3dda338bp-1, { 0x1.371fc201e8f75p-3, 0x1.e6cb62af18a02p-62 } },   // c = 1 + (21) / 128
			{ 0x1.b4e81b4e81b4fp-1, { 0x1.44d2b6ccb7d1cp-3, 0x1.7d3d950f87e23p-59 } },   // c = 1 + (22) / 128
			{ 0x1.b2036406c80d9p-1, { 0x1.526e5e3a1b438p-3, -0x1.546ff8a470d3ap-57 } },  // c = 1 + (23) / 128
			{ 0x1.af286bca1af28p-1, { 0x1.5ff3070a793d6p-3, -0x1.bc60efafc6f6cp-58 } },  // c = 1 + (24) / 128
			{ 0x1.ac5701ac5701bp-1, { 0x1.6d60fe719d21bp-3, 0x1.d551d97132e87p-57 } },   // c = 1 + (25) / 128
			{ 0x1.a98ef606a63bep-1, { 0x1.7ab890210d907p-3, -0x1.1072534a57e7dp-57 } },  // c = 1 + (26) / 128
			{ 0x1.a6d01a6d01a6dp-1, { 0x1.87fa06520c911p-3, -0x1.9f7fdbfa08d9ap-57 } },  // c = 1 + (27) / 128
			{ 0x1.a41a41a41a41ap-1, { 0x1.9525a9cf456b6p-3, -0x1.26fb3e2b1d1dap-57 } },  // c = 1 + (28) / 128
			{ 0x1.a16d3f97a4b02p-1, { 0x1.a23bc1fe2b561p-3, 0x1.24dc46c1ea664p-57 } },   // c = 1 + (29) / 128
			{ 0x1.9ec8e951033d9p-1, { 0x1.af3c94e80bff3p-3, 0x1.a3398064df33ep-57 } },   // c = 1 + (30) / 128
			{ 0x1.9c2d14ee4a102p-1, { 0x1.bc286742d8cd4p-3, 0x1.cfce744870f57p-58 } },   // c = 1 + (31) / 128
			{ 0x1.999999999999ap-1, { 0x1.c8ff7c79a9a20p-3, -0x1.4f689f8434011p-57 } },  // c = 1 + (32) / 128
			{ 0x1.970e4f80cb872p-1, { 0x1.d5c216b4fbb94p-3, -0x1.a37794d03657dp-58 } },  // c = 1 + (33) / 128
			{ 0x1.948b0fcd6e9e0p-1, { 0x1.e27076e2af2e8p-3, -0x1.61578001e015ep-59 } },  // c = 1 + (34) / 128
			{ 0x1.920fb49d0e229p-1, { 0x1.ef0adcbdc5935p-3, 0x1.e8637950dc20dp-57 } },   // c = 1 + (35) / 128
			{ 0x1.8f9c18f9c18fap-1, { 0x1.fb9186d5e3e29p-3, 0x1.355519b0de535p-57 } },   // c = 1 + (36) / 128
			{ 0x1.8d3018d3018d3p-1, { 0x1.0402594b4d041p-2, -0x1.08ec217a5022dp-57 } },  // c = 1 + (37) / 128
			{ 0x1.8acb90f6bf3aap-1, { 0x1.0a324e27390e2p-2, 0x1.bdcfde8061c03p-56 } },   // c = 1 + (38) / 128
			{ 0x1.886e5f0abb04ap-1, { 0x1.1058bf9ae4ad4p-2, 0x1.3f415699663ecp-63 } },   // c = 1 + (39) / 128
			{ 0x1.8618618618618p-1, { 0x1.1675cababa60fp-2, 0x1.ce63eab883727p-61 } },   // c = 1 + (40) / 128
			{ 0x1.83c977ab2beddp-1, { 0x1.1c898c16999fbp-2, 0x1.9f1a39d500e3cp-56 } },   // c = 1 + (41) / 128
			{ 0x1.8181818181818p-1, { 0x1.22941fbcf7966p-2, -0x1.dbd7ac258a2bdp-58 } },  // c = 1 + (42) / 128
			{ 0x1.7f405fd017f40p-1, { 0x1.2895a13de86a4p-2, 0x1.7ad24c13f040fp-56 } },   // c = 1 + (43) / 128
			{ 0x1.7d05f417d05f4p-1, { 0x1.2e8e2bae11d31p-2, -0x1.1e99b72bd7bf2p-57 } },  // c = 1 + (44) / 128
			{ 0x1.7ad2208e0ecc3p-1, { 0x1.347dd9a987d56p-2, -0x1.16ea62c048cfbp-56 } },  // c = 1 + (45) / 128
			{ 0x1.78a4c8178a4c8p-1, { 0x1.3a64c556945eap-2, 0x1.cbcd735d03424p-60 } },   // c = 1 + (46) / 128
			{ 0x1.767dce434a9b1p-1, { 0x1.404308686a7e4p-2, -0x1.f79f6c1059cdbp-57 } },  // c = 1 + (47) / 128
			{ 0x1.745d1745d1746p-1, { 0x1.4618bc21c5ec2p-2, -0x1.7a42642661c62p-61 } },  // c = 1 + (48) / 128
			{ 0x1.724287f46debcp-1, { 0x1.4be5f957778a1p-2, -0x1.4b366b609027ap-58 } },  // c = 1 + (49) / 128
			{ 0x1.702e05c0b8170p-1, { 0x1.51aad872df82ep-2, -0x1.d8db0a7cc1543p-56 } },  // c = 1 + (50) / 128
			{ 0x1.6e1f76b4337c7p-1, { 0x1.5767717455a6cp-2, -0x1.fb2a49af933e8p-57 } },  // c = 1 + (51) / 128
			{ 0x1.6c16c16c16c17p-1, { 0x1.5d1bdbf5809cap-2, -0x1.7dc9c7c23801fp-56 } },  // c = 1 + (52) / 128
			{ 0x1.6a13cd1537290p-1, { 0x1.62c82f2b9c796p-2, -0x1.090a0dd59fe35p-58 } },  // c = 1 + (53) / 128
			{ 0x1.6816816816817p-1, { 0x1.686c81e9b14adp-2, 0x1.710af840538e3p-56 } },   // c = 1 + (54) / 128
			{ 0x1.661ec6a5122f9p-1, { 0x1.6e08eaa2ba1e4p-2, -0x1.bfb1b39ca3a0fp-56 } },  // c = 1 + (55) / 128
			{ 0x1.642c8590b2164p-1, { 0x1.739d7f6bbd007p-2, 0x1.ce24c53fad3f0p-58 } },   // c = 1 + (56) / 128
			{ 0x1.623fa77016240p-1, { 0x1.792a55fdd47a1p-2, 0x1.f057691fe9ed7p-56 } },   // c = 1 + (57) / 128
			{ 0x1.6058160581606p-1, { 0x1.7eaf83b82afc2p-2, -0x1.698b43096b576p-59 } },  // c = 1 + (58) / 128
			{ 0x1.5e75bb8d015e7p-1, { 0x1.842d1da1e8b18p-2, 0x1.54ec519784677p-56 } },   // c = 1 + (59) / 128
			{ 0x1.5c9882b931057p-1, { 0x1.89a3386c1425bp-2, 0x1.2d38c40881e0bp-57 } },   // c = 1 + (60) / 128
			{ 0x1.5ac056b015ac0p-1, { 0x1.8f11e873662c8p-2, 0x1.f85da755a61a3p-56 } },   // c = 1 + (61) / 128
			{ 0x1.58ed2308158edp-1, { 0x1.947941c2116fbp-2, 0x1.1266e8a3e8838p-57 } },   // c = 1 + (62) / 128
			{ 0x1.571ed3c506b3ap-1, { 0x1.99d958117e08ap-2, -0x1.315b444ee1f38p-56 } },  // c = 1 + (63) / 128
			{ 0x1.5555555555555p-1, { 0x1.9f323ecbf984dp-2, -0x1.a92e513217f58p-59 } },  // c = 1 + (64) / 128
		} };

		// log(x) for finite x > 0 (subnormals included), accurate to about 2^-69 relative. Pow multiplies this relative
		// error by |y log x|, up to about 745 for a finite result, so the logarithm must be far more accurate than a double.
		static DoubleDouble LogKernel(double x)
		{
			int exponent = 0;
			uint64_t bits = std::bit_cast<uint64_t>(x);
			if (bits < SmallestNormalBits)
			{
				bits = std::bit_cast<uint64_t>(x * 0x1p54);
				exponent = -54;
			}
			exponent += static_cast<int>(bits >> 52) - 1023;
			double mantissa = std::bit_cast<double>((bits & MantissaMask) | 0x3ff0000000000000ull);
			if (mantissa >= 1.5)
			{
				mantissa *= 0.5;
				++exponent;
			}

			// j = round((m - 1) * 128) in [-32, 64]; every step is exact.
			const int index = static_cast<int>(RoundToInteger((mantissa - 1.0) * 128.0));
			const LogTableEntry& entry = LogTable[static_cast<size_t>(index + 32)];
			// r = m * InverseC - 1 exactly (|r| <= 2^-7.5): the product is exact as a double-double and its high part
			// minus 1 is Sterbenz-exact.
			const DoubleDouble product = TwoProduct(mantissa, entry.InverseC);
			const DoubleDouble r = TwoSum(product.Hi - 1.0, product.Lo);
			const double rHi = r.Hi;
			// log1p(r.Hi + r.Lo) = log1p(r.Hi) + log1p(r.Lo / (1 + r.Hi)); |r.Lo| <= 2^-53, so the second term is
			// r.Lo / (1 + r.Hi) to far below 2^-100.
			const double loTerm = r.Lo / (1.0 + rHi);
			// log1p(r.Hi) = r.Hi - r.Hi^2/2 + r.Hi^3 * P(r.Hi), with the square exact as a double-double.
			const DoubleDouble halfSquare = TwoProduct(0.5 * rHi, rHi);
			const double cubic = rHi * rHi * rHi * Horner(rHi, Log1pTaylor);

			// e ln2 + log(1/InverseC) + log1p(r), summed without losing the low bits of any leading term.
			const double e = static_cast<double>(exponent);
			const DoubleDouble first = TwoSum(e * Ln2Hi, entry.LogC.Hi);
			const DoubleDouble second = TwoSum(first.Hi, rHi);
			const DoubleDouble third = TwoSum(second.Hi, -halfSquare.Hi);
			const double lo = (first.Lo + second.Lo + third.Lo) + (e * Ln2Lo + entry.LogC.Lo + ((loTerm - halfSquare.Lo) + cubic));
			return TwoSum(third.Hi, lo);
		}

		// Trigonometric argument reduction: |x| = n pi/2 + r

		static constexpr double TwoOverPi = 0x1.45f306dc9c883p-1; // RN(2 / pi)
		// pi/2 = Part1 + Part2 + Part3 + Part3Tail; each part has 33 significant bits, so n * Part is exact for n < 2^20.
		static constexpr double PiOver2Part1 = 0x1.921fb544p+0;
		static constexpr double PiOver2Part2 = 0x1.0b4611a6p-34;
		static constexpr double PiOver2Part3 = 0x1.3198a2ep-69;
		static constexpr double PiOver2Part3Tail = 0x1.b839a252049c1p-104;
		// Arguments up to here use the Cody-Waite reduction above (n < 2^19); larger ones use Payne-Hanek.
		static constexpr double MediumReductionLimit = 0x1p19;

		// Bits 1 ... 1216 of the binary fraction of 2/pi (2/pi = 0.101000101111...b), most significant first: enough for
		// the largest double exponent plus the 192-bit window ReduceLarge multiplies with.
		// clang-format off
		static constexpr std::array<uint64_t, 19> TwoOverPiBits = {
			0xa2f9836e4e441529ull, 0xfc2757d1f534ddc0ull, 0xdb6295993c439041ull, 0xfe5163abdebbc561ull,
			0xb7246e3a424dd2e0ull, 0x06492eea09d1921cull, 0xfe1deb1cb129a73eull, 0xe88235f52ebb4484ull,
			0xe99c7026b45f7e41ull, 0x3991d639835339f4ull, 0x9c845f8bbdf9283bull, 0x1ff897ffde05980full,
			0xef2f118b5a0a6d1full, 0x6d367ecf27cb09b7ull, 0x4f463f669e5fea2dull, 0x7527bac7ebe5f17bull,
			0x3d0739f78a5292eaull, 0x6bfb5fb11f8d5d08ull, 0x56033046fc7b6babull,
		};
		// clang-format on

		// Cody-Waite reduction for 0 <= x <= MediumReductionLimit: about 150 bits of pi/2, enough for every double in
		// range (the closest double to a multiple of pi/2 there is far above 2^-100).
		static ReducedAngle ReduceMedium(double x)
		{
			const double nearest = RoundToInteger(x * TwoOverPi);
			// x - nearest * Part1 is exact: the product is exact and x is within a factor of 2 of it (Sterbenz).
			const DoubleDouble first = TwoSum(x - nearest * PiOver2Part1, -(nearest * PiOver2Part2));
			const DoubleDouble second = TwoSum(first.Hi, -(nearest * PiOver2Part3));
			const double lo = (first.Lo + second.Lo) - nearest * PiOver2Part3Tail;
			return { TwoSum(second.Hi, lo), static_cast<uint32_t>(static_cast<uint64_t>(nearest) & 3) };
		}

		// The full 128-bit product of a and b, portably.
		static UInt128 Multiply64(uint64_t a, uint64_t b)
		{
			const uint64_t aLow = a & 0xffffffffull;
			const uint64_t aHigh = a >> 32;
			const uint64_t bLow = b & 0xffffffffull;
			const uint64_t bHigh = b >> 32;
			const uint64_t lowLow = aLow * bLow;
			const uint64_t lowHigh = aLow * bHigh;
			const uint64_t highLow = aHigh * bLow;
			const uint64_t middle = (lowLow >> 32) + (lowHigh & 0xffffffffull) + (highLow & 0xffffffffull);
			return { aHigh * bHigh + (lowHigh >> 32) + (highLow >> 32) + (middle >> 32), (middle << 32) | (lowLow & 0xffffffffull) };
		}

		// Payne-Hanek reduction for finite x > MediumReductionLimit: x = mantissa * 2^exponent (a 53-bit integer), and
		// x * 2/pi modulo 4 is mantissa times a 192-bit window of the bits of 2/pi, computed exactly in integers.
		static ReducedAngle ReduceLarge(double x)
		{
			const uint64_t bits = std::bit_cast<uint64_t>(x);
			const uint64_t mantissa = (bits & MantissaMask) | (MantissaMask + 1);
			const int exponent = static_cast<int>((bits & ExponentMask) >> 52) - 1075; // >= -33 here

			// Bit i of 2/pi contributes mantissa * 2^(exponent - i), a multiple of 4 for i <= exponent - 2. The window holds
			// bits firstBit ... firstBit + 191; the bits after it change the fraction by less than 2^-135.
			const int firstBit = std::max(1, exponent - 1);
			const size_t word = static_cast<size_t>(firstBit - 1) / 64;
			const int shift = (firstBit - 1) % 64;
			std::array<uint64_t, 3> window{};
			for (size_t index = 0; index < window.size(); ++index)
			{
				const uint64_t upper = TwoOverPiBits[word + index];
				const uint64_t lower = TwoOverPiBits[word + index + 1];
				window[index] = shift == 0 ? upper : (upper << shift) | (lower >> (64 - shift));
			}

			// product = mantissa * window, 245 bits in little-endian words (the fifth word stays 0 for the reads below).
			const UInt128 low = Multiply64(mantissa, window[2]);
			const UInt128 middle = Multiply64(mantissa, window[1]);
			const UInt128 high = Multiply64(mantissa, window[0]);
			std::array<uint64_t, 5> product{};
			product[0] = low.Low;
			product[1] = low.High + middle.Low;
			const uint64_t carry1 = product[1] < middle.Low ? 1 : 0;
			const uint64_t partial = middle.High + high.Low;
			const uint64_t carry2 = partial < high.Low ? 1 : 0;
			product[2] = partial + carry1;
			const uint64_t carry3 = product[2] < partial ? 1 : 0;
			product[3] = high.High + carry2 + carry3;

			// x * 2/pi = product * 2^-fractionBits (modulo 4).
			const int fractionBits = firstBit + 191 - exponent;
			const auto bitsFrom = [&product](int lowestBit) -> uint64_t
			{
				const size_t index = static_cast<size_t>(lowestBit / 64);
				const int offset = lowestBit % 64;
				return offset == 0 ? product[index] : (product[index] >> offset) | (product[index + 1] << (64 - offset));
			};
			uint32_t quadrant = static_cast<uint32_t>(bitsFrom(fractionBits) & 3);
			uint64_t fractionHigh = bitsFrom(fractionBits - 64);
			uint64_t fractionLow = bitsFrom(fractionBits - 128);

			// A fraction of 1/2 or more rounds the quadrant up and leaves fraction - 1 (two's complement negation).
			const bool negative = (fractionHigh >> 63) != 0;
			if (negative)
			{
				quadrant = (quadrant + 1) & 3;
				fractionLow = ~fractionLow + 1;
				fractionHigh = ~fractionHigh + (fractionLow == 0 ? 1 : 0);
			}
			if (fractionHigh == 0 && fractionLow == 0)
				return { {}, quadrant };

			// |fraction| = (fractionHigh, fractionLow) * 2^-128, normalized so that its top bit is set.
			const int leading = fractionHigh != 0 ? std::countl_zero(fractionHigh) : 64 + std::countl_zero(fractionLow);
			uint64_t top = 0;
			uint64_t rest = 0;
			if (leading == 0)
			{
				top = fractionHigh;
				rest = fractionLow;
			}
			else if (leading < 64)
			{
				top = (fractionHigh << leading) | (fractionLow >> (64 - leading));
				rest = fractionLow << leading;
			}
			else
			{
				top = fractionLow << (leading - 64);
			}
			const double fractionHi = static_cast<double>(top >> 11) * PowerOfTwo(-53 - leading);
			const double fractionLo = static_cast<double>(((top & 0x7ffull) << 53) | (rest >> 11)) * PowerOfTwo(-117 - leading);

			// r = fraction * pi/2.
			const DoubleDouble scaled = TwoProduct(fractionHi, PiOver2.Hi);
			const DoubleDouble r = FastTwoSum(scaled.Hi, scaled.Lo + (fractionHi * PiOver2.Lo + fractionLo * PiOver2.Hi));
			return { negative ? Negate(r) : r, quadrant };
		}

		// Sine and cosine of a reduced argument: r = c + d, sin r = S cos d + C sin d, cos r = C cos d - S sin d

		// sin(j/32) and cos(j/32) for j = 0 ... 26 (26.5/32 > pi/4 plus rounding).
		static constexpr std::array<SinCosTableEntry, 27> SinCosTable = { {
			{ { 0x0p+0, 0x0p+0 }, { 0x1.0000000000000p+0, 0x0p+0 } },                                               // 0/32
			{ { 0x1.ffeaaaeeee86fp-6, -0x1.cd406fb224ae2p-60 }, { 0x1.ffc00155527d3p-1, -0x1.3b54492d89b5bp-55 } }, // 1/32
			{ { 0x1.ffaaaeeed4edbp-5, -0x1.2d16d32684b69p-59 }, { 0x1.ff0015549f4d3p-1, 0x1.328387b99426fp-55 } },  // 2/32
			{ { 0x1.7f701032550e4p-4, 0x1.afc2d1800501ap-60 }, { 0x1.fdc06bf7e6b9bp-1, 0x1.31902b535f8dbp-55 } },   // 3/32
			{ { 0x1.feaaeee86ee36p-4, -0x1.afcb2bcc6f03bp-59 }, { 0x1.fc015527d5bd3p-1, 0x1.b68f35094efb8p-55 } },  // 4/32
			{ { 0x1.3eb312c5d66cbp-3, 0x1.47d666b66cb91p-57 }, { 0x1.f9c340a7cc428p-1, 0x1.c5b6b063b7462p-55 } },   // 5/32
			{ { 0x1.7dc102fbaf2b5p-3, 0x1.5ab50e23c97c3p-59 }, { 0x1.f706bdf9ece1cp-1, -0x1.698c80c36dcb4p-55 } },  // 6/32
			{ { 0x1.bc6f84edc6199p-3, 0x1.9c1a56a7b0cabp-57 }, { 0x1.f3cc7c3b3d16ep-1, -0x1.21a3ad28a3494p-57 } },  // 7/32
			{ { 0x1.faaeed4f31577p-3, -0x1.15d88508e32b8p-57 }, { 0x1.f01549f7deea1p-1, 0x1.d3c1e99e5cafdp-55 } },  // 8/32
			{ { 0x1.1c37d64c6b876p-2, 0x1.46076fe0dcff4p-56 }, { 0x1.ebe214f76efa8p-1, -0x1.02f9f12ba543ep-55 } },  // 9/32
			{ { 0x1.3ad129769d3d8p-2, 0x1.03d550487839ap-63 }, { 0x1.e733ea0193d40p-1, -0x1.6428b3546ce13p-55 } },  // 10/32
			{ { 0x1.591bc9fa2f597p-2, 0x1.7c74bac3fe0cbp-57 }, { 0x1.e20bf49acd6c1p-1, -0x1.660aec7ef636bp-58 } },  // 11/32
			{ { 0x1.7710255764214p-2, -0x1.6ead7314bb6cep-57 }, { 0x1.dc6b7eb995912p-1, 0x1.4b364776dcd35p-58 } },  // 12/32
			{ { 0x1.94a6be9f546c5p-2, -0x1.69ce13e683f58p-56 }, { 0x1.d653f073e4040p-1, -0x1.76236434bec37p-55 } }, // 13/32
			{ { 0x1.b1d8305321617p-2, -0x1.ae242cb99f519p-56 }, { 0x1.cfc6cfa52ad9fp-1, 0x1.8b5b5508f2a0dp-55 } },  // 14/32
			{ { 0x1.ce9d2e3d4a51fp-2, -0x1.2fc8a12dae298p-57 }, { 0x1.c8c5bf8ce1a84p-1, 0x1.ab3d1a1590123p-56 } },  // 15/32
			{ { 0x1.eaee8744b05f0p-2, -0x1.789b43c9b027dp-58 }, { 0x1.c1528065b7d50p-1, -0x1.892111312e828p-55 } }, // 16/32
			{ { 0x1.0362939c69955p-1, -0x1.2d8cd78397b01p-55 }, { 0x1.b96eeef58840ep-1, 0x1.45a3cc78fade0p-58 } },  // 17/32
			{ { 0x1.110d0c4b69c3bp-1, 0x1.d918998809981p-55 }, { 0x1.b11d04162a4c6p-1, 0x1.1dd561efbc0c2p-56 } },   // 18/32
			{ { 0x1.1e7343236574cp-1, 0x1.22a3fa4f41d5ap-56 }, { 0x1.a85ed4373e02dp-1, 0x1.9be06385ec792p-57 } },   // 19/32
			{ { 0x1.2b91dea88421ep-1, -0x1.fa371db216ab0p-55 }, { 0x1.9f368ed912f85p-1, -0x1.1d200c5791606p-55 } }, // 20/32
			{ { 0x1.386597456282bp-1, -0x1.10fada93b07a8p-56 }, { 0x1.95a67e00cb1fdp-1, -0x1.0befda21f862dp-55 } }, // 21/32
			{ { 0x1.44eb381cf386bp-1, -0x1.3ed6c1e6a5505p-55 }, { 0x1.8bb105a5dc900p-1, 0x1.863e03e9474c1p-55 } },  // 22/32
			{ { 0x1.511f9fd7b351cp-1, -0x1.5c0e861c48831p-55 }, { 0x1.8158a31916d5dp-1, -0x1.de8b90b8228dep-57 } }, // 23/32
			{ { 0x1.5cffc16bf8f0dp-1, 0x1.96cb370eb578ap-55 }, { 0x1.769fec655211fp-1, -0x1.827d5cf8c68c5p-57 } },  // 24/32
			{ { 0x1.6888a4e134b2fp-1, -0x1.6b7d37644d5e6p-55 }, { 0x1.6b898fa9efb5dp-1, 0x1.15ac786ccf4b2p-56 } },  // 25/32
			{ { 0x1.73b7680dea578p-1, -0x1.2248306dc12a2p-56 }, { 0x1.6018526f563dfp-1, 0x1.46ca5e0e432d0p-55 } },  // 26/32
		} };

		static KernelArgument PrepareKernel(DoubleDouble r)
		{
			const bool negative = r.Hi < 0.0;
			const DoubleDouble magnitude = negative ? Negate(r) : r;
			const size_t index = std::min(static_cast<size_t>(magnitude.Hi * 32.0 + 0.5), SinCosTable.size() - 1);
			// Sterbenz-exact: magnitude.Hi is within 1/64 of index / 32 (or index is 0).
			const double offset = magnitude.Hi - static_cast<double>(index) * (1.0 / 32.0);
			return { index, { offset, magnitude.Lo }, negative };
		}

		// For |d| <= 1/64: (sin d - d) / d^3 through d^7 and (cos d - 1) / d^2 through d^6, in powers of d^2 (the omitted
		// terms are below 2^-63 of the result).
		static constexpr std::array<double, 3> SinTaylor = { -1.0 / 6.0, 1.0 / 120.0, -1.0 / 5040.0 };
		static constexpr std::array<double, 3> CosTaylor = { -1.0 / 2.0, 1.0 / 24.0, -1.0 / 720.0 };

		static double SinTail(double d, double dSquared)
		{
			return d * dSquared * Horner(dSquared, SinTaylor);
		}

		static double CosTail(double dSquared)
		{
			return dSquared * Horner(dSquared, CosTaylor);
		}

		// sin(R) for the argument's R; the large product C * d is exact.
		static DoubleDouble SinOfKernel(const KernelArgument& argument)
		{
			const SinCosTableEntry& entry = SinCosTable[argument.Index];
			const double d = argument.Offset.Hi;
			const double dLo = argument.Offset.Lo;
			const double dSquared = d * d;
			const DoubleDouble cosTimesD = TwoProduct(entry.Cos.Hi, d);
			const DoubleDouble head = TwoSum(entry.Sin.Hi, cosTimesD.Hi);
			const double lo = head.Lo
				+ (cosTimesD.Lo
					+ (entry.Sin.Lo + entry.Cos.Lo * d + entry.Cos.Hi * (dLo + SinTail(d, dSquared))
						+ entry.Sin.Hi * (CosTail(dSquared) - d * dLo)));
			const DoubleDouble result = TwoSum(head.Hi, lo);
			return argument.Negative ? Negate(result) : result;
		}

		// cos(R) for the argument's R (even in R); the large product S * d is exact.
		static DoubleDouble CosOfKernel(const KernelArgument& argument)
		{
			const SinCosTableEntry& entry = SinCosTable[argument.Index];
			const double d = argument.Offset.Hi;
			const double dLo = argument.Offset.Lo;
			const double dSquared = d * d;
			const DoubleDouble sinTimesD = TwoProduct(entry.Sin.Hi, d);
			const DoubleDouble head = TwoSum(entry.Cos.Hi, -sinTimesD.Hi);
			const double lo = head.Lo
				+ (-sinTimesD.Lo
					+ (entry.Cos.Lo - entry.Sin.Lo * d - entry.Sin.Hi * (dLo + SinTail(d, dSquared))
						+ entry.Cos.Hi * (CosTail(dSquared) - d * dLo)));
			return TwoSum(head.Hi, lo);
		}

		// For finite x with |x| >= TinyArgument.
		static SinCosArgument PrepareSinCos(double x)
		{
			const double magnitude = std::fabs(x);
			const ReducedAngle reduced = magnitude <= MediumReductionLimit ? ReduceMedium(magnitude) : ReduceLarge(magnitude);
			return { PrepareKernel(reduced.R), reduced.Quadrant, x < 0.0 };
		}

		// sin |x| from the quadrant: sin(n pi/2 + r) cycles through sin r, cos r, -sin r, -cos r.
		static double SinOfMagnitude(const SinCosArgument& argument)
		{
			switch (argument.Quadrant)
			{
				case 0:  return SinOfKernel(argument.Kernel).Hi;
				case 1:  return CosOfKernel(argument.Kernel).Hi;
				case 2:  return -SinOfKernel(argument.Kernel).Hi;
				default: return -CosOfKernel(argument.Kernel).Hi;
			}
		}

		static double SinOfArgument(const SinCosArgument& argument)
		{
			const double result = SinOfMagnitude(argument);
			return argument.Negative ? -result : result;
		}

		// cos x = cos |x|: cos(n pi/2 + r) cycles through cos r, -sin r, -cos r, sin r.
		static double CosOfArgument(const SinCosArgument& argument)
		{
			switch (argument.Quadrant)
			{
				case 0:  return CosOfKernel(argument.Kernel).Hi;
				case 1:  return -SinOfKernel(argument.Kernel).Hi;
				case 2:  return -CosOfKernel(argument.Kernel).Hi;
				default: return SinOfKernel(argument.Kernel).Hi;
			}
		}

		// Arc tangent: atan t = atan c + atan((t - c) / (1 + t c)) for the table point c = j/32 nearest to t

		// atan(j/32) for j = 0 ... 32.
		static constexpr std::array<DoubleDouble, 33> ATanTable = { {
			{ 0x0p+0, 0x0p+0 },                               // atan(0/32)
			{ 0x1.ffd55bba97625p-6, -0x1.5ec431444912cp-60 }, // atan(1/32)
			{ 0x1.ff55bb72cfdeap-5, -0x1.c934d86d23f1dp-60 }, // atan(2/32)
			{ 0x1.7ee182602f10fp-4, -0x1.cfb654c0c3d98p-58 }, // atan(3/32)
			{ 0x1.fd5ba9aac2f6ep-4, -0x1.cd37686760c17p-59 }, // atan(4/32)
			{ 0x1.3d6eee8c6626cp-3, 0x1.61a3b0ce9281bp-57 },  // atan(5/32)
			{ 0x1.7b97b4bce5b02p-3, 0x1.347b0b4f881cap-58 },  // atan(6/32)
			{ 0x1.b90d7529260a2p-3, 0x1.17b10d2e0e5abp-61 },  // atan(7/32)
			{ 0x1.f5b75f92c80ddp-3, 0x1.8ab6e3cf7afbdp-57 },  // atan(8/32)
			{ 0x1.18bf5a30bf178p-2, 0x1.30ca4748b1bf9p-57 },  // atan(9/32)
			{ 0x1.362773707ebccp-2, -0x1.963a544b672d8p-57 }, // atan(10/32)
			{ 0x1.530ad9951cd4ap-2, -0x1.2566480884082p-57 }, // atan(11/32)
			{ 0x1.6f61941e4def1p-2, -0x1.c63aae6f6e918p-56 }, // atan(12/32)
			{ 0x1.8b24d394a1b25p-2, 0x1.b6d0ba3748fa8p-56 },  // atan(13/32)
			{ 0x1.a64eec3cc23fdp-2, -0x1.24dec1b50b7ffp-56 }, // atan(14/32)
			{ 0x1.c0db4c94ec9f0p-2, -0x1.cc1ce70934c34p-56 }, // atan(15/32)
			{ 0x1.dac670561bb4fp-2, 0x1.a2b7f222f65e2p-56 },  // atan(16/32)
			{ 0x1.f40dd0b541418p-2, -0x1.a3992dc382a23p-57 }, // atan(17/32)
			{ 0x1.0657e94db30d0p-1, -0x1.d5b495f6349e6p-56 }, // atan(18/32)
			{ 0x1.1255d9bfbd2a9p-1, -0x1.2bdaee1c0ee35p-58 }, // atan(19/32)
			{ 0x1.1e00babdefeb4p-1, -0x1.928df287a668fp-58 }, // atan(20/32)
			{ 0x1.2958e59308e31p-1, -0x1.09e73b0c6c087p-56 }, // atan(21/32)
			{ 0x1.345f01cce37bbp-1, 0x1.1021137c71102p-55 },  // atan(22/32)
			{ 0x1.3f13fb89e96f4p-1, 0x1.ecf8b492644f0p-56 },  // atan(23/32)
			{ 0x1.4978fa3269ee1p-1, 0x1.2419a87f2a458p-56 },  // atan(24/32)
			{ 0x1.538f57b89061fp-1, -0x1.1bb74abda520cp-55 }, // atan(25/32)
			{ 0x1.5d58987169b18p-1, 0x1.0028e4bc5e7cap-57 },  // atan(26/32)
			{ 0x1.66d663923e087p-1, -0x1.6ea6febe8bbbap-56 }, // atan(27/32)
			{ 0x1.700a7c5784634p-1, -0x1.8c34d25aadef6p-56 }, // atan(28/32)
			{ 0x1.78f6bbd5d315ep-1, 0x1.406a089803740p-55 },  // atan(29/32)
			{ 0x1.819d0b7158a4dp-1, -0x1.bf76229d3b917p-56 }, // atan(30/32)
			{ 0x1.89ff5ff57f1f8p-1, -0x1.55b9a5e177a1bp-55 }, // atan(31/32)
			{ 0x1.921fb54442d18p-1, 0x1.1a62633145c07p-55 },  // atan(32/32)
		} };

		// (atan u - u) / u^3 through u^9, in powers of u^2 (the omitted u^11 / 11 is below 2^-63 of u for |u| <= 1/64).
		static constexpr std::array<double, 4> ATanTaylor = { -1.0 / 3.0, 1.0 / 5.0, -1.0 / 7.0, 1.0 / 9.0 };

		// atan(t) for a double-double 0 <= t <= 1 (plus rounding).
		static DoubleDouble ATanKernel(DoubleDouble t)
		{
			const size_t index = std::min(static_cast<size_t>(t.Hi * 32.0 + 0.5), ATanTable.size() - 1);
			const double c = static_cast<double>(index) * (1.0 / 32.0);
			// u = (t - c) / (1 + t c) in double-double; t.Hi - c is Sterbenz-exact. |u| <= 1/64 plus rounding.
			const DoubleDouble numerator = TwoSum(t.Hi - c, t.Lo);
			const DoubleDouble product = TwoProduct(t.Hi, c);
			const DoubleDouble onePlusProduct = TwoSum(1.0, product.Hi);
			const DoubleDouble denominator = FastTwoSum(onePlusProduct.Hi, onePlusProduct.Lo + (product.Lo + t.Lo * c));
			const DoubleDouble u = Divide(numerator, denominator);
			const double uSquared = u.Hi * u.Hi;
			const double tail = u.Hi * uSquared * Horner(uSquared, ATanTaylor);
			const DoubleDouble& entry = ATanTable[index];
			const DoubleDouble head = TwoSum(entry.Hi, u.Hi);
			return TwoSum(head.Hi, head.Lo + (entry.Lo + (u.Lo + tail)));
		}

		// atan(numerator / denominator) in [0, pi/4] for finite 0 < numerator <= denominator.
		static DoubleDouble ATanOfRatio(double numerator, double denominator)
		{
			const double quotient = numerator / denominator;
			// atan q rounds to q here, and q is the correctly rounded quotient (gradual underflow included).
			if (quotient < 0x1p-60)
				return { quotient, 0.0 };
			// Scale both by a power of two (exact: quotient >= 2^-60 keeps the numerator normal) so the double-double
			// division cannot overflow or underflow.
			if (denominator > 0x1p500)
			{
				numerator *= 0x1p-600;
				denominator *= 0x1p-600;
			}
			else if (denominator < 0x1p-500)
			{
				numerator *= 0x1p600;
				denominator *= 0x1p600;
			}
			return ATanKernel(Divide({ numerator, 0.0 }, { denominator, 0.0 }));
		}

		// sqrt(1 - a^2) for 0 <= a <= 1; 1 - a^2 is formed exactly.
		static DoubleDouble SqrtOneMinusSquare(double a)
		{
			const DoubleDouble square = TwoProduct(a, a);
			const DoubleDouble difference = TwoSum(1.0, -square.Hi);
			const DoubleDouble w = TwoSum(difference.Hi, difference.Lo - square.Lo);
			if (w.Hi <= 0.0)
				return {};
			return Sqrt(w);
		}

		// (sinh a - a) / a^3 through a^13 in powers of a^2: 1/3!, 1/5!, ... 1/13! (for |a| < 1/4 the omitted a^15 / 15! is
		// below 2^-68 of a).
		static constexpr std::array<double, 6> SinhTaylor = {
			1.0 / 6.0,
			1.0 / 120.0,
			1.0 / 5040.0,
			1.0 / 362880.0,
			1.0 / 39916800.0,
			1.0 / 6227020800.0,
		};

		// (tanh a - a) / a^3 through a^17 in powers of a^2; the coefficient of a^(2n-1) is 2^2n (2^2n - 1) B_2n / (2n)!
		// (Bernoulli numbers B_2n). For |a| < 1/8 the omitted a^19 term is below 2^-63 of a.
		static constexpr std::array<double, 8> TanhTaylor = {
			-1.0 / 3.0,
			2.0 / 15.0,
			-17.0 / 315.0,
			62.0 / 2835.0,
			-1382.0 / 155925.0,
			21844.0 / 6081075.0,
			-929569.0 / 638512875.0,
			6404582.0 / 10854718875.0,
		};

	}

	// Trigonometric functions

	double DetMath::Sin(double x)
	{
		if (std::isnan(x))
			return x;
		if (std::isinf(x))
			return Utils::QuietNaN;
		if (std::fabs(x) < Utils::TinyArgument)
			return x;
		return Utils::SinOfArgument(Utils::PrepareSinCos(x));
	}

	float DetMath::Sin(float x)
	{
		return static_cast<float>(Sin(static_cast<double>(x)));
	}

	double DetMath::Cos(double x)
	{
		if (std::isnan(x))
			return x;
		if (std::isinf(x))
			return Utils::QuietNaN;
		if (std::fabs(x) < Utils::TinyArgument)
			return 1.0;
		return Utils::CosOfArgument(Utils::PrepareSinCos(x));
	}

	float DetMath::Cos(float x)
	{
		return static_cast<float>(Cos(static_cast<double>(x)));
	}

	double DetMath::Tan(double x)
	{
		if (std::isnan(x))
			return x;
		if (std::isinf(x))
			return Utils::QuietNaN;
		if (std::fabs(x) < Utils::TinyArgument)
			return x;
		const SinCosArgument argument = Utils::PrepareSinCos(x);
		const DoubleDouble sine = Utils::SinOfKernel(argument.Kernel);
		const DoubleDouble cosine = Utils::CosOfKernel(argument.Kernel);
		// tan(n pi/2 + r) is tan r for even n and -cot r for odd n.
		const double result = (argument.Quadrant & 1) == 0 ? Utils::Divide(sine, cosine).Hi : -Utils::Divide(cosine, sine).Hi;
		return argument.Negative ? -result : result;
	}

	float DetMath::Tan(float x)
	{
		return static_cast<float>(Tan(static_cast<double>(x)));
	}

	SinCosResult<double> DetMath::SinCos(double x)
	{
		if (std::isnan(x))
			return { x, x };
		if (std::isinf(x))
			return { Utils::QuietNaN, Utils::QuietNaN };
		if (std::fabs(x) < Utils::TinyArgument)
			return { x, 1.0 };
		const SinCosArgument argument = Utils::PrepareSinCos(x);
		return { Utils::SinOfArgument(argument), Utils::CosOfArgument(argument) };
	}

	SinCosResult<float> DetMath::SinCos(float x)
	{
		const SinCosResult<double> result = SinCos(static_cast<double>(x));
		return { static_cast<float>(result.Sin), static_cast<float>(result.Cos) };
	}

	double DetMath::ASin(double x)
	{
		if (std::isnan(x))
			return x;
		const double a = std::fabs(x);
		if (a > 1.0)
			return Utils::QuietNaN;
		if (a < Utils::TinyArgument)
			return x;
		// asin a = atan2(a, s) with s = sqrt(1 - a^2), always through an atan argument <= 1.
		const DoubleDouble s = Utils::SqrtOneMinusSquare(a);
		DoubleDouble angle;
		if (a <= s.Hi)
			angle = Utils::ATanKernel(Utils::Divide({ a, 0.0 }, s));
		else
			angle = Utils::Subtract(Utils::PiOver2, Utils::ATanKernel(Utils::Divide(s, { a, 0.0 })));
		return x < 0.0 ? -angle.Hi : angle.Hi;
	}

	float DetMath::ASin(float x)
	{
		return static_cast<float>(ASin(static_cast<double>(x)));
	}

	double DetMath::ACos(double x)
	{
		if (std::isnan(x))
			return x;
		const double a = std::fabs(x);
		if (a > 1.0)
			return Utils::QuietNaN;
		// acos |x| = atan2(s, |x|) with s = sqrt(1 - x^2); acos x = pi - acos |x| for negative x.
		const DoubleDouble s = Utils::SqrtOneMinusSquare(a);
		DoubleDouble angle;
		if (s.Hi <= a)
			angle = Utils::ATanKernel(Utils::Divide(s, { a, 0.0 }));
		else
			angle = Utils::Subtract(Utils::PiOver2, Utils::ATanKernel(Utils::Divide({ a, 0.0 }, s)));
		if (x < 0.0)
			angle = Utils::Subtract(Utils::Pi, angle);
		return angle.Hi;
	}

	float DetMath::ACos(float x)
	{
		return static_cast<float>(ACos(static_cast<double>(x)));
	}

	double DetMath::ATan(double x)
	{
		if (std::isnan(x))
			return x;
		const double a = std::fabs(x);
		if (a < Utils::TinyArgument)
			return x;
		double result = Utils::PiOver2.Hi; // atan a rounds to pi/2 for a >= 2^60 and infinity
		if (a <= 1.0)
			result = Utils::ATanKernel({ a, 0.0 }).Hi;
		else if (a < 0x1p60)
			result = Utils::Subtract(Utils::PiOver2, Utils::ATanKernel(Utils::Divide({ 1.0, 0.0 }, { a, 0.0 }))).Hi;
		return x < 0.0 ? -result : result;
	}

	float DetMath::ATan(float x)
	{
		return static_cast<float>(ATan(static_cast<double>(x)));
	}

	double DetMath::ATan2(double y, double x)
	{
		if (std::isnan(y))
			return y;
		if (std::isnan(x))
			return x;

		// Special values (C Annex F); the sign of the result is the sign of y throughout.
		double result = 0.0;
		if (y == 0.0)
			result = std::signbit(x) ? Utils::Pi.Hi : 0.0;
		else if (std::isinf(y))
			result = std::isinf(x) ? (x > 0.0 ? Utils::PiOver4 : Utils::ThreePiOver4) : Utils::PiOver2.Hi;
		else if (x == 0.0)
			result = Utils::PiOver2.Hi;
		else if (std::isinf(x))
			result = x > 0.0 ? 0.0 : Utils::Pi.Hi;
		else
		{
			const double ay = std::fabs(y);
			const double ax = std::fabs(x);
			// The first-quadrant angle through an atan argument <= 1, then reflected into x's half plane.
			DoubleDouble angle;
			if (ay <= ax)
				angle = Utils::ATanOfRatio(ay, ax);
			else
				angle = Utils::Subtract(Utils::PiOver2, Utils::ATanOfRatio(ax, ay));
			if (x < 0.0)
				angle = Utils::Subtract(Utils::Pi, angle);
			result = angle.Hi;
		}
		return std::signbit(y) ? -result : result;
	}

	float DetMath::ATan2(float y, float x)
	{
		return static_cast<float>(ATan2(static_cast<double>(y), static_cast<double>(x)));
	}

	// Hyperbolic functions

	double DetMath::Sinh(double x)
	{
		if (std::isnan(x))
			return x;
		const double a = std::fabs(x);
		if (a < Utils::TinyArgument)
			return x;
		double result = Utils::Infinity;
		if (a < 0.25)
		{
			const double z = a * a;
			result = a + a * z * Utils::Horner(z, Utils::SinhTaylor);
		}
		else if (a < 22.0)
		{
			// (e^a - e^-a) / 2 in double-double: the subtraction loses at most 2 bits of the 2^-66 accuracy.
			const DoubleDouble difference = Utils::Subtract(Utils::ExpDoubleDouble(a), Utils::ExpDoubleDouble(-a));
			result = 0.5 * difference.Hi;
		}
		else if (a < Utils::ExpKernelLimit)
		{
			// e^-a is below 2^-63 of e^a; halving through the exponent overflows only when the result does.
			const ScaledDoubleDouble exponential = Utils::ExpKernel(a, 0.0);
			result = Utils::ScaleByPowerOfTwo(exponential.Value.Hi, exponential.Exponent - 1);
		}
		return x < 0.0 ? -result : result;
	}

	float DetMath::Sinh(float x)
	{
		return static_cast<float>(Sinh(static_cast<double>(x)));
	}

	double DetMath::Cosh(double x)
	{
		if (std::isnan(x))
			return x;
		const double a = std::fabs(x);
		if (a < 22.0)
		{
			const DoubleDouble sum = Utils::Add(Utils::ExpDoubleDouble(a), Utils::ExpDoubleDouble(-a));
			return 0.5 * sum.Hi;
		}
		if (a < Utils::ExpKernelLimit)
		{
			const ScaledDoubleDouble exponential = Utils::ExpKernel(a, 0.0);
			return Utils::ScaleByPowerOfTwo(exponential.Value.Hi, exponential.Exponent - 1);
		}
		return Utils::Infinity;
	}

	float DetMath::Cosh(float x)
	{
		return static_cast<float>(Cosh(static_cast<double>(x)));
	}

	double DetMath::Tanh(double x)
	{
		if (std::isnan(x))
			return x;
		const double a = std::fabs(x);
		if (a < Utils::TinyArgument)
			return x;
		double result = 1.0; // 1 - tanh a < 2^-63 for a >= 22
		if (a < 0.125)
		{
			const double z = a * a;
			result = a + a * z * Utils::Horner(z, Utils::TanhTaylor);
		}
		else if (a < 22.0)
		{
			// (e^2a - 1) / (e^2a + 1) in double-double.
			const DoubleDouble exponential = Utils::ExpDoubleDouble(2.0 * a);
			const DoubleDouble numerator = Utils::Add(exponential, { -1.0, 0.0 });
			const DoubleDouble denominator = Utils::Add(exponential, { 1.0, 0.0 });
			result = Utils::Divide(numerator, denominator).Hi;
		}
		return x < 0.0 ? -result : result;
	}

	float DetMath::Tanh(float x)
	{
		return static_cast<float>(Tanh(static_cast<double>(x)));
	}

	// Exponential, logarithms and power

	double DetMath::Exp(double x)
	{
		if (std::isnan(x))
			return x;
		if (x > Utils::ExpKernelLimit)
			return Utils::Infinity;
		if (x < -Utils::ExpKernelLimit)
			return 0.0;
		const ScaledDoubleDouble exponential = Utils::ExpKernel(x, 0.0);
		return Utils::ScaleByPowerOfTwo(exponential.Value.Hi, exponential.Exponent);
	}

	float DetMath::Exp(float x)
	{
		return static_cast<float>(Exp(static_cast<double>(x)));
	}

	double DetMath::Log(double x)
	{
		if (std::isnan(x))
			return x;
		if (x < 0.0)
			return Utils::QuietNaN;
		if (x == 0.0)
			return -Utils::Infinity;
		if (std::isinf(x))
			return x;
		return Utils::LogKernel(x).Hi;
	}

	float DetMath::Log(float x)
	{
		return static_cast<float>(Log(static_cast<double>(x)));
	}

	double DetMath::Log10(double x)
	{
		if (std::isnan(x))
			return x;
		if (x < 0.0)
			return Utils::QuietNaN;
		if (x == 0.0)
			return -Utils::Infinity;
		if (std::isinf(x))
			return x;
		// log x * log10(e) in double-double: about 2^-64 relative, so exact powers of ten give exact integers.
		const DoubleDouble logarithm = Utils::LogKernel(x);
		const DoubleDouble product = Utils::TwoProduct(logarithm.Hi, Utils::Log10OfE.Hi);
		return product.Hi + (product.Lo + (logarithm.Hi * Utils::Log10OfE.Lo + logarithm.Lo * Utils::Log10OfE.Hi));
	}

	float DetMath::Log10(float x)
	{
		return static_cast<float>(Log10(static_cast<double>(x)));
	}

	double DetMath::Pow(double base, double exponent)
	{
		// Special values (C Annex F), in the order the standard gives them precedence.
		if (exponent == 0.0 || base == 1.0)
			return 1.0;
		if (std::isnan(base))
			return base;
		if (std::isnan(exponent))
			return exponent;

		const bool exponentIsInteger = std::isfinite(exponent) && std::floor(exponent) == exponent;
		// Integers of magnitude 2^53 and above are even.
		const bool exponentIsOdd =
			exponentIsInteger && std::fabs(exponent) < 0x1p53 && std::floor(exponent * 0.5) != exponent * 0.5;
		if (std::isinf(exponent))
		{
			const double magnitude = std::fabs(base);
			if (magnitude == 1.0)
				return 1.0;
			return ((magnitude < 1.0) == (exponent < 0.0)) ? Utils::Infinity : 0.0;
		}
		if (base == 0.0)
		{
			if (exponent < 0.0)
				return exponentIsOdd ? std::copysign(Utils::Infinity, base) : Utils::Infinity;
			return exponentIsOdd ? base : 0.0;
		}
		if (std::isinf(base))
		{
			const double magnitude = exponent < 0.0 ? 0.0 : Utils::Infinity;
			return (base < 0.0 && exponentIsOdd) ? -magnitude : magnitude;
		}

		double sign = 1.0;
		if (base < 0.0)
		{
			if (!exponentIsInteger)
				return Utils::QuietNaN;
			if (exponentIsOdd)
				sign = -1.0;
			base = -base;
		}

		// base^exponent = e^(exponent * log base), with the product in double-double (|log base| >= 2^-53 here).
		const DoubleDouble logarithm = Utils::LogKernel(base);
		if (std::fabs(exponent) > 0x1p900)
			return sign * (((exponent > 0.0) == (logarithm.Hi > 0.0)) ? Utils::Infinity : 0.0);
		const DoubleDouble product = Utils::TwoProduct(exponent, logarithm.Hi);
		const double productLo = product.Lo + exponent * logarithm.Lo;
		if (product.Hi > Utils::ExpKernelLimit)
			return sign * Utils::Infinity;
		if (product.Hi < -Utils::ExpKernelLimit)
			return sign * 0.0;
		const ScaledDoubleDouble exponential = Utils::ExpKernel(product.Hi, productLo);
		return sign * Utils::ScaleByPowerOfTwo(exponential.Value.Hi, exponential.Exponent);
	}

	float DetMath::Pow(float base, float exponent)
	{
		return static_cast<float>(Pow(static_cast<double>(base), static_cast<double>(exponent)));
	}

}
