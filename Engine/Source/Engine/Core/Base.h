#pragma once

#include <memory>
#include <type_traits>
#include <utility>

// Platform. premake defines exactly one ENGINE_PLATFORM_* macro per target system (Dependencies.lua); this checks
// that it agrees with the compiler's own target so a misconfigured project fails at compile time.
#if defined(ENGINE_PLATFORM_WINDOWS) + defined(ENGINE_PLATFORM_LINUX) + defined(ENGINE_PLATFORM_MACOS) != 1
	#error "Exactly one of ENGINE_PLATFORM_WINDOWS, ENGINE_PLATFORM_LINUX and ENGINE_PLATFORM_MACOS must be defined (see Dependencies.lua)."
#endif

#if defined(ENGINE_PLATFORM_WINDOWS) && !(defined(_WIN64) && defined(_M_X64))
	#error "ENGINE_PLATFORM_WINDOWS requires an x64 Windows target."
#elif defined(ENGINE_PLATFORM_LINUX) && !(defined(__linux__) && defined(__x86_64__))
	#error "ENGINE_PLATFORM_LINUX requires an x86-64 Linux target."
#elif defined(ENGINE_PLATFORM_MACOS) && !(defined(__APPLE__) && defined(__aarch64__))
	#error "ENGINE_PLATFORM_MACOS requires an arm64 macOS target."
#endif

// Build configuration. premake defines exactly one of these (Dependencies.lua).
#if defined(ENGINE_DEBUG) + defined(ENGINE_RELEASE) + defined(ENGINE_DIST) != 1
	#error "Exactly one of ENGINE_DEBUG, ENGINE_RELEASE and ENGINE_DIST must be defined (see Dependencies.lua)."
#endif

namespace Engine {

	// Unique ownership, the default (Architecture §4.7).
	template<typename T>
	using Scope = std::unique_ptr<T>;

	template<typename T, typename... Args>
	[[nodiscard]] constexpr Scope<T> CreateScope(Args&&... args)
	{
		return std::make_unique<T>(std::forward<Args>(args)...);
	}

	// Shared ownership, only for genuinely shared immutable data such as loaded assets and job results.
	template<typename T>
	using Ref = std::shared_ptr<T>;

	template<typename T, typename... Args>
	[[nodiscard]] Ref<T> CreateRef(Args&&... args)
	{
		return std::make_shared<T>(std::forward<Args>(args)...);
	}

	// Bit-flag enums (Architecture Appendix A). Opt in with
	//     template<> inline constexpr bool EnableFlagOperators<MyFlags> = true;
	// next to the enum, which must be a scoped enum with an unsigned underlying type.
	template<typename T>
	inline constexpr bool EnableFlagOperators = false;

	template<typename T>
	concept FlagEnum = std::is_scoped_enum_v<T> && std::is_unsigned_v<std::underlying_type_t<T>> && EnableFlagOperators<T>;

	template<FlagEnum T>
	[[nodiscard]] constexpr T operator|(T lhs, T rhs)
	{
		using Underlying = std::underlying_type_t<T>;
		return static_cast<T>(static_cast<Underlying>(std::to_underlying(lhs) | std::to_underlying(rhs)));
	}

	template<FlagEnum T>
	[[nodiscard]] constexpr T operator&(T lhs, T rhs)
	{
		using Underlying = std::underlying_type_t<T>;
		return static_cast<T>(static_cast<Underlying>(std::to_underlying(lhs) & std::to_underlying(rhs)));
	}

	template<FlagEnum T>
	[[nodiscard]] constexpr T operator^(T lhs, T rhs)
	{
		using Underlying = std::underlying_type_t<T>;
		return static_cast<T>(static_cast<Underlying>(std::to_underlying(lhs) ^ std::to_underlying(rhs)));
	}

	template<FlagEnum T>
	[[nodiscard]] constexpr T operator~(T value)
	{
		using Underlying = std::underlying_type_t<T>;
		return static_cast<T>(static_cast<Underlying>(~std::to_underlying(value)));
	}

	template<FlagEnum T>
	constexpr T& operator|=(T& lhs, T rhs)
	{
		lhs = lhs | rhs;
		return lhs;
	}

	template<FlagEnum T>
	constexpr T& operator&=(T& lhs, T rhs)
	{
		lhs = lhs & rhs;
		return lhs;
	}

	// True when every bit of `flag` is set in `value`. An empty `flag` is trivially contained.
	template<FlagEnum T>
	[[nodiscard]] constexpr bool HasFlag(T value, T flag)
	{
		return (std::to_underlying(value) & std::to_underlying(flag)) == std::to_underlying(flag);
	}

}
