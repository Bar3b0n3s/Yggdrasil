#include "EnginePCH.h"
#include "Engine/Core/BinaryWriter.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Utf8.h"

#include <bit>

namespace Engine {

	// Cooked binaries are little-endian (Architecture §6.8); the bytes of a value are its object representation on the
	// supported hosts.
	static_assert(std::endian::native == std::endian::little, "cooked formats assume a little-endian host");

	namespace Utils {

		template<typename T>
		static void AppendValue(Buffer& buffer, T value)
		{
			const auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
			buffer.insert(buffer.end(), bytes.begin(), bytes.end());
		}

		template<typename T>
		static void OverwriteValue(Buffer& buffer, size_t offset, T value)
		{
			const auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
			std::memcpy(buffer.data() + offset, bytes.data(), bytes.size());
		}

		// True when [offset, offset + size) lies within `buffer`, without overflowing.
		static bool IsWithin(const Buffer& buffer, size_t offset, size_t size)
		{
			return offset <= buffer.size() && size <= buffer.size() - offset;
		}

	}

	void BinaryWriter::WriteU8(uint8_t value)
	{
		Utils::AppendValue(m_Buffer, value);
	}

	void BinaryWriter::WriteU16(uint16_t value)
	{
		Utils::AppendValue(m_Buffer, value);
	}

	void BinaryWriter::WriteU32(uint32_t value)
	{
		Utils::AppendValue(m_Buffer, value);
	}

	void BinaryWriter::WriteU64(uint64_t value)
	{
		Utils::AppendValue(m_Buffer, value);
	}

	void BinaryWriter::WriteI8(int8_t value)
	{
		Utils::AppendValue(m_Buffer, value);
	}

	void BinaryWriter::WriteI16(int16_t value)
	{
		Utils::AppendValue(m_Buffer, value);
	}

	void BinaryWriter::WriteI32(int32_t value)
	{
		Utils::AppendValue(m_Buffer, value);
	}

	void BinaryWriter::WriteI64(int64_t value)
	{
		Utils::AppendValue(m_Buffer, value);
	}

	void BinaryWriter::WriteF32(float value)
	{
		Utils::AppendValue(m_Buffer, value);
	}

	void BinaryWriter::WriteF64(double value)
	{
		Utils::AppendValue(m_Buffer, value);
	}

	void BinaryWriter::WriteBool(bool value)
	{
		Utils::AppendValue(m_Buffer, static_cast<uint8_t>(value ? 1 : 0));
	}

	void BinaryWriter::WriteBytes(std::span<const std::byte> bytes)
	{
		m_Buffer.insert(m_Buffer.end(), bytes.begin(), bytes.end());
	}

	void BinaryWriter::WriteString(std::string_view text)
	{
		ENGINE_CORE_ASSERT(IsValidUtf8(text), "BinaryWriter::WriteString needs valid UTF-8");
		// A wrong length prefix would corrupt everything written after it, so the bound is checked in Dist as well.
		ENGINE_CORE_VERIFY(text.size() <= std::numeric_limits<uint32_t>::max(), "BinaryWriter::WriteString: {} bytes do not fit a uint32 length",
			text.size());
		WriteU32(static_cast<uint32_t>(text.size()));
		WriteBytes(AsBytes(text));
	}

	void BinaryWriter::OverwriteU32(size_t offset, uint32_t value)
	{
		ENGINE_CORE_ASSERT(Utils::IsWithin(m_Buffer, offset, sizeof(value)), "BinaryWriter::OverwriteU32 at {} is outside the {} written bytes",
			offset, m_Buffer.size());
		if (Utils::IsWithin(m_Buffer, offset, sizeof(value)))
			Utils::OverwriteValue(m_Buffer, offset, value);
	}

	void BinaryWriter::OverwriteU64(size_t offset, uint64_t value)
	{
		ENGINE_CORE_ASSERT(Utils::IsWithin(m_Buffer, offset, sizeof(value)), "BinaryWriter::OverwriteU64 at {} is outside the {} written bytes",
			offset, m_Buffer.size());
		if (Utils::IsWithin(m_Buffer, offset, sizeof(value)))
			Utils::OverwriteValue(m_Buffer, offset, value);
	}

	void BinaryWriter::OverwriteBytes(size_t offset, std::span<const std::byte> bytes)
	{
		ENGINE_CORE_ASSERT(Utils::IsWithin(m_Buffer, offset, bytes.size()), "BinaryWriter::OverwriteBytes of {} bytes at {} is outside the {} written bytes",
			bytes.size(), offset, m_Buffer.size());
		if (Utils::IsWithin(m_Buffer, offset, bytes.size()) && !bytes.empty())
			std::memcpy(m_Buffer.data() + offset, bytes.data(), bytes.size());
	}

	void BinaryWriter::AlignTo(size_t alignment)
	{
		const bool isPowerOfTwo = alignment != 0 && (alignment & (alignment - 1)) == 0;
		ENGINE_CORE_ASSERT(isPowerOfTwo, "BinaryWriter::AlignTo needs a power of two, got {}", alignment);
		if (!isPowerOfTwo)
			return;
		const size_t padding = (alignment - (m_Buffer.size() & (alignment - 1))) & (alignment - 1);
		m_Buffer.insert(m_Buffer.end(), padding, std::byte{ 0 });
	}

	Buffer BinaryWriter::TakeBuffer()
	{
		Buffer buffer = std::move(m_Buffer);
		m_Buffer.clear();
		return buffer;
	}

}
