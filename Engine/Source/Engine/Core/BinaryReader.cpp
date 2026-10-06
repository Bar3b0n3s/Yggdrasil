#include "EnginePCH.h"
#include "Engine/Core/BinaryReader.h"

// M1 contract stub (Roadmap rule 3): stream C implements the bounds-checked reads. ReadArray is inline in
// BinaryReader.h and builds on ReadBytes.

namespace Engine {

	BinaryReader::BinaryReader(std::span<const std::byte> data)
		: m_Data(data)
	{
	}

	Status BinaryReader::Seek(size_t /*position*/)
	{
		return MakeError(ErrorCode::Unsupported, "BinaryReader::Seek is an M1 contract stub");
	}

	Status BinaryReader::Skip(size_t /*count*/)
	{
		return MakeError(ErrorCode::Unsupported, "BinaryReader::Skip is an M1 contract stub");
	}

	Result<uint8_t> BinaryReader::ReadU8()
	{
		return MakeError(ErrorCode::Unsupported, "BinaryReader::ReadU8 is an M1 contract stub");
	}

	Result<uint16_t> BinaryReader::ReadU16()
	{
		return MakeError(ErrorCode::Unsupported, "BinaryReader::ReadU16 is an M1 contract stub");
	}

	Result<uint32_t> BinaryReader::ReadU32()
	{
		return MakeError(ErrorCode::Unsupported, "BinaryReader::ReadU32 is an M1 contract stub");
	}

	Result<uint64_t> BinaryReader::ReadU64()
	{
		return MakeError(ErrorCode::Unsupported, "BinaryReader::ReadU64 is an M1 contract stub");
	}

	Result<int8_t> BinaryReader::ReadI8()
	{
		return MakeError(ErrorCode::Unsupported, "BinaryReader::ReadI8 is an M1 contract stub");
	}

	Result<int16_t> BinaryReader::ReadI16()
	{
		return MakeError(ErrorCode::Unsupported, "BinaryReader::ReadI16 is an M1 contract stub");
	}

	Result<int32_t> BinaryReader::ReadI32()
	{
		return MakeError(ErrorCode::Unsupported, "BinaryReader::ReadI32 is an M1 contract stub");
	}

	Result<int64_t> BinaryReader::ReadI64()
	{
		return MakeError(ErrorCode::Unsupported, "BinaryReader::ReadI64 is an M1 contract stub");
	}

	Result<float> BinaryReader::ReadF32()
	{
		return MakeError(ErrorCode::Unsupported, "BinaryReader::ReadF32 is an M1 contract stub");
	}

	Result<double> BinaryReader::ReadF64()
	{
		return MakeError(ErrorCode::Unsupported, "BinaryReader::ReadF64 is an M1 contract stub");
	}

	Result<bool> BinaryReader::ReadBool()
	{
		return MakeError(ErrorCode::Unsupported, "BinaryReader::ReadBool is an M1 contract stub");
	}

	Result<std::string> BinaryReader::ReadString()
	{
		return MakeError(ErrorCode::Unsupported, "BinaryReader::ReadString is an M1 contract stub");
	}

	Result<std::span<const std::byte>> BinaryReader::ReadBytes(size_t /*count*/)
	{
		return MakeError(ErrorCode::Unsupported, "BinaryReader::ReadBytes is an M1 contract stub");
	}

}
