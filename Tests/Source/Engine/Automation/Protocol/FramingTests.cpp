#include "TestsPCH.h"

#include "Engine/Automation/Protocol/Framing.h"

#include "Engine/Core/Random.h"

namespace Engine {

	// A uniform value in [min, max].
	static uint32_t DrawFrameValue(Random& random, uint32_t min, uint32_t max)
	{
		return static_cast<uint32_t>(random.RangeInt(min, max));
	}

	static std::span<const std::byte> AsFrameBytes(std::string_view text)
	{
		return std::as_bytes(std::span(text.data(), text.size()));
	}

	// Feeds `stream` in chunks of the given sizes (cycled) and collects every payload until the decoder needs more bytes or
	// fails. Returns the payloads and the failure, if any.
	static std::pair<std::vector<std::string>, std::optional<FrameErrorKind>> DecodeInChunks(std::string_view stream,
		std::span<const size_t> chunkSizes)
	{
		FrameDecoder decoder;
		std::vector<std::string> payloads;
		size_t offset = 0;
		size_t chunk = 0;
		while (offset < stream.size())
		{
			const size_t size = std::min(chunkSizes[chunk++ % chunkSizes.size()], stream.size() - offset);
			decoder.Append(AsFrameBytes(stream.substr(offset, size)));
			offset += size;
			while (true)
			{
				Result<std::optional<std::string>> next = decoder.Next();
				if (!next.has_value())
					return { payloads, decoder.GetFailure() };
				if (!next->has_value())
					break;
				payloads.push_back(std::move(**next));
			}
		}
		return { payloads, decoder.GetFailure() };
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("Framing: random byte streams never crash the decoder")
		{
			// Seeded random streams, and seeded mutations of valid frames (flipped, dropped and inserted bytes), fed in random
			// chunk sizes. The oracle: the decoder never crashes or asserts, every result is a payload, "need more" or a
			// failure, and a failed decoder keeps failing with the same kind.
			Random random(0xF4A3E5u);
			const std::string valid = EncodeFrame(R"({"jsonrpc":"2.0","id":1,"method":"session.info","params":{}})") + EncodeFrame(R"({"jsonrpc":"2.0","id":2,"method":"scene.tree","params":{"depth":2}})");
			for (int iteration = 0; iteration < 10000; ++iteration)
			{
				std::string stream;
				if (iteration % 2 == 0)
				{
					const size_t length = DrawFrameValue(random, 0, 512);
					for (size_t index = 0; index < length; ++index)
						stream.push_back(static_cast<char>(DrawFrameValue(random, 0, 255)));
				}
				else
				{
					stream = valid;
					const uint32_t mutations = DrawFrameValue(random, 1, 8);
					for (uint32_t mutation = 0; mutation < mutations && !stream.empty(); ++mutation)
					{
						const size_t position = DrawFrameValue(random, 0, static_cast<uint32_t>(stream.size() - 1));
						switch (DrawFrameValue(random, 0, 2))
						{
							case 0:  stream[position] = static_cast<char>(DrawFrameValue(random, 0, 255)); break;
							case 1:  stream.erase(position, 1); break;
							default: stream.insert(position, 1, static_cast<char>(DrawFrameValue(random, 0, 255))); break;
						}
					}
				}

				const std::array<size_t, 3> chunks = { DrawFrameValue(random, 1, 7), DrawFrameValue(random, 1, 64), DrawFrameValue(random, 1, 1024) };
				FrameDecoder decoder;
				size_t offset = 0;
				size_t chunk = 0;
				std::optional<FrameErrorKind> failure;
				while (offset < stream.size())
				{
					const size_t size = std::min(chunks[chunk++ % chunks.size()], stream.size() - offset);
					decoder.Append(AsFrameBytes(std::string_view(stream).substr(offset, size)));
					offset += size;
					Result<std::optional<std::string>> next = decoder.Next();
					while (next.has_value() && next->has_value())
						next = decoder.Next();
					if (!next.has_value())
					{
						failure = decoder.GetFailure();
						REQUIRE(failure.has_value());
						break;
					}
				}
				if (failure.has_value())
				{
					CHECK_FALSE(decoder.Next().has_value());
					CHECK(decoder.GetFailure() == failure);
				}
			}
		}

		TEST_CASE("Framing: a frame split at every byte decodes to its payload")
		{
			const std::string payload = R"({"jsonrpc":"2.0","id":7,"method":"entity.get","params":{"entity":"/Game"}})";
			const std::array<size_t, 1> oneByte = { 1 };
			const auto [payloads, failure] = DecodeInChunks(EncodeFrame(payload), oneByte);
			CHECK_FALSE(failure.has_value());
			REQUIRE(payloads.size() == 1);
			CHECK(payloads[0] == payload);
		}

		TEST_CASE("Framing: several frames in one chunk decode in order")
		{
			const std::string stream = EncodeFrame("{\"a\":1}") + EncodeFrame("[]") + EncodeFrame("{\"b\":\"x\"}");
			const std::array<size_t, 1> whole = { stream.size() };
			const auto [payloads, failure] = DecodeInChunks(stream, whole);
			CHECK_FALSE(failure.has_value());
			CHECK(payloads == std::vector<std::string>{ "{\"a\":1}", "[]", "{\"b\":\"x\"}" });
		}

		TEST_CASE("Framing: an HTTP request line fails at once with NotContentLength")
		{
			// The decoder needs no line end to recognize the probe: "GE" can no longer start "Content-Length:".
			FrameDecoder decoder;
			decoder.Append(AsFrameBytes("GE"));
			CHECK_FALSE(decoder.Next().has_value());
			CHECK(decoder.GetFailure() == FrameErrorKind::NotContentLength);

			const std::array<size_t, 1> whole = { 64 };
			CHECK(DecodeInChunks("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n", whole).second == FrameErrorKind::NotContentLength);
			CHECK(DecodeInChunks("content-length: 2\r\n\r\n{}", whole).second == std::nullopt);
		}

		TEST_CASE("Framing: a Content-Length over 64 MB fails as Oversized before the payload arrives")
		{
			FrameDecoder decoder;
			decoder.Append(AsFrameBytes(std::format("Content-Length: {}\r\n\r\n", MaxFramePayloadBytes + 1)));
			CHECK_FALSE(decoder.Next().has_value());
			CHECK(decoder.GetFailure() == FrameErrorKind::Oversized);

			FrameDecoder atLimit;
			atLimit.Append(AsFrameBytes(std::format("Content-Length: {}\r\n\r\n", MaxFramePayloadBytes)));
			const Result<std::optional<std::string>> waiting = atLimit.Next();
			REQUIRE(waiting.has_value());
			CHECK_FALSE(waiting->has_value());
		}

		TEST_CASE("Framing: invalid UTF-8 and nesting over 128 fail")
		{
			const std::array<size_t, 1> whole = { 1u << 20 };
			CHECK(DecodeInChunks(EncodeFrame("{\"a\":\"\xC3\x28\"}"), whole).second == FrameErrorKind::InvalidUtf8);

			const std::string deep = std::string(MaxMessageNestingDepth + 1, '[') + std::string(MaxMessageNestingDepth + 1, ']');
			CHECK(DecodeInChunks(EncodeFrame(deep), whole).second == FrameErrorKind::TooDeep);
			const std::string atLimit = std::string(MaxMessageNestingDepth, '[') + std::string(MaxMessageNestingDepth, ']');
			CHECK(DecodeInChunks(EncodeFrame(atLimit), whole).second == std::nullopt);
			// Brackets inside strings do not nest.
			const std::string inString = "[\"" + std::string(MaxMessageNestingDepth + 1, '[') + "\"]";
			CHECK(DecodeInChunks(EncodeFrame(inString), whole).second == std::nullopt);
		}

		TEST_CASE("Framing: Content-Type after Content-Length is accepted and other headers are malformed")
		{
			const std::array<size_t, 1> whole = { 256 };
			const auto [payloads, failure] = DecodeInChunks("Content-Length: 2\r\nContent-Type: application/vscode-jsonrpc; charset=utf-8\r\n\r\n{}", whole);
			CHECK_FALSE(failure.has_value());
			CHECK(payloads == std::vector<std::string>{ "{}" });

			CHECK(DecodeInChunks("Content-Length: 2\r\nX-Other: 1\r\n\r\n{}", whole).second == FrameErrorKind::MalformedHeader);
			CHECK(DecodeInChunks("Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}", whole).second == FrameErrorKind::MalformedHeader);
			CHECK(DecodeInChunks("Content-Length: x\r\n\r\n{}", whole).second == FrameErrorKind::MalformedHeader);
			CHECK(DecodeInChunks("Content-Length: 2\n\n{}", whole).second == FrameErrorKind::MalformedHeader);
		}

		TEST_CASE("Framing: EncodeFrame writes the header the decoder reads")
		{
			CHECK(EncodeFrame("{}") == "Content-Length: 2\r\n\r\n{}");
			CHECK(EncodeFrame("\xC3\xA9") == "Content-Length: 2\r\n\r\n\xC3\xA9"); // bytes, not characters
		}

		TEST_CASE("Framing: FrameErrorKindToString names every kind")
		{
			CHECK(FrameErrorKindToString(FrameErrorKind::NotContentLength) == "NotContentLength");
			CHECK(FrameErrorKindToString(FrameErrorKind::MalformedHeader) == "MalformedHeader");
			CHECK(FrameErrorKindToString(FrameErrorKind::Oversized) == "Oversized");
			CHECK(FrameErrorKindToString(FrameErrorKind::InvalidUtf8) == "InvalidUtf8");
			CHECK(FrameErrorKindToString(FrameErrorKind::TooDeep) == "TooDeep");
		}
	}

}
