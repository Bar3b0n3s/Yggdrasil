#include "TestsPCH.h"

#include "Engine/Core/DetMath.h"

#include "Engine/Core/DetMathReferenceData.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Random.h"

#include <bit>
#include <cmath>
#include <limits>

// Accuracy is measured against correctly rounded references: the exact results rounded to nearest, which
// Scripts/GenerateDetMathReference.py computes with exact arithmetic into DetMathReferenceData.h. The platform's math
// library is no oracle. Its accuracy differs between C runtimes, so a bound against it fails on whichever platform has
// the least accurate library (Docs/Decisions/0007-detmath-reference-oracle.md).

namespace Engine {

	namespace {

		enum class UnaryFunction : uint8_t
		{
			Sin,
			Cos,
			Tan,
			ASin,
			ACos,
			ATan,
			Sinh,
			Cosh,
			Tanh,
			Exp,
			Log,
			Log10
		};

		// An argument with the correctly rounded sine, cosine and tangent of its exact value.
		template<typename T>
		struct TrigRow
		{
			T X{};
			T Sin{};
			T Cos{};
			T Tan{};
		};

		// A notable argument and the correctly rounded result.
		struct NotableRow
		{
			UnaryFunction Function = UnaryFunction::Sin;
			double X = 0.0;
			double Expected = 0.0;
		};

		// The unsigned integer type with the size of the floating-point type T.
		template<typename T>
		using BitsOf = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;

	}

	// Maps an IEEE value to an unsigned integer that grows monotonically from -Inf to +Inf.
	template<typename T>
	static uint64_t MonotonicBits(T value)
	{
		constexpr BitsOf<T> SignBit = BitsOf<T>{ 1 } << (sizeof(T) * 8 - 1);
		const BitsOf<T> bits = std::bit_cast<BitsOf<T>>(value);
		return static_cast<uint64_t>((bits & SignBit) != 0 ? static_cast<BitsOf<T>>(~bits) : static_cast<BitsOf<T>>(bits | SignBit));
	}

	// Distance in units in the last place; 0 when both are NaN, the maximum when only one is (+0 and -0 are 1 apart).
	template<typename T>
	static uint64_t UlpDistance(T a, T b)
	{
		if (std::isnan(a) || std::isnan(b))
			return std::isnan(a) && std::isnan(b) ? 0 : std::numeric_limits<uint64_t>::max();
		const uint64_t first = MonotonicBits(a);
		const uint64_t second = MonotonicBits(b);
		return first > second ? first - second : second - first;
	}

	static std::string UnaryFunctionName(UnaryFunction function)
	{
		switch (function)
		{
			case UnaryFunction::Sin:   return "Sin";
			case UnaryFunction::Cos:   return "Cos";
			case UnaryFunction::Tan:   return "Tan";
			case UnaryFunction::ASin:  return "ASin";
			case UnaryFunction::ACos:  return "ACos";
			case UnaryFunction::ATan:  return "ATan";
			case UnaryFunction::Sinh:  return "Sinh";
			case UnaryFunction::Cosh:  return "Cosh";
			case UnaryFunction::Tanh:  return "Tanh";
			case UnaryFunction::Exp:   return "Exp";
			case UnaryFunction::Log:   return "Log";
			case UnaryFunction::Log10: return "Log10";
		}
		return "unknown";
	}

	template<typename T>
	static T EvaluateEngine(UnaryFunction function, T x)
	{
		switch (function)
		{
			case UnaryFunction::Sin:   return DetMath::Sin(x);
			case UnaryFunction::Cos:   return DetMath::Cos(x);
			case UnaryFunction::Tan:   return DetMath::Tan(x);
			case UnaryFunction::ASin:  return DetMath::ASin(x);
			case UnaryFunction::ACos:  return DetMath::ACos(x);
			case UnaryFunction::ATan:  return DetMath::ATan(x);
			case UnaryFunction::Sinh:  return DetMath::Sinh(x);
			case UnaryFunction::Cosh:  return DetMath::Cosh(x);
			case UnaryFunction::Tanh:  return DetMath::Tanh(x);
			case UnaryFunction::Exp:   return DetMath::Exp(x);
			case UnaryFunction::Log:   return DetMath::Log(x);
			case UnaryFunction::Log10: return DetMath::Log10(x);
		}
		return std::numeric_limits<T>::quiet_NaN();
	}

	// "0x3ff8000000000000 (1.5)": the bit pattern, then the shortest decimal that round-trips the value.
	template<typename T>
	static std::string DescribeValue(T value)
	{
		return std::format("{:#0{}x} ({})", std::bit_cast<BitsOf<T>>(value), sizeof(T) * 2 + 2, value);
	}

	template<typename T, typename Bits, typename Evaluate>
	static T EvaluateReference(const Test::DetMathUnaryReference<Bits>& reference, Evaluate evaluate)
	{
		return evaluate(std::bit_cast<T>(reference.X));
	}

	template<typename T, typename Bits, typename Evaluate>
	static T EvaluateReference(const Test::DetMathBinaryReference<Bits>& reference, Evaluate evaluate)
	{
		return evaluate(std::bit_cast<T>(reference.First), std::bit_cast<T>(reference.Second));
	}

	template<typename T, typename Bits>
	static std::string DescribeArguments(const Test::DetMathUnaryReference<Bits>& reference)
	{
		return DescribeValue(std::bit_cast<T>(reference.X));
	}

	template<typename T, typename Bits>
	static std::string DescribeArguments(const Test::DetMathBinaryReference<Bits>& reference)
	{
		return std::format("{}, {}", DescribeValue(std::bit_cast<T>(reference.First)), DescribeValue(std::bit_cast<T>(reference.Second)));
	}

	// Checks every case of one reference table: `evaluate` must be within 1 ULP of the correctly rounded result. A failure
	// names the function and lists the first failing cases with the bit patterns of the arguments and of both results.
	template<typename T, typename Reference, size_t N, typename Evaluate>
	static void CheckReferences(const std::string& name, const std::array<Reference, N>& references, Evaluate evaluate)
	{
		constexpr size_t ListedFailures = 8;
		size_t failures = 0;
		uint64_t worst = 0;
		std::string listed;
		for (const Reference& reference : references)
		{
			const T actual = EvaluateReference<T>(reference, evaluate);
			const T expected = std::bit_cast<T>(reference.Expected);
			const uint64_t distance = UlpDistance(actual, expected);
			worst = std::max(worst, distance);
			if (distance <= 1)
				continue;
			if (failures < ListedFailures)
			{
				listed += std::format("\n  {}({}) = {}, correctly rounded {}: {} ULP apart", name, DescribeArguments<T>(reference),
					DescribeValue(actual), DescribeValue(expected), distance);
			}
			++failures;
		}
		const std::string summary = std::format("{}: {} of {} cases are more than 1 ULP from the correctly rounded result (worst {} ULP){}",
			name, failures, N, worst, listed);
		INFO(summary);
		CHECK(failures == 0);
	}

	// Checks a unary table through EvaluateEngine, named like "Sin(float)".
	template<typename T, typename Reference, size_t N>
	static void CheckUnaryReferences(UnaryFunction function, const std::array<Reference, N>& references)
	{
		const std::string name = std::format("{}({})", UnaryFunctionName(function), sizeof(T) == 4 ? "float" : "double");
		CheckReferences<T>(name, references, [function](T x)
		{
			return EvaluateEngine(function, x);
		});
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("DetMath: within 1 ULP of correctly rounded references over seeded tables")
		{
			// DetMath.h promises 1 ULP. Its double results are the correctly rounded value or, within 0.0005 ULP of a halfway
			// point, its neighbour (Exp's subnormal results are within 0.72 ULP), and a float result rounds such a double
			// result once more, which keeps it within 1 ULP of the correctly rounded float. SinCos is checked on the Sin and
			// Cos tables.
			SUBCASE("float")
			{
				CheckUnaryReferences<float>(UnaryFunction::Sin, Test::DetMathSinFloat);
				CheckUnaryReferences<float>(UnaryFunction::Cos, Test::DetMathCosFloat);
				CheckUnaryReferences<float>(UnaryFunction::Tan, Test::DetMathTanFloat);
				CheckUnaryReferences<float>(UnaryFunction::ASin, Test::DetMathASinFloat);
				CheckUnaryReferences<float>(UnaryFunction::ACos, Test::DetMathACosFloat);
				CheckUnaryReferences<float>(UnaryFunction::ATan, Test::DetMathATanFloat);
				CheckUnaryReferences<float>(UnaryFunction::Sinh, Test::DetMathSinhFloat);
				CheckUnaryReferences<float>(UnaryFunction::Cosh, Test::DetMathCoshFloat);
				CheckUnaryReferences<float>(UnaryFunction::Tanh, Test::DetMathTanhFloat);
				CheckUnaryReferences<float>(UnaryFunction::Exp, Test::DetMathExpFloat);
				CheckUnaryReferences<float>(UnaryFunction::Log, Test::DetMathLogFloat);
				CheckUnaryReferences<float>(UnaryFunction::Log10, Test::DetMathLog10Float);
				CheckReferences<float>("SinCos(float).Sin", Test::DetMathSinFloat, [](float x)
				{
					return DetMath::SinCos(x).Sin;
				});
				CheckReferences<float>("SinCos(float).Cos", Test::DetMathCosFloat, [](float x)
				{
					return DetMath::SinCos(x).Cos;
				});
				CheckReferences<float>("ATan2(float)", Test::DetMathATan2Float, [](float y, float x)
				{
					return DetMath::ATan2(y, x);
				});
				CheckReferences<float>("Pow(float)", Test::DetMathPowFloat, [](float base, float exponent)
				{
					return DetMath::Pow(base, exponent);
				});
			}

			SUBCASE("double")
			{
				CheckUnaryReferences<double>(UnaryFunction::Sin, Test::DetMathSinDouble);
				CheckUnaryReferences<double>(UnaryFunction::Cos, Test::DetMathCosDouble);
				CheckUnaryReferences<double>(UnaryFunction::Tan, Test::DetMathTanDouble);
				CheckUnaryReferences<double>(UnaryFunction::ASin, Test::DetMathASinDouble);
				CheckUnaryReferences<double>(UnaryFunction::ACos, Test::DetMathACosDouble);
				CheckUnaryReferences<double>(UnaryFunction::ATan, Test::DetMathATanDouble);
				CheckUnaryReferences<double>(UnaryFunction::Sinh, Test::DetMathSinhDouble);
				CheckUnaryReferences<double>(UnaryFunction::Cosh, Test::DetMathCoshDouble);
				CheckUnaryReferences<double>(UnaryFunction::Tanh, Test::DetMathTanhDouble);
				CheckUnaryReferences<double>(UnaryFunction::Exp, Test::DetMathExpDouble);
				CheckUnaryReferences<double>(UnaryFunction::Log, Test::DetMathLogDouble);
				CheckUnaryReferences<double>(UnaryFunction::Log10, Test::DetMathLog10Double);
				CheckReferences<double>("SinCos(double).Sin", Test::DetMathSinDouble, [](double x)
				{
					return DetMath::SinCos(x).Sin;
				});
				CheckReferences<double>("SinCos(double).Cos", Test::DetMathCosDouble, [](double x)
				{
					return DetMath::SinCos(x).Cos;
				});
				CheckReferences<double>("ATan2(double)", Test::DetMathATan2Double, [](double y, double x)
				{
					return DetMath::ATan2(y, x);
				});
				CheckReferences<double>("Pow(double)", Test::DetMathPowDouble, [](double base, double exponent)
				{
					return DetMath::Pow(base, exponent);
				});
			}
		}

		TEST_CASE("DetMath: output hash over 1,000,000 seeded inputs matches the committed value")
		{
			// Recorded from the first passing implementation (M1 stream B); the same value is required in every
			// configuration and on every platform (Roadmap M1, ADR 0003 decision 10). It must never change: a changed hash
			// means simulation results changed for every recorded replay. A mismatch in one configuration only points at
			// floating-point contraction or a fast-math flag in that build (Architecture §2.2).
			constexpr uint64_t CommittedHash = 0x1f03563cdf5ec2cfull;

			Random random(0xde7a7);
			XXH64Hasher hasher(0);
			for (int index = 0; index < 1000000; ++index)
			{
				const double angle = random.RangeDouble(-100.0, 100.0);
				const double unit = random.RangeDouble(-1.0, 1.0);
				const double positive = random.RangeDouble(1e-6, 1e6);
				const double exponent = random.RangeDouble(-8.0, 8.0);
				const double hyperbolic = random.RangeDouble(-20.0, 20.0);
				const float angleF = static_cast<float>(angle);
				const float hyperbolicF = static_cast<float>(hyperbolic);
				const SinCosResult<double> sinCos = DetMath::SinCos(angle);
				const SinCosResult<float> sinCosF = DetMath::SinCos(angleF);

				const std::array<double, 16> doubles = {
					DetMath::Sin(angle),
					DetMath::Cos(angle),
					DetMath::Tan(angle),
					sinCos.Sin,
					sinCos.Cos,
					DetMath::ASin(unit),
					DetMath::ACos(unit),
					DetMath::ATan(angle),
					DetMath::ATan2(unit, angle),
					DetMath::Sinh(hyperbolic),
					DetMath::Cosh(hyperbolic),
					DetMath::Tanh(hyperbolic),
					DetMath::Exp(exponent),
					DetMath::Log(positive),
					DetMath::Log10(positive),
					DetMath::Pow(positive, exponent),
				};
				const std::array<float, 16> floats = {
					DetMath::Sin(angleF),
					DetMath::Cos(angleF),
					DetMath::Tan(angleF),
					sinCosF.Sin,
					sinCosF.Cos,
					DetMath::ASin(static_cast<float>(unit)),
					DetMath::ACos(static_cast<float>(unit)),
					DetMath::ATan(angleF),
					DetMath::ATan2(static_cast<float>(unit), angleF),
					DetMath::Sinh(hyperbolicF),
					DetMath::Cosh(hyperbolicF),
					DetMath::Tanh(hyperbolicF),
					DetMath::Exp(static_cast<float>(exponent)),
					DetMath::Log(static_cast<float>(positive)),
					DetMath::Log10(static_cast<float>(positive)),
					DetMath::Pow(static_cast<float>(positive), static_cast<float>(exponent)),
				};
				for (const double value : doubles)
					hasher.UpdateU64(std::bit_cast<uint64_t>(value));
				for (const float value : floats)
					hasher.UpdateU64(std::bit_cast<uint32_t>(value));
			}

			CHECK(hasher.Digest() == CommittedHash);
		}

		TEST_CASE("DetMath: SinCos equals Sin and Cos bit for bit")
		{
			Random random(41);
			for (int index = 0; index < 10000; ++index)
			{
				const double x = random.RangeDouble(-1000.0, 1000.0);
				const SinCosResult<double> both = DetMath::SinCos(x);
				REQUIRE(std::bit_cast<uint64_t>(both.Sin) == std::bit_cast<uint64_t>(DetMath::Sin(x)));
				REQUIRE(std::bit_cast<uint64_t>(both.Cos) == std::bit_cast<uint64_t>(DetMath::Cos(x)));

				const float xf = static_cast<float>(x);
				const SinCosResult<float> bothF = DetMath::SinCos(xf);
				REQUIRE(std::bit_cast<uint32_t>(bothF.Sin) == std::bit_cast<uint32_t>(DetMath::Sin(xf)));
				REQUIRE(std::bit_cast<uint32_t>(bothF.Cos) == std::bit_cast<uint32_t>(DetMath::Cos(xf)));
			}
		}

		TEST_CASE("DetMath: special values follow C Annex F")
		{
			constexpr double Infinity = std::numeric_limits<double>::infinity();
			constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

			CHECK(std::isnan(DetMath::Sin(NaN)));
			CHECK(std::isnan(DetMath::Sin(Infinity)));
			CHECK(std::isnan(DetMath::Cos(-Infinity)));
			CHECK(std::isnan(DetMath::ASin(1.5)));
			CHECK(std::isnan(DetMath::ACos(-1.5f)));
			CHECK(std::isnan(DetMath::Log(-1.0)));
			CHECK(DetMath::Log(0.0) == -Infinity);
			CHECK(DetMath::Log(-0.0f) == -std::numeric_limits<float>::infinity());
			CHECK(DetMath::Exp(1000.0) == Infinity);
			CHECK(DetMath::Exp(-1000.0) == 0.0);
			CHECK(DetMath::Exp(0.0) == 1.0);
			CHECK(DetMath::Pow(2.0, 10.0) == 1024.0);
			CHECK(DetMath::Pow(-2.0, 3.0) == -8.0);
			CHECK(std::isnan(DetMath::Pow(-2.0, 0.5)));
			CHECK(DetMath::Pow(NaN, 0.0) == 1.0);
			CHECK(DetMath::Pow(1.0, NaN) == 1.0);
			CHECK(std::signbit(DetMath::Sin(-0.0)));
			CHECK(std::signbit(DetMath::ATan2(-0.0, 1.0)));
			CHECK(DetMath::ATan2(0.0, -1.0) == doctest::Approx(3.141592653589793));
			CHECK(DetMath::ATan2(1.0, 0.0) == doctest::Approx(1.5707963267948966));

			CHECK(DetMath::Sinh(Infinity) == Infinity);
			CHECK(DetMath::Sinh(-Infinity) == -Infinity);
			CHECK(std::signbit(DetMath::Sinh(-0.0)));
			CHECK(DetMath::Sinh(1000.0) == Infinity);
			CHECK(DetMath::Cosh(-Infinity) == Infinity);
			CHECK(DetMath::Cosh(0.0) == 1.0);
			CHECK(DetMath::Cosh(-1000.0f) == std::numeric_limits<float>::infinity());
			CHECK(DetMath::Tanh(Infinity) == 1.0);
			CHECK(DetMath::Tanh(-Infinity) == -1.0);
			CHECK(std::signbit(DetMath::Tanh(-0.0f)));
			CHECK(std::isnan(DetMath::Tanh(NaN)));
			CHECK(std::isnan(DetMath::Log10(-1.0)));
			CHECK(DetMath::Log10(0.0) == -Infinity);
			CHECK(DetMath::Log10(-0.0f) == -std::numeric_limits<float>::infinity());
		}

		TEST_CASE("DetMath: Log10 is exact at the representable powers of ten")
		{
			double power = 1.0;
			for (int exponent = 0; exponent <= 22; ++exponent)
			{
				INFO("10^", exponent);
				CHECK(DetMath::Log10(power) == static_cast<double>(exponent));
				power *= 10.0;
			}

			float powerF = 1.0f;
			for (int exponent = 0; exponent <= 10; ++exponent)
			{
				INFO("10^", exponent, " (float)");
				CHECK(DetMath::Log10(powerF) == static_cast<float>(exponent));
				powerF *= 10.0f;
			}

			// The digit-counting idiom of game scripts: floor(log10(n)) + 1 digits.
			CHECK(std::floor(DetMath::Log10(1000.0)) == 3.0);
			CHECK(std::floor(DetMath::Log10(999.0)) == 2.0);
		}

		TEST_CASE("DetMath: huge arguments of Sin, Cos and Tan are reduced exactly")
		{
			// Expected values are the exact results rounded to nearest, from 60-digit arithmetic with a 780-digit pi. The
			// arguments cover both reductions (the Cody-Waite limit is 2^19) and the double that lies closest to a multiple of
			// pi/2, 6381956970095103 * 2^797, whose cosine is about -4.7e-19.
			// clang-format off
			const std::array<TrigRow<double>, 8> rows = { {
				{ 0x1p+19,               0x1.57481ec90fde3p-3,  0x1.f8c1986ca67fap-1,   0x1.5c354a31a846ep-3 },
				{ 0x1.0000000000001p+19, 0x1.57481ecd01616p-3,  0x1.f8c1986c7b96ap-1,   0x1.5c354a35c5e0fp-3 },
				{ 1e6,                   -0x1.6664b2568d867p-2, 0x1.df9df9906d32cp-1,   -0x1.7e9768ab734cp-2 },
				{ 3e9,                   0x1.f958b458cc91bp-1,  -0x1.4917f746fa4fp-3,   -0x1.891b289d24f04p+2 },
				{ 1e22,                  -0x1.b453ab76bf397p-1, 0x1.0be2cef01c8f4p-1,   -0x1.a0f79c1b6b257p+0 },
				{ 1e100,                 -0x1.85c5e5b929359p-2, 0x1.d9757496841f5p-1,   -0x1.a5807d6f76f7dp-2 },
				{ 0x1.6ac5b262ca1ffp+849, 0x1p+0,               -0x1.14ae72e6ba22fp-61, -0x1.d9ba9a7975636p+60 },
				{ 0x1.fffffffffffffp+1023, 0x1.452fc98b34e97p-8, -0x1.fffe62ecfab75p-1, -0x1.4530cfe729484p-8 },
			} };
			const std::array<TrigRow<float>, 3> rowsF = { {
				{ 1e10f,             -0x1.f334c8p-2f, 0x1.bf098ap-1f,  -0x1.1dep-1f },
				{ 1e30f,             -0x1.95136p-1f,  -0x1.392444p-1f, 0x1.4b2876p+0f },
				{ 0x1.fffffep+127f,  -0x1.0b3366p-1f, 0x1.b4bf2cp-1f,  -0x1.393d94p-1f },
			} };
			// clang-format on
			for (const TrigRow<double>& row : rows)
			{
				INFO("x = ", row.X);
				CHECK(UlpDistance(DetMath::Sin(row.X), row.Sin) <= 1);
				CHECK(UlpDistance(DetMath::Cos(row.X), row.Cos) <= 1);
				CHECK(UlpDistance(DetMath::Tan(row.X), row.Tan) <= 1);
				// Sine and tangent are odd, cosine is even, bit for bit.
				CHECK(DetMath::Sin(-row.X) == -DetMath::Sin(row.X));
				CHECK(DetMath::Cos(-row.X) == DetMath::Cos(row.X));
				CHECK(DetMath::Tan(-row.X) == -DetMath::Tan(row.X));
			}
			for (const TrigRow<float>& row : rowsF)
			{
				INFO("x = ", row.X, " (float)");
				CHECK(UlpDistance(DetMath::Sin(row.X), row.Sin) <= 1);
				CHECK(UlpDistance(DetMath::Cos(row.X), row.Cos) <= 1);
				CHECK(UlpDistance(DetMath::Tan(row.X), row.Tan) <= 1);
			}
		}

		TEST_CASE("DetMath: results are correctly rounded at notable arguments")
		{
			constexpr double Pi = 3.141592653589793; // RN(pi)
			const std::array<NotableRow, 17> rows = { {
				{ UnaryFunction::Sin, Pi, 1.2246467991473532e-16 },
				{ UnaryFunction::Cos, Pi, -1.0 },
				{ UnaryFunction::Sin, 0.5, 0.479425538604203 },
				{ UnaryFunction::Cos, 0.5, 0.8775825618903728 },
				{ UnaryFunction::Tan, Pi / 4.0, 0.9999999999999999 },
				{ UnaryFunction::ATan, 1.0, 0.7853981633974483 },
				{ UnaryFunction::ASin, 1.0, 1.5707963267948966 },
				{ UnaryFunction::ACos, -1.0, Pi },
				{ UnaryFunction::ACos, 0.0, 1.5707963267948966 },
				{ UnaryFunction::Exp, 1.0, 2.718281828459045 },
				{ UnaryFunction::Exp, -1.0, 0.36787944117144233 },
				{ UnaryFunction::Log, 2.0, 0.6931471805599453 },
				{ UnaryFunction::Log, 10.0, 2.302585092994046 },
				{ UnaryFunction::Log10, 2.0, 0.3010299956639812 },
				{ UnaryFunction::Sinh, 1.0, 1.1752011936438014 },
				{ UnaryFunction::Cosh, 1.0, 1.5430806348152437 },
				{ UnaryFunction::Tanh, 1.0, 0.7615941559557649 },
			} };
			for (const NotableRow& row : rows)
			{
				INFO(UnaryFunctionName(row.Function), "(", row.X, ")");
				CHECK(EvaluateEngine(row.Function, row.X) == row.Expected);
			}
			CHECK(DetMath::ATan2(1.0, -1.0) == 2.356194490192345);
			CHECK(DetMath::Pow(2.0, 0.5) == 1.4142135623730951);
		}

		TEST_CASE("DetMath: Pow is exact when the result is representable")
		{
			double power = 1.0;
			for (int exponent = 0; exponent <= 22; ++exponent)
			{
				INFO("10^", exponent);
				CHECK(DetMath::Pow(10.0, static_cast<double>(exponent)) == power);
				power *= 10.0;
			}
			for (int exponent = -1074; exponent <= 1023; ++exponent)
			{
				INFO("2^", exponent);
				REQUIRE(DetMath::Pow(2.0, static_cast<double>(exponent)) == std::ldexp(1.0, exponent));
			}
			float powerF = 1.0f;
			for (int exponent = 0; exponent <= 10; ++exponent)
			{
				INFO("10^", exponent, " (float)");
				CHECK(DetMath::Pow(10.0f, static_cast<float>(exponent)) == powerF);
				powerF *= 10.0f;
			}
			CHECK(DetMath::Pow(-3.0, 3.0) == -27.0);
			CHECK(DetMath::Pow(-3.0, 4.0) == 81.0);
			CHECK(DetMath::Pow(4.0, 0.5) == 2.0);
			CHECK(DetMath::Pow(0.25, -1.5) == 8.0);
			CHECK(DetMath::Pow(7.0, 1.0) == 7.0);
			CHECK(DetMath::Pow(0.1, 1.0) == 0.1);
		}

		TEST_CASE("DetMath: overflow and underflow happen where the exact result leaves the format")
		{
			constexpr double Infinity = std::numeric_limits<double>::infinity();
			constexpr double Smallest = std::numeric_limits<double>::denorm_min();

			CHECK(DetMath::Exp(709.78) == doctest::Approx(1.7928227943945155e308));
			CHECK(DetMath::Exp(709.79) == Infinity);
			CHECK(DetMath::Exp(-745.0) == Smallest); // e^-745 is closer to 2^-1074 than to 0
			CHECK(DetMath::Exp(-746.0) == 0.0);
			CHECK(DetMath::Exp(-740.0) > 0.0);
			CHECK(DetMath::Exp(-740.0) < std::numeric_limits<double>::min());
			CHECK(DetMath::Exp(88.7f) == doctest::Approx(3.3259768e38).epsilon(1e-6));
			CHECK(DetMath::Exp(88.8f) == std::numeric_limits<float>::infinity());

			CHECK(std::isfinite(DetMath::Sinh(710.0)));
			CHECK(DetMath::Sinh(711.0) == Infinity);
			CHECK(DetMath::Sinh(-711.0) == -Infinity);
			CHECK(std::isfinite(DetMath::Cosh(-710.0)));
			CHECK(DetMath::Cosh(711.0) == Infinity);

			CHECK(DetMath::Pow(10.0, 308.0) == doctest::Approx(1e308));
			CHECK(DetMath::Pow(10.0, 309.0) == Infinity);
			CHECK(DetMath::Pow(10.0, -320.0) > 0.0);
			CHECK(DetMath::Pow(10.0, -330.0) == 0.0);
			CHECK(DetMath::Pow(2.0, 1024.0) == Infinity);
			CHECK(DetMath::Pow(2.0, -1075.5) == 0.0);
			CHECK(DetMath::Pow(-2.0, 1025.0) == -Infinity);
			CHECK(DetMath::Pow(1.0 + 0x1p-52, 0x1p62) == Infinity);
			CHECK(DetMath::Pow(0.5, 0x1p950) == 0.0);
		}

		TEST_CASE("DetMath: Pow, ATan2 and the other edge cases follow C Annex F")
		{
			constexpr double Infinity = std::numeric_limits<double>::infinity();
			constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
			constexpr double Pi = 3.141592653589793;
			constexpr double PiOver2 = 1.5707963267948966;

			SUBCASE("pow")
			{
				CHECK(DetMath::Pow(0.0, -3.0) == Infinity);
				CHECK(DetMath::Pow(-0.0, -3.0) == -Infinity);
				CHECK(DetMath::Pow(-0.0, -2.0) == Infinity);
				CHECK(DetMath::Pow(-0.0, -0.5) == Infinity);
				CHECK(DetMath::Pow(-0.0, -Infinity) == Infinity);
				CHECK(DetMath::Pow(0.0, 3.0) == 0.0);
				CHECK_FALSE(std::signbit(DetMath::Pow(0.0, 3.0)));
				CHECK(std::signbit(DetMath::Pow(-0.0, 3.0)));
				CHECK_FALSE(std::signbit(DetMath::Pow(-0.0, 2.0)));
				CHECK_FALSE(std::signbit(DetMath::Pow(-0.0, Infinity)));
				CHECK(DetMath::Pow(-1.0, Infinity) == 1.0);
				CHECK(DetMath::Pow(-1.0, -Infinity) == 1.0);
				CHECK(DetMath::Pow(1.0, Infinity) == 1.0);
				CHECK(DetMath::Pow(Infinity, 0.0) == 1.0);
				CHECK(DetMath::Pow(0.5, -Infinity) == Infinity);
				CHECK(DetMath::Pow(-2.0, -Infinity) == 0.0);
				CHECK(DetMath::Pow(-0.5, Infinity) == 0.0);
				CHECK(DetMath::Pow(2.0, Infinity) == Infinity);
				CHECK(DetMath::Pow(-Infinity, -3.0) == 0.0);
				CHECK(std::signbit(DetMath::Pow(-Infinity, -3.0)));
				CHECK_FALSE(std::signbit(DetMath::Pow(-Infinity, -2.0)));
				CHECK(DetMath::Pow(-Infinity, 3.0) == -Infinity);
				CHECK(DetMath::Pow(-Infinity, 2.5) == Infinity);
				CHECK(DetMath::Pow(Infinity, -1.0) == 0.0);
				CHECK(DetMath::Pow(Infinity, 0.5) == Infinity);
				CHECK(std::isnan(DetMath::Pow(-8.0, 1.0 / 3.0)));
				CHECK(std::isnan(DetMath::Pow(NaN, 1.0)));
				CHECK(std::isnan(DetMath::Pow(2.0, NaN)));
				CHECK(DetMath::Pow(-2.0, 0x1p60) == Infinity); // huge integers are even
				CHECK(DetMath::Pow(-0.5f, 3.0f) == -0.125f);
			}

			SUBCASE("atan2")
			{
				CHECK(DetMath::ATan2(0.0, -0.0) == Pi);
				CHECK(DetMath::ATan2(-0.0, -0.0) == -Pi);
				CHECK(DetMath::ATan2(0.0, 0.0) == 0.0);
				CHECK_FALSE(std::signbit(DetMath::ATan2(0.0, 0.0)));
				CHECK(std::signbit(DetMath::ATan2(-0.0, 0.0)));
				CHECK(DetMath::ATan2(-0.0, -1.0) == -Pi);
				CHECK(DetMath::ATan2(-1.0, 0.0) == -PiOver2);
				CHECK(DetMath::ATan2(1.0, -0.0) == PiOver2);
				CHECK(DetMath::ATan2(1.0, -Infinity) == Pi);
				CHECK(DetMath::ATan2(-1.0, -Infinity) == -Pi);
				CHECK(std::signbit(DetMath::ATan2(-1.0, Infinity)));
				CHECK(DetMath::ATan2(1.0, Infinity) == 0.0);
				CHECK(DetMath::ATan2(-Infinity, 1.0) == -PiOver2);
				CHECK(DetMath::ATan2(Infinity, -Infinity) == 2.356194490192345);
				CHECK(DetMath::ATan2(-Infinity, Infinity) == -0.7853981633974483);
				CHECK(std::isnan(DetMath::ATan2(NaN, 1.0)));
				CHECK(std::isnan(DetMath::ATan2(1.0, NaN)));
				// Extreme ratios: the quotient underflows or the angle rounds to +-pi/2.
				CHECK(DetMath::ATan2(1e-300, 1e300) == 0.0);
				CHECK(DetMath::ATan2(1e-300, -1e300) == Pi);
				CHECK(DetMath::ATan2(1e300, 1e-300) == PiOver2);
				CHECK(DetMath::ATan2(0x1p-1074, 0x1p-1073) == doctest::Approx(0.4636476090008061));
				CHECK(DetMath::ATan2(3e300, 4e300) == doctest::Approx(0.6435011087932844));
			}

			SUBCASE("other functions")
			{
				CHECK(DetMath::Exp(-Infinity) == 0.0);
				CHECK(DetMath::Exp(Infinity) == Infinity);
				CHECK(std::isnan(DetMath::Exp(NaN)));
				CHECK(DetMath::Log(Infinity) == Infinity);
				CHECK(DetMath::Log10(Infinity) == Infinity);
				CHECK(DetMath::Log(1.0) == 0.0);
				CHECK_FALSE(std::signbit(DetMath::Log(1.0)));
				CHECK(DetMath::Log(0x1p-1074) == doctest::Approx(-744.4400719213812));
				CHECK(DetMath::ATan(Infinity) == PiOver2);
				CHECK(DetMath::ATan(-Infinity) == -PiOver2);
				CHECK(DetMath::ATan(1e300) == PiOver2);
				CHECK(std::isnan(DetMath::Tan(Infinity)));
				CHECK(std::isnan(DetMath::Cos(NaN)));
				CHECK(std::isnan(DetMath::Cosh(NaN)));
				CHECK(std::isnan(DetMath::Sinh(NaN)));
				CHECK(std::isnan(DetMath::ACos(NaN)));
				CHECK(std::signbit(DetMath::Tan(-0.0)));
				CHECK(std::signbit(DetMath::ASin(-0.0)));
				CHECK(std::signbit(DetMath::ATan(-0.0f)));
				CHECK(DetMath::Cos(-0.0) == 1.0);
				CHECK(DetMath::ACos(1.0) == 0.0);
				CHECK(DetMath::ASin(-1.0) == -PiOver2);
				CHECK(DetMath::Sin(1e-30) == 1e-30);
				CHECK(DetMath::Tanh(0x1p-1074) == 0x1p-1074);

				const SinCosResult<double> infinite = DetMath::SinCos(Infinity);
				CHECK(std::isnan(infinite.Sin));
				CHECK(std::isnan(infinite.Cos));
				const SinCosResult<float> zero = DetMath::SinCos(-0.0f);
				CHECK(std::signbit(zero.Sin));
				CHECK(zero.Cos == 1.0f);
			}
		}

		TEST_CASE("DetMath: odd functions are odd and even functions are even, bit for bit")
		{
			Random random(57);
			for (int index = 0; index < 20000; ++index)
			{
				const double x = random.RangeDouble(-50.0, 50.0);
				const double unit = random.RangeDouble(-1.0, 1.0);
				INFO("x = ", x, ", unit = ", unit);
				REQUIRE(DetMath::Sin(-x) == -DetMath::Sin(x));
				REQUIRE(DetMath::Tan(-x) == -DetMath::Tan(x));
				REQUIRE(DetMath::ATan(-x) == -DetMath::ATan(x));
				REQUIRE(DetMath::Sinh(-x) == -DetMath::Sinh(x));
				REQUIRE(DetMath::Tanh(-x) == -DetMath::Tanh(x));
				REQUIRE(DetMath::ASin(-unit) == -DetMath::ASin(unit));
				REQUIRE(DetMath::ATan2(-unit, x) == -DetMath::ATan2(unit, x));
				REQUIRE(DetMath::Cos(-x) == DetMath::Cos(x));
				REQUIRE(DetMath::Cosh(-x) == DetMath::Cosh(x));
			}
		}

		TEST_CASE("DetMath: SinCos equals Sin and Cos for huge and special arguments")
		{
			const std::array<double, 9> arguments = {
				0x1p19,
				0x1.0000000000001p19,
				1e22,
				-1e300,
				0x1.6ac5b262ca1ffp+849,
				1e-30,
				-0.0,
				0x1p-1074,
				std::numeric_limits<double>::max(),
			};
			for (const double x : arguments)
			{
				INFO("x = ", x);
				const SinCosResult<double> both = DetMath::SinCos(x);
				CHECK(std::bit_cast<uint64_t>(both.Sin) == std::bit_cast<uint64_t>(DetMath::Sin(x)));
				CHECK(std::bit_cast<uint64_t>(both.Cos) == std::bit_cast<uint64_t>(DetMath::Cos(x)));
			}
			const SinCosResult<float> large = DetMath::SinCos(1e30f);
			CHECK(std::bit_cast<uint32_t>(large.Sin) == std::bit_cast<uint32_t>(DetMath::Sin(1e30f)));
			CHECK(std::bit_cast<uint32_t>(large.Cos) == std::bit_cast<uint32_t>(DetMath::Cos(1e30f)));
		}
	}

}
