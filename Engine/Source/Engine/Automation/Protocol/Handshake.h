#pragma once

#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <string>
#include <string_view>

// Session authentication (Architecture §13.2): the first request on a connection must be
// session.hello {token, protocolVersion, client: {name, version}}. The token is 256 bits from the OS CSPRNG, published only
// in the user-only session file, and compared in constant time; three failures close the connection; a different major
// protocol version is rejected listing both versions. The I/O thread performs these checks before a request ever reaches
// the main thread (ADR 0008 decision 6); the session.hello method handler then only reports the session.

namespace Engine {

	// 256 bits (§13.2).
	inline constexpr size_t AuthTokenBytes = 32;
	// The token's text: 2 lowercase hexadecimal digits per byte.
	inline constexpr size_t AuthTokenLength = 2 * AuthTokenBytes;
	// Rejected requests before a successful session.hello (a bad token, or any other method) after which the connection is
	// closed.
	inline constexpr size_t MaxAuthFailures = 3;

	// A fresh token: AuthTokenBytes from SecureRandom as AuthTokenLength lowercase hex digits. Errors: those of
	// SecureRandom::Fill (Io).
	[[nodiscard]] Result<std::string> GenerateAuthToken();

	// True when `given` equals `expected`. The time taken depends only on expected.size(), never on where the first
	// difference lies, and a `given` of another length compares unequal after the same amount of work.
	[[nodiscard]] bool AuthTokensEqual(std::string_view expected, std::string_view given);

	// The members of session.hello's params the I/O thread checks.
	struct HelloRequest
	{
		std::string Token;
		ProtocolVersion Version;
		std::string ClientName;    // client.name, such as "engine-mcp" or "engine-tests"; 1 to 64 printable ASCII characters
		std::string ClientVersion; // client.version; may be empty
	};

	// Reads session.hello params, as strictly as the session.hello handler's param struct does (one rule in both places, so
	// a hello the I/O thread accepts never fails on the main thread). Errors: InvalidArgument (answered as InvalidParams,
	// and counted as an authentication failure, before the connection is authenticated) naming the first bad member: token
	// missing or not a string; protocolVersion missing or not "<major>.<minor>"; client missing, not an object, or a name
	// that is empty, longer than 64 bytes or not printable ASCII; version not a string; an unknown member of the params or
	// of client. The hello's members are fixed for major version 1 (JsonRpc.h).
	[[nodiscard]] Result<HelloRequest> ParseHelloRequest(const Json& params);

	// The I/O thread's decision. Errors: PermissionDenied (Unauthorized) "invalid token" for a token that does not match,
	// which counts towards MaxAuthFailures; Unsupported "protocol version <client> is not compatible with <server>" for a
	// different major version, after which the connection is closed at once. A newer or older minor version is accepted
	// (the server speaks its own minor version; methods a client does not know are simply not called).
	[[nodiscard]] Status CheckHello(const HelloRequest& hello, std::string_view expectedToken, ProtocolVersion serverVersion);

}
