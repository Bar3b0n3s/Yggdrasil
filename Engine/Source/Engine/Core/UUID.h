#pragma once

#include "Engine/Core/Base.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Engine {

	// A 64-bit identifier (Architecture §4.8): entity IDs, asset handles, prefab-local IDs. 0 is the invalid UUID.
	// The text form is exactly 16 lowercase hexadecimal digits, zero-padded ("00000000000003ff"); parsing accepts either
	// case. UUIDs are never written as JSON numbers (JSON consumers lose precision above 2^53). Ordering and hashing use
	// the numeric value, so sorting by UUID is the canonical order wherever an order is observable. Plain value type:
	// trivially copyable, thread-compatible.
	class UUID
	{
	public:
		static constexpr size_t TextLength = 16;
		// Automation accepts a unique prefix of at least this many hex digits for an entity (§4.8, §13.4).
		static constexpr size_t MinimumPrefixLength = 6;

		constexpr UUID() = default;
		constexpr explicit UUID(uint64_t value)
			: m_Value(value)
		{
		}

		[[nodiscard]] constexpr bool IsValid() const { return m_Value != 0; }
		[[nodiscard]] constexpr uint64_t GetValue() const { return m_Value; }

		// The 16-digit lowercase text form. The invalid UUID formats as "0000000000000000".
		[[nodiscard]] std::string ToString() const;

		// True when `prefix` is a valid prefix (IsValidPrefix) and equals the first prefix.size() digits of ToString(),
		// compared case-insensitively.
		[[nodiscard]] bool MatchesPrefix(std::string_view prefix) const;

		constexpr std::strong_ordering operator<=>(const UUID&) const = default;

		// Parses exactly 16 hexadecimal digits (either case, nothing else: no "0x", no whitespace, no sign). Returns
		// nullopt for any other text. "0000000000000000" parses to the invalid UUID; callers that require a valid ID
		// check IsValid().
		[[nodiscard]] static std::optional<UUID> FromString(std::string_view text);

		// True for MinimumPrefixLength to TextLength hexadecimal digits (either case) and nothing else.
		[[nodiscard]] static bool IsValidPrefix(std::string_view prefix);
	private:
		uint64_t m_Value = 0;
	};

}

template<>
struct std::hash<Engine::UUID>
{
	[[nodiscard]] size_t operator()(const Engine::UUID& uuid) const noexcept
	{
		return std::hash<uint64_t>()(uuid.GetValue());
	}
};

// Formats as the 16-digit text form; no format specification is accepted ("{}").
template<>
struct std::formatter<Engine::UUID, char>
{
	constexpr std::format_parse_context::iterator parse(std::format_parse_context& context)
	{
		return context.begin();
	}

	template<typename FormatContext>
	auto format(const Engine::UUID& uuid, FormatContext& context) const
	{
		return std::format_to(context.out(), "{:016x}", uuid.GetValue());
	}
};
