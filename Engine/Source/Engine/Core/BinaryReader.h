#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// Cooked binaries are little-endian (Architecture §6.8); the engine only targets little-endian hosts.
static_assert(std::endian::native == std::endian::little, "cooked formats assume a little-endian host");

namespace Engine {

	// A bounds-checked little-endian reader over untrusted bytes: cooked artifacts, pak entries, binary replays (§6.8).
	// It never trusts a size or offset it reads: every read checks the remaining length first (with overflow-safe
	// arithmetic) and fails with a Parse error naming the offset and the requested size, leaving the position unchanged.
	// It never asserts on data and never reads out of bounds, whatever the input (fuzzed by "BinaryReader: 10,000 seeded
	// mutations never crash and always return Result"). The viewed bytes must outlive the reader and the spans it returns.
	// Not thread-safe (one owner).
	class BinaryReader
	{
	public:
		explicit BinaryReader(std::span<const std::byte> data);

		[[nodiscard]] size_t GetPosition() const { return m_Position; }
		[[nodiscard]] size_t GetSize() const { return m_Data.size(); }
		[[nodiscard]] size_t GetRemaining() const { return m_Data.size() - m_Position; }
		[[nodiscard]] bool IsAtEnd() const { return m_Position == m_Data.size(); }

		// Moves to an absolute offset (<= GetSize()). Errors: Parse.
		[[nodiscard]] Status Seek(size_t position);
		// Advances by `count` bytes. Errors: Parse.
		[[nodiscard]] Status Skip(size_t count);

		[[nodiscard]] Result<uint8_t> ReadU8();
		[[nodiscard]] Result<uint16_t> ReadU16();
		[[nodiscard]] Result<uint32_t> ReadU32();
		[[nodiscard]] Result<uint64_t> ReadU64();
		[[nodiscard]] Result<int8_t> ReadI8();
		[[nodiscard]] Result<int16_t> ReadI16();
		[[nodiscard]] Result<int32_t> ReadI32();
		[[nodiscard]] Result<int64_t> ReadI64();
		// IEEE 754 bit patterns, returned as stored (NaN and infinities included; validating values is the loader's job).
		[[nodiscard]] Result<float> ReadF32();
		[[nodiscard]] Result<double> ReadF64();
		// One byte that must be 0 or 1. Errors: Parse (truncated or any other value).
		[[nodiscard]] Result<bool> ReadBool();

		// A view of the next `count` bytes. Errors: Parse.
		[[nodiscard]] Result<std::span<const std::byte>> ReadBytes(size_t count);

		// A uint32 byte length followed by that many bytes of UTF-8 (BinaryWriter::WriteString). Errors: Parse (truncated),
		// Validation (invalid UTF-8).
		[[nodiscard]] Result<std::string> ReadString();

		// `count` consecutive trivially copyable elements (vertices, indices), copied out. The byte size count * sizeof(T)
		// is checked for overflow and against the remaining length before anything is allocated, so a corrupt count
		// cannot trigger a huge allocation. Errors: Parse.
		template<typename T>
			requires std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>
		[[nodiscard]] Result<std::vector<T>> ReadArray(size_t count)
		{
			if (count > std::numeric_limits<size_t>::max() / sizeof(T))
				return MakeError(ErrorCode::Parse, "array of {} elements of {} bytes overflows at offset {}", count, sizeof(T), m_Position);
			Result<std::span<const std::byte>> bytes = ReadBytes(count * sizeof(T));
			if (!bytes)
				return std::unexpected(std::move(bytes).error());
			std::vector<T> elements(count);
			if (count != 0)
				std::memcpy(elements.data(), bytes->data(), bytes->size());
			return elements;
		}
	private:
		std::span<const std::byte> m_Data;
		size_t m_Position = 0;
	};

}
