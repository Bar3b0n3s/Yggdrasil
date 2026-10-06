#include "TestsPCH.h"

#include "Engine/Core/Base.h"

namespace Engine {

	namespace {

		enum class SmallFlags : uint8_t
		{
			None = 0,
			First = 1 << 0,
			Second = 1 << 1,
			Third = 1 << 2,
			Last = 1 << 7,
		};

		enum class WideFlags : uint32_t
		{
			None = 0,
			Low = 1u << 0,
			Middle = 1u << 15,
			High = 1u << 31,
		};

		// Not opted in: no flag operators, although it is a scoped enum with an unsigned underlying type.
		enum class PlainEnum : uint32_t
		{
			Zero,
			One,
		};

		// Opted in below, but the underlying type is signed.
		enum class SignedFlags : int32_t
		{
			None = 0,
			First = 1,
		};

		// Opted in below, but unscoped.
		enum UnscopedFlags : uint32_t
		{
			UnscopedNone = 0,
			UnscopedFirst = 1,
		};

		// Whether the operators exist for T at all (a dependent requires-expression, so a miss is false, not an error).
		template<typename T>
		concept HasBitwiseOr = requires(T value) { value | value; };

		template<typename T>
		concept HasComplement = requires(T value) { ~value; };

		struct Counter
		{
			explicit Counter(int start, Scope<int> step)
				: Value(start), Step(std::move(step))
			{
			}

			int Value = 0;
			Scope<int> Step;
		};

	}

	template<>
	inline constexpr bool EnableFlagOperators<SmallFlags> = true;

	template<>
	inline constexpr bool EnableFlagOperators<WideFlags> = true;

	template<>
	inline constexpr bool EnableFlagOperators<SignedFlags> = true;

	template<>
	inline constexpr bool EnableFlagOperators<UnscopedFlags> = true;

	static_assert(FlagEnum<SmallFlags>);
	static_assert(FlagEnum<WideFlags>);
	static_assert(!FlagEnum<PlainEnum>, "an enum that is not opted in gets no flag operators");
	static_assert(EnableFlagOperators<SignedFlags> && !FlagEnum<SignedFlags>, "a signed underlying type is rejected");
	static_assert(EnableFlagOperators<UnscopedFlags> && !FlagEnum<UnscopedFlags>, "an unscoped enum is rejected");
	static_assert(HasBitwiseOr<SmallFlags> && HasComplement<WideFlags>);
	static_assert(!HasBitwiseOr<PlainEnum>, "operator| is constrained to flag enums");
	static_assert(!HasComplement<SignedFlags>, "operator~ is constrained to flag enums");

	// The operators are usable in constant expressions.
	static_assert((SmallFlags::First | SmallFlags::Second) == static_cast<SmallFlags>(0b11));
	static_assert(HasFlag(WideFlags::Low | WideFlags::High, WideFlags::High));

	TEST_SUITE("Core")
	{
		TEST_CASE("Base: flag operators combine, intersect and toggle bits")
		{
			SUBCASE("8-bit underlying type")
			{
				const SmallFlags both = SmallFlags::First | SmallFlags::Third;
				CHECK(std::to_underlying(both) == 0b101);
				CHECK((both & SmallFlags::Third) == SmallFlags::Third);
				CHECK((both & SmallFlags::Second) == SmallFlags::None);
				CHECK((both ^ SmallFlags::First) == SmallFlags::Third);
				CHECK((both ^ SmallFlags::Second) == (SmallFlags::First | SmallFlags::Second | SmallFlags::Third));
			}

			SUBCASE("32-bit underlying type")
			{
				const WideFlags both = WideFlags::Low | WideFlags::High;
				CHECK(std::to_underlying(both) == 0x80000001u);
				CHECK((both & WideFlags::High) == WideFlags::High);
				CHECK((both & WideFlags::Middle) == WideFlags::None);
				CHECK((both ^ WideFlags::High) == WideFlags::Low);
			}
		}

		TEST_CASE("Base: operator~ complements every bit of the underlying type")
		{
			// The 8-bit complement goes through int promotion; the result must still fit the underlying type.
			CHECK(std::to_underlying(~SmallFlags::None) == 0xFF);
			CHECK(std::to_underlying(~SmallFlags::First) == 0xFE);
			CHECK((~SmallFlags::Last & SmallFlags::Last) == SmallFlags::None);
			CHECK(std::to_underlying(~WideFlags::None) == 0xFFFFFFFFu);
			CHECK(std::to_underlying(~WideFlags::High) == 0x7FFFFFFFu);
		}

		TEST_CASE("Base: compound flag assignment updates and returns the left operand")
		{
			SmallFlags flags = SmallFlags::None;

			SmallFlags& afterOr = (flags |= SmallFlags::Second);
			CHECK(&afterOr == &flags);
			CHECK(flags == SmallFlags::Second);

			flags |= SmallFlags::Last;
			CHECK(std::to_underlying(flags) == 0b10000010);

			SmallFlags& afterAnd = (flags &= ~SmallFlags::Second);
			CHECK(&afterAnd == &flags);
			CHECK(flags == SmallFlags::Last);
		}

		TEST_CASE("Base: HasFlag is true only when every bit of the flag is set")
		{
			const WideFlags value = WideFlags::Low | WideFlags::Middle;

			CHECK(HasFlag(value, WideFlags::Low));
			CHECK(HasFlag(value, WideFlags::Low | WideFlags::Middle));
			CHECK_FALSE(HasFlag(value, WideFlags::High));
			CHECK_FALSE(HasFlag(value, WideFlags::Low | WideFlags::High));

			SUBCASE("an empty flag is contained in every value")
			{
				CHECK(HasFlag(value, WideFlags::None));
				CHECK(HasFlag(WideFlags::None, WideFlags::None));
			}
		}

		TEST_CASE("Base: CreateScope forwards its arguments into a uniquely owned object")
		{
			static_assert(std::is_same_v<decltype(CreateScope<Counter>(1, Scope<int>())), Scope<Counter>>);

			Scope<int> step = CreateScope<int>(7);
			int* stepAddress = step.get();
			Scope<Counter> counter = CreateScope<Counter>(3, std::move(step));

			REQUIRE(counter != nullptr);
			CHECK(counter->Value == 3);
			REQUIRE(counter->Step != nullptr);
			CHECK(counter->Step.get() == stepAddress);
			CHECK(*counter->Step == 7);
			CHECK(step == nullptr);
		}

		TEST_CASE("Base: CreateRef forwards its arguments into a shared object")
		{
			static_assert(std::is_same_v<decltype(CreateRef<std::string>(3, 'x')), Ref<std::string>>);

			const Ref<std::string> text = CreateRef<std::string>(3, 'x');
			REQUIRE(text != nullptr);
			CHECK(*text == "xxx");
			CHECK(text.use_count() == 1);

			const Ref<std::string> shared = text;
			CHECK(shared.get() == text.get());
			CHECK(text.use_count() == 2);
		}
	}

}
