#include "TestsPCH.h"

#include "Engine/Core/BinaryWriter.h"

namespace Engine {

	static std::vector<uint8_t> Bytes(std::span<const std::byte> data)
	{
		std::vector<uint8_t> values;
		for (const std::byte value : data)
			values.push_back(std::to_integer<uint8_t>(value));
		return values;
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("BinaryWriter: values are written little-endian")
		{
			BinaryWriter writer;
			writer.WriteU16(0x1234);
			writer.WriteU32(0xdeadbeef);
			writer.WriteI8(-1);
			writer.WriteF32(1.0f);
			writer.WriteBool(true);
			const std::vector<uint8_t> expected = { 0x34, 0x12, 0xef, 0xbe, 0xad, 0xde, 0xff, 0x00, 0x00, 0x80, 0x3f, 0x01 };
			CHECK(Bytes(writer.GetData()) == expected);
		}

		TEST_CASE("BinaryWriter: strings are a uint32 length followed by the bytes")
		{
			BinaryWriter writer;
			writer.WriteString("ab");
			writer.WriteString("");
			CHECK(Bytes(writer.GetData()) == std::vector<uint8_t>{ 2, 0, 0, 0, 'a', 'b', 0, 0, 0, 0 });
		}

		TEST_CASE("BinaryWriter: Overwrite patches bytes that were already written")
		{
			BinaryWriter writer;
			writer.WriteU32(0);
			writer.WriteU64(0);
			writer.WriteU8(7);
			writer.OverwriteU32(0, 0x01020304);
			writer.OverwriteU64(4, 0x1112131415161718ull);
			const std::array<std::byte, 1> patch = { std::byte{ 9 } };
			writer.OverwriteBytes(12, patch);
			CHECK(Bytes(writer.GetData())
				== std::vector<uint8_t>{ 0x04, 0x03, 0x02, 0x01, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x09 });
		}

		TEST_CASE("BinaryWriter: AlignTo pads with zeros to the next multiple")
		{
			BinaryWriter writer;
			writer.WriteU8(0xaa);
			writer.AlignTo(4);
			CHECK(writer.GetSize() == 4);
			writer.AlignTo(4);
			CHECK(writer.GetSize() == 4);
			writer.WriteU8(0xbb);
			writer.AlignTo(8);
			CHECK(Bytes(writer.GetData()) == std::vector<uint8_t>{ 0xaa, 0, 0, 0, 0xbb, 0, 0, 0 });
		}

		TEST_CASE("BinaryWriter: WriteArray writes every element in order")
		{
			BinaryWriter writer;
			const std::array<uint16_t, 3> values = { 1, 0x0203, 0xffff };
			writer.WriteArray(std::span<const uint16_t>(values));
			CHECK(Bytes(writer.GetData()) == std::vector<uint8_t>{ 1, 0, 3, 2, 0xff, 0xff });
		}

		TEST_CASE("BinaryWriter: AlignTo(1) and empty writes add nothing")
		{
			BinaryWriter writer;
			writer.AlignTo(1);
			writer.WriteBytes({});
			CHECK(writer.GetSize() == 0);
			writer.WriteU8(1);
			writer.AlignTo(1);
			writer.AlignTo(2);
			CHECK(Bytes(writer.GetData()) == std::vector<uint8_t>{ 1, 0 });
		}

		TEST_CASE("BinaryWriter: TakeBuffer moves the bytes out and empties the writer")
		{
			BinaryWriter writer;
			writer.WriteU32(5);
			const Buffer buffer = writer.TakeBuffer();
			CHECK(buffer.size() == 4);
			CHECK(writer.GetSize() == 0);
		}
	}

}
