#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// LSP-style framing of the automation transport (Architecture §13.2): "Content-Length: N\r\n\r\n<N bytes of UTF-8 JSON>".
// The decoder is the first line of defence of a loopback socket that any local process (and, through DNS rebinding, a
// browser) can reach, so it accepts nothing but well-formed frames and fails, for good, on the first violation; the
// server then closes the connection.

namespace Engine {

	// The largest accepted payload (§13.2: frames over 64 MB close the connection).
	inline constexpr size_t MaxFramePayloadBytes = 64ull * 1024 * 1024;
	// The deepest accepted nesting of arrays and objects in a payload (§13.2: over 128 closes the connection), counted as
	// in MaxJsonDepth: "[]" is 1.
	inline constexpr size_t MaxMessageNestingDepth = 128;
	// The longest accepted header section, the blank line included.
	inline constexpr size_t MaxFrameHeaderBytes = 1024;

	// Why a byte stream was rejected.
	enum class FrameErrorKind : uint8_t
	{
		// The stream does not start with "Content-Length:" (any ASCII case): an HTTP request ("GET / HTTP/1.1") or other
		// traffic. Detected as soon as the received bytes can no longer be a prefix of it, without waiting for a line end.
		NotContentLength,
		// A header line that is not "Content-Length: <decimal>" first or "Content-Type: <text>" after it, a missing or
		// repeated Content-Length, a bare LF, or a header section over MaxFrameHeaderBytes.
		MalformedHeader,
		// A Content-Length over the decoder's payload limit (MaxFramePayloadBytes unless set lower), detected from the header
		// before the payload arrives.
		Oversized,
		// A payload that is not valid UTF-8.
		InvalidUtf8,
		// A payload nested deeper than MaxMessageNestingDepth (scanned outside strings, before any JSON parsing).
		TooDeep
	};

	// "NotContentLength", "MalformedHeader", ...
	[[nodiscard]] std::string_view FrameErrorKindToString(FrameErrorKind kind);

	// Incremental decoder of one connection's byte stream. Bytes arrive in any split (one byte at a time or several frames
	// at once); every complete frame yields its payload in order. A value type; one thread at a time.
	//
	// Never crashes and never asserts on any input (Roadmap M4 "Framing: random byte streams never crash the decoder"); it
	// buffers at most MaxFrameHeaderBytes + its payload limit bytes of one frame plus whatever was appended after it.
	class FrameDecoder
	{
	public:
		// A decoder whose frames may carry at most `maxPayloadBytes` (at most MaxFramePayloadBytes, which is used beyond it).
		// The automation server starts each connection with a small limit and raises it once the connection is
		// authenticated, so traffic without the token never makes it buffer or parse a large frame (§13.2).
		explicit FrameDecoder(size_t maxPayloadBytes = MaxFramePayloadBytes);

		// Changes the payload limit (clamped to MaxFramePayloadBytes). It applies from the frame at the front of the buffer
		// on: the header of the next frame is read again on every Next call.
		void SetMaxPayloadBytes(size_t maxPayloadBytes);
		[[nodiscard]] size_t GetMaxPayloadBytes() const { return m_MaxPayloadBytes; }

		// Appends received bytes. After a failure appended bytes are ignored.
		void Append(std::span<const std::byte> bytes);

		// The next complete payload (the frame's JSON text, without the header), or nullopt when more bytes are needed.
		// Errors: Validation whose message names the FrameErrorKind (also returned by GetFailure); the decoder has failed for
		// good, every later call returns the same error, and the connection must be closed.
		[[nodiscard]] Result<std::optional<std::string>> Next();

		// The failure, once Next has reported one.
		[[nodiscard]] std::optional<FrameErrorKind> GetFailure() const { return m_Failure; }

		// Bytes received and not yet returned as a payload.
		[[nodiscard]] size_t GetBufferedSize() const { return m_Buffer.size(); }
	private:
		std::vector<std::byte> m_Buffer;
		std::optional<FrameErrorKind> m_Failure;
		size_t m_MaxPayloadBytes = MaxFramePayloadBytes;
	};

	// The frame of `payload`: "Content-Length: <payload.size()>\r\n\r\n" followed by the payload. The payload must be at most
	// MaxFramePayloadBytes (asserted); the engine's own responses are bounded far below that by result offloading.
	[[nodiscard]] std::string EncodeFrame(std::string_view payload);

}
