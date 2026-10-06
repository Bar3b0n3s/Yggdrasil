#include "EnginePCH.h"
#include "Engine/Core/BinaryWriter.h"

// M1 contract stub (Roadmap rule 3): stream C implements the little-endian writes. Until then nothing is written.

namespace Engine {

	void BinaryWriter::WriteU8(uint8_t /*value*/)
	{
	}

	void BinaryWriter::WriteU16(uint16_t /*value*/)
	{
	}

	void BinaryWriter::WriteU32(uint32_t /*value*/)
	{
	}

	void BinaryWriter::WriteU64(uint64_t /*value*/)
	{
	}

	void BinaryWriter::WriteI8(int8_t /*value*/)
	{
	}

	void BinaryWriter::WriteI16(int16_t /*value*/)
	{
	}

	void BinaryWriter::WriteI32(int32_t /*value*/)
	{
	}

	void BinaryWriter::WriteI64(int64_t /*value*/)
	{
	}

	void BinaryWriter::WriteF32(float /*value*/)
	{
	}

	void BinaryWriter::WriteF64(double /*value*/)
	{
	}

	void BinaryWriter::WriteBool(bool /*value*/)
	{
	}

	void BinaryWriter::WriteBytes(std::span<const std::byte> /*bytes*/)
	{
	}

	void BinaryWriter::WriteString(std::string_view /*text*/)
	{
	}

	void BinaryWriter::OverwriteU32(size_t /*offset*/, uint32_t /*value*/)
	{
	}

	void BinaryWriter::OverwriteU64(size_t /*offset*/, uint64_t /*value*/)
	{
	}

	void BinaryWriter::OverwriteBytes(size_t /*offset*/, std::span<const std::byte> /*bytes*/)
	{
	}

	void BinaryWriter::AlignTo(size_t /*alignment*/)
	{
	}

	Buffer BinaryWriter::TakeBuffer()
	{
		return std::move(m_Buffer);
	}

}
