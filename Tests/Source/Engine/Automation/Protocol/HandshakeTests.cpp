#include "TestsPCH.h"

#include "Engine/Automation/Protocol/Handshake.h"

#include "Engine/Core/Json/JsonReader.h"

namespace Engine {

	static Json ParseHandshakeJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("Handshake: tokens are 64 lowercase hex digits and differ")
		{
			const Result<std::string> first = GenerateAuthToken();
			const Result<std::string> second = GenerateAuthToken();
			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			CHECK(first->size() == AuthTokenLength);
			CHECK(std::all_of(first->begin(), first->end(), [](char character)
			{
				return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
			}));
			CHECK(*first != *second);
		}

		TEST_CASE("Handshake: token comparison is exact")
		{
			const std::string token(AuthTokenLength, 'a');
			CHECK(AuthTokensEqual(token, token));
			CHECK_FALSE(AuthTokensEqual(token, std::string(AuthTokenLength - 1, 'a')));
			CHECK_FALSE(AuthTokensEqual(token, std::string(AuthTokenLength, 'A')));
			CHECK_FALSE(AuthTokensEqual(token, std::string(AuthTokenLength - 1, 'a') + "b"));
			CHECK_FALSE(AuthTokensEqual(token, {}));
		}

		TEST_CASE("Handshake: a bad token is PermissionDenied and a different major version Unsupported")
		{
			const std::string token(AuthTokenLength, 'c');
			const HelloRequest good{ .Token = token, .Version = { 1, 3 }, .ClientName = "engine-tests", .ClientVersion = "1" };
			CHECK(CheckHello(good, token, CurrentProtocolVersion).has_value());

			HelloRequest badToken = good;
			badToken.Token = std::string(AuthTokenLength, 'd');
			const Status denied = CheckHello(badToken, token, CurrentProtocolVersion);
			REQUIRE_FALSE(denied.has_value());
			CHECK(denied.error().GetCode() == ErrorCode::PermissionDenied);

			HelloRequest newer = good;
			newer.Version = { 2, 0 };
			const Status incompatible = CheckHello(newer, token, CurrentProtocolVersion);
			REQUIRE_FALSE(incompatible.has_value());
			CHECK(incompatible.error().GetCode() == ErrorCode::Unsupported);
			CHECK(incompatible.error().GetMessageText().contains("2.0"));
			CHECK(incompatible.error().GetMessageText().contains("1.0"));
		}

		TEST_CASE("Handshake: hello params are read and malformed ones name the member")
		{
			const Result<HelloRequest> hello = ParseHelloRequest(
				ParseHandshakeJson(R"({"token":"abc","protocolVersion":"1.0","client":{"name":"engine-mcp","version":"0.1"}})"));
			REQUIRE(hello.has_value());
			CHECK(hello->Token == "abc");
			CHECK(hello->Version == ProtocolVersion{ 1, 0 });
			CHECK(hello->ClientName == "engine-mcp");
			CHECK(hello->ClientVersion == "0.1");

			// Unknown members are rejected here exactly as the session.hello handler's param struct rejects them.
			const std::array<std::pair<std::string_view, std::string_view>, 7> cases = { {
				{ R"({"protocolVersion":"1.0","client":{"name":"a"}})", "token" },
				{ R"({"token":"t","protocolVersion":"one","client":{"name":"a"}})", "protocolVersion" },
				{ R"({"token":"t","protocolVersion":"1.0"})", "client" },
				{ R"({"token":"t","protocolVersion":"1.0","client":{"name":""}})", "name" },
				{ R"({"token":"t","protocolVersion":"1.0","client":{"name":"a\u0001"}})", "name" },
				{ R"({"token":"t","protocolVersion":"1.0","client":{"name":"a"},"features":[]})", "features" },
				{ R"({"token":"t","protocolVersion":"1.0","client":{"name":"a","build":"x"}})", "build" },
			} };
			for (const auto& [params, member] : cases)
			{
				INFO(std::string(params));
				const Result<HelloRequest> failure = ParseHelloRequest(ParseHandshakeJson(params));
				REQUIRE_FALSE(failure.has_value());
				CHECK(failure.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(failure.error().ToString().contains(std::string(member)));
			}
		}
	}

}
