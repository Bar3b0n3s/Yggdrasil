#include "TestsPCH.h"

#include "Engine/Core/BinaryReader.h"

#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/Random.h"

#include <cmath>
#include <limits>

namespace Engine {

	namespace {

		struct Vertex
		{
			float Position[3] = {};
			float Uv[2] = {};
		};

	}

	template<typename T>
	static std::optional<ErrorCode> ErrorCodeOf(const Result<T>& result)
	{
		return result.has_value() ? std::nullopt : std::optional<ErrorCode>(result.error().GetCode());
	}

	// A cooked-artifact-like payload: a 32-byte header in the §6.8 layout, a string table, a vertex array and an index
	// array whose counts come from the data itself.
	static Buffer MakeCookedPayload()
	{
		BinaryWriter writer;
		writer.WriteBytes(AsBytes("ECKD"));
		writer.WriteU16(1); // FormatVersion
		writer.WriteU16(3); // AssetType
		writer.WriteU32(2); // ImporterVersion
		writer.WriteU32(0); // Flags
		const size_t payloadSizeOffset = writer.GetSize();
		writer.WriteU64(0); // PayloadSize, patched below
		writer.WriteU64(0); // PayloadXXH64 (not checked by this test)
		const size_t payloadStart = writer.GetSize();

		writer.WriteU32(2);
		writer.WriteString("Straight");
		writer.WriteString("Curve \xc3\xa9");

		const std::array<Vertex, 3> vertices = {
			Vertex{ { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
			Vertex{ { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f } },
			Vertex{ { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } },
		};
		writer.WriteU32(static_cast<uint32_t>(vertices.size()));
		writer.WriteArray(std::span<const Vertex>(vertices));

		const std::array<uint32_t, 3> indices = { 0, 1, 2 };
		writer.WriteU32(static_cast<uint32_t>(indices.size()));
		writer.WriteArray(std::span<const uint32_t>(indices));
		writer.WriteBool(true);
		writer.WriteF64(0.5);

		writer.OverwriteU64(payloadSizeOffset, writer.GetSize() - payloadStart);
		return writer.TakeBuffer();
	}

	// Parses MakeCookedPayload's layout the way a loader does: every size comes from the data and is trusted only after
	// BinaryReader checked it.
	static Status ParseCookedPayload(std::span<const std::byte> bytes)
	{
		BinaryReader reader(bytes);
		ENGINE_TRY_ASSIGN(const std::span<const std::byte> magic, reader.ReadBytes(4));
		if (AsStringView(magic) != "ECKD")
			return MakeError(ErrorCode::Parse, "bad magic");
		ENGINE_TRY(reader.ReadU16());
		ENGINE_TRY(reader.ReadU16());
		ENGINE_TRY(reader.ReadU32());
		ENGINE_TRY(reader.ReadU32());
		ENGINE_TRY_ASSIGN(const uint64_t payloadSize, reader.ReadU64());
		ENGINE_TRY(reader.ReadU64());
		if (payloadSize != reader.GetRemaining())
			return MakeError(ErrorCode::Parse, "payload size {} does not match {} remaining bytes", payloadSize, reader.GetRemaining());

		ENGINE_TRY_ASSIGN(const uint32_t nameCount, reader.ReadU32());
		for (uint32_t index = 0; index < nameCount; ++index)
			ENGINE_TRY(reader.ReadString());
		ENGINE_TRY_ASSIGN(const uint32_t vertexCount, reader.ReadU32());
		ENGINE_TRY(reader.ReadArray<Vertex>(vertexCount));
		ENGINE_TRY_ASSIGN(const uint32_t indexCount, reader.ReadU32());
		ENGINE_TRY_ASSIGN(const std::vector<uint32_t> indices, reader.ReadArray<uint32_t>(indexCount));
		for (const uint32_t index : indices)
		{
			if (index >= vertexCount)
				return MakeError(ErrorCode::Validation, "index {} out of range", index);
		}
		ENGINE_TRY(reader.ReadBool());
		ENGINE_TRY(reader.ReadF64());
		if (!reader.IsAtEnd())
			return MakeError(ErrorCode::Parse, "{} trailing bytes", reader.GetRemaining());
		return {};
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("BinaryReader: 10,000 seeded mutations never crash and always return Result")
		{
			const Buffer original = MakeCookedPayload();
			REQUIRE(ParseCookedPayload(original).has_value());

			Random random(0xb1a5);
			int failures = 0;
			for (int iteration = 0; iteration < 10000; ++iteration)
			{
				Buffer mutated = original;
				switch (random.RangeInt(0, 3))
				{
					case 0: // flip random bits
					{
						const int64_t flips = random.RangeInt(1, 8);
						for (int64_t flip = 0; flip < flips; ++flip)
						{
							const size_t offset = static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(mutated.size()) - 1));
							mutated[offset] ^= static_cast<std::byte>(1u << random.RangeInt(0, 7));
						}
						break;
					}
					case 1: // truncate
						mutated.resize(static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(mutated.size()) - 1)));
						break;
					case 2: // overwrite a 32-bit field with an extreme value
					{
						const size_t offset = static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(mutated.size()) - 4));
						const uint32_t value = random.NextBool() ? 0xffffffffu : 0x80000000u;
						std::memcpy(mutated.data() + offset, &value, sizeof(value));
						break;
					}
					case 3: // insert random bytes
					{
						const size_t offset = static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(mutated.size())));
						const int64_t count = random.RangeInt(1, 16);
						for (int64_t index = 0; index < count; ++index)
						{
							const std::byte inserted = static_cast<std::byte>(random.NextU32() & 0xffu);
							mutated.insert(mutated.begin() + static_cast<std::ptrdiff_t>(offset), inserted);
						}
						break;
					}
				}

				// The oracle: the parser returns (a crash or an assert would end the run) with a value or a Parse or
				// Validation error.
				const Status status = ParseCookedPayload(mutated);
				if (!status.has_value())
				{
					++failures;
					const ErrorCode code = status.error().GetCode();
					REQUIRE((code == ErrorCode::Parse || code == ErrorCode::Validation));
				}
			}
			CHECK(failures > 0);
		}

		TEST_CASE("BinaryReader: reads little-endian values written by BinaryWriter")
		{
			BinaryWriter writer;
			writer.WriteU8(0xab);
			writer.WriteU16(0x1234);
			writer.WriteU32(0xdeadbeef);
			writer.WriteU64(0x0123456789abcdefull);
			writer.WriteI8(-5);
			writer.WriteI16(-1234);
			writer.WriteI32(-123456789);
			writer.WriteI64(-1234567890123ll);
			writer.WriteF32(1.5f);
			writer.WriteF64(-2.25);
			writer.WriteBool(false);
			writer.WriteString("UTF-8 \xc3\xa9");
			const Buffer buffer = writer.TakeBuffer();

			BinaryReader reader(buffer);
			CHECK(reader.ReadU8() == uint8_t{ 0xab });
			CHECK(reader.ReadU16() == uint16_t{ 0x1234 });
			CHECK(reader.ReadU32() == 0xdeadbeefu);
			CHECK(reader.ReadU64() == 0x0123456789abcdefull);
			CHECK(reader.ReadI8() == int8_t{ -5 });
			CHECK(reader.ReadI16() == int16_t{ -1234 });
			CHECK(reader.ReadI32() == -123456789);
			CHECK(reader.ReadI64() == -1234567890123ll);
			CHECK(reader.ReadF32() == 1.5f);
			CHECK(reader.ReadF64() == -2.25);
			CHECK(reader.ReadBool() == false);
			CHECK(reader.ReadString() == std::string("UTF-8 \xc3\xa9"));
			CHECK(reader.IsAtEnd());
			CHECK(ErrorCodeOf(reader.ReadU8()) == ErrorCode::Parse);
		}

		TEST_CASE("BinaryReader: a failed read leaves the position unchanged")
		{
			const std::array<std::byte, 3> bytes = { std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 } };
			BinaryReader reader(bytes);
			REQUIRE(reader.ReadU8().has_value());
			CHECK(reader.GetPosition() == 1);

			const Result<uint32_t> tooLong = reader.ReadU32();
			REQUIRE_FALSE(tooLong.has_value());
			CHECK(tooLong.error().GetCode() == ErrorCode::Parse);
			CHECK(tooLong.error().GetMessageText().contains("4 bytes requested at offset 1,"));
			CHECK(reader.GetPosition() == 1);
			CHECK(reader.ReadU16() == uint16_t{ 0x0302 });

			CHECK(ErrorCodeOf(reader.Seek(4)) == ErrorCode::Parse);
			CHECK(reader.Seek(3).has_value());
			CHECK(reader.IsAtEnd());
			CHECK(reader.Seek(0).has_value());
			CHECK(ErrorCodeOf(reader.Skip(4)) == ErrorCode::Parse);
			CHECK(reader.GetPosition() == 0);
		}

		TEST_CASE("BinaryReader: ReadArray rejects counts that overflow or exceed the data")
		{
			const std::array<std::byte, 8> bytes{};
			BinaryReader reader(bytes);
			CHECK(ErrorCodeOf(reader.ReadArray<uint64_t>(std::numeric_limits<size_t>::max())) == ErrorCode::Parse);
			CHECK(ErrorCodeOf(reader.ReadArray<uint32_t>(3)) == ErrorCode::Parse);
			CHECK(reader.GetPosition() == 0);

			const Result<std::vector<uint32_t>> two = reader.ReadArray<uint32_t>(2);
			REQUIRE(two.has_value());
			CHECK(two->size() == 2);
			CHECK(reader.ReadArray<uint32_t>(0).has_value());
		}

		TEST_CASE("BinaryReader: ReadBytes and Skip view and advance within the data")
		{
			const std::array<std::byte, 6> bytes = { std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 }, std::byte{ 4 }, std::byte{ 5 }, std::byte{ 6 } };
			BinaryReader reader(bytes);
			const Result<std::span<const std::byte>> first = reader.ReadBytes(2);
			REQUIRE(first.has_value());
			CHECK(first->data() == bytes.data());
			CHECK(first->size() == 2);
			REQUIRE(reader.Skip(3).has_value());
			CHECK(reader.GetRemaining() == 1);
			CHECK(ErrorCodeOf(reader.ReadBytes(2)) == ErrorCode::Parse);
			CHECK(reader.ReadBytes(0).has_value());
			CHECK(reader.ReadU8() == uint8_t{ 6 });
			CHECK(reader.IsAtEnd());
		}

		TEST_CASE("BinaryReader: floats keep their bit patterns, NaN and infinities included")
		{
			BinaryWriter writer;
			writer.WriteF32(std::numeric_limits<float>::infinity());
			writer.WriteF64(std::numeric_limits<double>::quiet_NaN());
			writer.WriteF32(-0.0f);
			const Buffer buffer = writer.TakeBuffer();

			BinaryReader reader(buffer);
			CHECK(reader.ReadF32() == std::numeric_limits<float>::infinity());
			const Result<double> nan = reader.ReadF64();
			REQUIRE(nan.has_value());
			CHECK(std::isnan(*nan));
			const Result<float> negativeZero = reader.ReadF32();
			REQUIRE(negativeZero.has_value());
			CHECK(std::signbit(*negativeZero));
		}

		TEST_CASE("BinaryReader: ReadBool accepts only 0 and 1")
		{
			const std::array<std::byte, 3> bytes = { std::byte{ 0 }, std::byte{ 1 }, std::byte{ 2 } };
			BinaryReader reader(bytes);
			CHECK(reader.ReadBool() == false);
			CHECK(reader.ReadBool() == true);
			CHECK(ErrorCodeOf(reader.ReadBool()) == ErrorCode::Parse);
		}

		TEST_CASE("BinaryReader: ReadString checks the length and the UTF-8")
		{
			BinaryWriter oversized;
			oversized.WriteU32(1000);
			oversized.WriteBytes(AsBytes("short"));
			BinaryReader truncated(oversized.GetData());
			CHECK(ErrorCodeOf(truncated.ReadString()) == ErrorCode::Parse);
			CHECK(truncated.GetPosition() == 0);

			BinaryWriter invalid;
			invalid.WriteU32(2);
			invalid.WriteBytes(AsBytes("\xc3\x28"));
			BinaryReader invalidReader(invalid.GetData());
			CHECK(ErrorCodeOf(invalidReader.ReadString()) == ErrorCode::Validation);
		}
	}

}
