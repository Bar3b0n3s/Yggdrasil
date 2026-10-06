#include "EnginePCH.h"
#include "Engine/Core/BinaryReader.h"

#include "Engine/Core/Buffer.h"
#include "Engine/Core/Utf8.h"

namespace Engine {

	namespace Utils {

		static std::unexpected<Error> TruncatedError(size_t position, size_t requested, size_t remaining)
		{
			return MakeError(ErrorCode::Parse, "unexpected end of data: {} bytes requested at offset {}, {} remaining", requested, position,
				remaining);
		}

		// A trivially copyable value stored little-endian at the reader's position (the host is little-endian,
		// static_assert in BinaryReader.h).
		template<typename T>
		static Result<T> ReadValue(BinaryReader& reader)
		{
			ENGINE_TRY_ASSIGN(const std::span<const std::byte> bytes, reader.ReadBytes(sizeof(T)));
			T value{};
			std::memcpy(&value, bytes.data(), sizeof(T));
			return value;
		}

	}

	BinaryReader::BinaryReader(std::span<const std::byte> data)
		: m_Data(data)
	{
	}

	Status BinaryReader::Seek(size_t position)
	{
		if (position > m_Data.size())
			return MakeError(ErrorCode::Parse, "cannot seek to offset {}: the data has {} bytes", position, m_Data.size());
		m_Position = position;
		return {};
	}

	Status BinaryReader::Skip(size_t count)
	{
		if (count > GetRemaining())
			return Utils::TruncatedError(m_Position, count, GetRemaining());
		m_Position += count;
		return {};
	}

	Result<uint8_t> BinaryReader::ReadU8()
	{
		return Utils::ReadValue<uint8_t>(*this);
	}

	Result<uint16_t> BinaryReader::ReadU16()
	{
		return Utils::ReadValue<uint16_t>(*this);
	}

	Result<uint32_t> BinaryReader::ReadU32()
	{
		return Utils::ReadValue<uint32_t>(*this);
	}

	Result<uint64_t> BinaryReader::ReadU64()
	{
		return Utils::ReadValue<uint64_t>(*this);
	}

	Result<int8_t> BinaryReader::ReadI8()
	{
		return Utils::ReadValue<int8_t>(*this);
	}

	Result<int16_t> BinaryReader::ReadI16()
	{
		return Utils::ReadValue<int16_t>(*this);
	}

	Result<int32_t> BinaryReader::ReadI32()
	{
		return Utils::ReadValue<int32_t>(*this);
	}

	Result<int64_t> BinaryReader::ReadI64()
	{
		return Utils::ReadValue<int64_t>(*this);
	}

	Result<float> BinaryReader::ReadF32()
	{
		return Utils::ReadValue<float>(*this);
	}

	Result<double> BinaryReader::ReadF64()
	{
		return Utils::ReadValue<double>(*this);
	}

	Result<bool> BinaryReader::ReadBool()
	{
		const size_t start = m_Position;
		ENGINE_TRY_ASSIGN(const uint8_t value, ReadU8());
		if (value > 1)
		{
			m_Position = start;
			return MakeError(ErrorCode::Parse, "invalid boolean {} at offset {} (expected 0 or 1)", value, start);
		}
		return value == 1;
	}

	Result<std::span<const std::byte>> BinaryReader::ReadBytes(size_t count)
	{
		if (count > GetRemaining())
			return Utils::TruncatedError(m_Position, count, GetRemaining());
		const std::span<const std::byte> bytes = m_Data.subspan(m_Position, count);
		m_Position += count;
		return bytes;
	}

	Result<std::string> BinaryReader::ReadString()
	{
		const size_t start = m_Position;
		ENGINE_TRY_ASSIGN(const uint32_t length, ReadU32());
		Result<std::span<const std::byte>> bytes = ReadBytes(length);
		if (!bytes)
		{
			m_Position = start;
			return MakeError(ErrorCode::Parse, "string of {} bytes at offset {} runs past the end of the data ({} bytes after its length)",
				length, start, GetRemaining() - sizeof(uint32_t));
		}

		const std::string_view text = AsStringView(*bytes);
		const size_t invalidOffset = FindInvalidUtf8(text);
		if (invalidOffset != text.size())
		{
			m_Position = start;
			return MakeError(ErrorCode::Validation, "string at offset {} is not valid UTF-8 (invalid byte sequence at offset {})", start,
				start + sizeof(uint32_t) + invalidOffset);
		}
		return std::string(text);
	}

}
