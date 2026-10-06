#include "TestsPCH.h"

#include "Engine/Core/DetMath.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Random.h"

#include <bit>
#include <cmath>
#include <limits>

// The accuracy domains below are the contract of DetMath.h ("within 2 ULP of the C++ standard library over the domains
// the DetMath tests sample"). The standard library is the reference only here, in a test: engine code on the simulation
// path never calls it (Architecture §4.12).

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

		// Samples are drawn uniformly from [Min, Max), or log-uniformly (10^uniform(Min, Max)) for logarithms, so every
		// magnitude is covered.
		struct UnaryCase
		{
			UnaryFunction Function = UnaryFunction::Sin;
			const char* Name = "";
			double Min = 0.0;
			double Max = 0.0;
			bool LogUniform = false;
		};

	}

	// Maps an IEEE value to an unsigned integer that grows monotonically from -Inf to +Inf.
	template<typename T>
	static uint64_t MonotonicBits(T value)
	{
		using Bits = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
		constexpr Bits SignBit = Bits{ 1 } << (sizeof(T) * 8 - 1);
		const Bits bits = std::bit_cast<Bits>(value);
		return static_cast<uint64_t>((bits & SignBit) != 0 ? static_cast<Bits>(~bits) : static_cast<Bits>(bits | SignBit));
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

	template<typename T>
	static T EvaluateReference(UnaryFunction function, T x)
	{
		switch (function)
		{
			case UnaryFunction::Sin:   return std::sin(x);
			case UnaryFunction::Cos:   return std::cos(x);
			case UnaryFunction::Tan:   return std::tan(x);
			case UnaryFunction::ASin:  return std::asin(x);
			case UnaryFunction::ACos:  return std::acos(x);
			case UnaryFunction::ATan:  return std::atan(x);
			case UnaryFunction::Sinh:  return std::sinh(x);
			case UnaryFunction::Cosh:  return std::cosh(x);
			case UnaryFunction::Tanh:  return std::tanh(x);
			case UnaryFunction::Exp:   return std::exp(x);
			case UnaryFunction::Log:   return std::log(x);
			case UnaryFunction::Log10: return std::log10(x);
		}
		return std::numeric_limits<T>::quiet_NaN();
	}

	// The worst ULP distance over 100,000 seeded samples of `unaryCase`.
	template<typename T>
	static uint64_t WorstUlp(const UnaryCase& unaryCase, uint64_t seed)
	{
		Random random(seed);
		uint64_t worst = 0;
		for (int index = 0; index < 100000; ++index)
		{
			const double draw = random.RangeDouble(unaryCase.Min, unaryCase.Max);
			const T x = static_cast<T>(unaryCase.LogUniform ? std::pow(10.0, draw) : draw);
			worst = std::max(worst, UlpDistance(EvaluateEngine(unaryCase.Function, x), EvaluateReference(unaryCase.Function, x)));
		}
		return worst;
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("DetMath: within 2 ULP of std over seeded tables" * doctest::skip(true))
		{
			SUBCASE("float")
			{
				const std::array<UnaryCase, 12> cases = {
					UnaryCase{ UnaryFunction::Sin, "Sin", -100.0, 100.0, false },
					UnaryCase{ UnaryFunction::Cos, "Cos", -100.0, 100.0, false },
					UnaryCase{ UnaryFunction::Tan, "Tan", -100.0, 100.0, false },
					UnaryCase{ UnaryFunction::ASin, "ASin", -1.0, 1.0, false },
					UnaryCase{ UnaryFunction::ACos, "ACos", -1.0, 1.0, false },
					UnaryCase{ UnaryFunction::ATan, "ATan", -1000.0, 1000.0, false },
					UnaryCase{ UnaryFunction::Sinh, "Sinh", -88.0, 88.0, false },
					UnaryCase{ UnaryFunction::Cosh, "Cosh", -88.0, 88.0, false },
					UnaryCase{ UnaryFunction::Tanh, "Tanh", -20.0, 20.0, false },
					UnaryCase{ UnaryFunction::Exp, "Exp", -87.0, 88.0, false },
					UnaryCase{ UnaryFunction::Log, "Log", -37.0, 38.0, true },
					UnaryCase{ UnaryFunction::Log10, "Log10", -37.0, 38.0, true },
				};
				for (size_t index = 0; index < cases.size(); ++index)
				{
					const uint64_t worst = WorstUlp<float>(cases[index], index + 1);
					INFO("float ", cases[index].Name, ": worst ", worst, " ULP");
					CHECK(worst <= 2);
				}
			}

			SUBCASE("double")
			{
				const std::array<UnaryCase, 12> cases = {
					UnaryCase{ UnaryFunction::Sin, "Sin", -100.0, 100.0, false },
					UnaryCase{ UnaryFunction::Cos, "Cos", -100.0, 100.0, false },
					UnaryCase{ UnaryFunction::Tan, "Tan", -100.0, 100.0, false },
					UnaryCase{ UnaryFunction::ASin, "ASin", -1.0, 1.0, false },
					UnaryCase{ UnaryFunction::ACos, "ACos", -1.0, 1.0, false },
					UnaryCase{ UnaryFunction::ATan, "ATan", -1000.0, 1000.0, false },
					UnaryCase{ UnaryFunction::Sinh, "Sinh", -700.0, 700.0, false },
					UnaryCase{ UnaryFunction::Cosh, "Cosh", -700.0, 700.0, false },
					UnaryCase{ UnaryFunction::Tanh, "Tanh", -40.0, 40.0, false },
					UnaryCase{ UnaryFunction::Exp, "Exp", -700.0, 700.0, false },
					UnaryCase{ UnaryFunction::Log, "Log", -300.0, 300.0, true },
					UnaryCase{ UnaryFunction::Log10, "Log10", -300.0, 300.0, true },
				};
				for (size_t index = 0; index < cases.size(); ++index)
				{
					const uint64_t worst = WorstUlp<double>(cases[index], index + 101);
					INFO("double ", cases[index].Name, ": worst ", worst, " ULP");
					CHECK(worst <= 2);
				}
			}

			SUBCASE("ATan2 and Pow")
			{
				Random random(201);
				uint64_t worstATan2 = 0;
				uint64_t worstPow = 0;
				for (int index = 0; index < 100000; ++index)
				{
					const double y = random.RangeDouble(-100.0, 100.0);
					const double x = random.RangeDouble(-100.0, 100.0);
					const double base = random.RangeDouble(1e-3, 1e3);
					const double exponent = random.RangeDouble(-8.0, 8.0);
					const float yf = static_cast<float>(y);
					const float xf = static_cast<float>(x);
					const float basef = static_cast<float>(base);
					const float exponentf = static_cast<float>(exponent);

					worstATan2 = std::max({
						worstATan2,
						UlpDistance(DetMath::ATan2(y, x), std::atan2(y, x)),
						UlpDistance(DetMath::ATan2(yf, xf), std::atan2(yf, xf)),
					});
					worstPow = std::max({
						worstPow,
						UlpDistance(DetMath::Pow(base, exponent), std::pow(base, exponent)),
						UlpDistance(DetMath::Pow(basef, exponentf), std::pow(basef, exponentf)),
					});
				}
				CHECK(worstATan2 <= 2);
				CHECK(worstPow <= 2);
			}
		}

		TEST_CASE("DetMath: output hash over 1,000,000 seeded inputs matches the committed value" * doctest::skip(true))
		{
			// Stream B records this value from its first passing implementation, in any configuration; afterwards it must
			// never change, and the test must pass identically in Debug and Release (Roadmap M1). A changed hash means
			// simulation results changed for every recorded replay.
			constexpr uint64_t CommittedHash = 0;

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

		TEST_CASE("DetMath: SinCos equals Sin and Cos bit for bit" * doctest::skip(true))
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

		TEST_CASE("DetMath: special values follow C Annex F" * doctest::skip(true))
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

		TEST_CASE("DetMath: Log10 is exact at the representable powers of ten" * doctest::skip(true))
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
	}

}
