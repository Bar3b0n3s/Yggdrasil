#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace Engine {

	// Builds little-endian binary data in memory (cooked artifacts, paks, binary replays, §6.8); the inverse of
	// BinaryReader. Writing cannot fail except by running out of memory. Not thread-safe (one owner).
	class BinaryWriter
	{
	public:
		BinaryWriter() = default;

		void WriteU8(uint8_t value);
		void WriteU16(uint16_t value);
		void WriteU32(uint32_t value);
		void WriteU64(uint64_t value);
		void WriteI8(int8_t value);
		void WriteI16(int16_t value);
		void WriteI32(int32_t value);
		void WriteI64(int64_t value);
		void WriteF32(float value);
		void WriteF64(double value);
		// One byte, 0 or 1.
		void WriteBool(bool value);

		void WriteBytes(std::span<const std::byte> bytes);

		// A uint32 byte length followed by the bytes of `text` (BinaryReader::ReadString). `text` must be valid UTF-8 and
		// shorter than 2^32 bytes (asserted).
		void WriteString(std::string_view text);

		// The bytes of every element, in order (BinaryReader::ReadArray). The object representation is copied verbatim,
		// so T must have no padding bytes: padding is indeterminate and would make cooked payloads, their hashes and pak
		// files differ between runs (§6.8, §15.2). Each cooked struct written this way is declared without implicit
		// padding (explicit padding members are zeroed) and pins its layout with a static_assert on its size next to its
		// definition (for example sizeof(Vertex) == 48).
		template<typename T>
			requires std::is_trivially_copyable_v<T>
		void WriteArray(std::span<const T> elements)
		{
			WriteBytes(std::as_bytes(elements));
		}

		// Overwrites already written bytes at `offset` (offset + size <= GetSize(), asserted): fills in sizes and
		// checksums of a header written before its payload.
		void OverwriteU32(size_t offset, uint32_t value);
		void OverwriteU64(size_t offset, uint64_t value);
		void OverwriteBytes(size_t offset, std::span<const std::byte> bytes);

		// Appends zero bytes until GetSize() is a multiple of `alignment` (a power of two, asserted).
		void AlignTo(size_t alignment);

		[[nodiscard]] size_t GetSize() const { return m_Buffer.size(); }
		[[nodiscard]] std::span<const std::byte> GetData() const { return m_Buffer; }

		// Moves the written bytes out; the writer is empty afterwards.
		[[nodiscard]] Buffer TakeBuffer();
	private:
		Buffer m_Buffer;
	};

}
