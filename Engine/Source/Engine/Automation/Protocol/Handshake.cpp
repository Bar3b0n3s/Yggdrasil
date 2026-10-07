#include "EnginePCH.h"
#include "Engine/Automation/Protocol/Handshake.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Platform/SecureRandom.h"

#include <array>
#include <format>

namespace Engine {

	namespace Utils {

		// The longest accepted client.name, in bytes.
		constexpr size_t MaxClientNameLength = 64;

		// An InvalidArgument error about the hello member at `pointer`.
		[[nodiscard]] static std::unexpected<Error> MakeHelloError(std::string pointer, std::string message)
		{
			ErrorLocation location;
			location.JsonPointer = pointer;
			return std::unexpected(Error(ErrorCode::InvalidArgument, std::format("invalid session.hello: {}", message))
					.WithLocation(std::move(location))
					.WithIssue(ErrorIssue{ .JsonPointer = std::move(pointer), .Message = std::move(message), .Hint = {}, .Suggestions = {} }));
		}

		// An error for the first member of `object` (a JSON object) that is not one of `known` (listed in the message as
		// `knownText`), or success.
		[[nodiscard]] static Status CheckHelloMembers(const JsonReader& object, std::span<const std::string_view> known, std::string_view knownText)
		{
			Result<std::vector<std::string>> unknown = object.FindUnknownMembers(known);
			if (!unknown)
				return std::unexpected(std::move(unknown).error());
			if (unknown->empty())
				return {};
			const std::string& member = unknown->front();
			return MakeHelloError(JsonReader::AppendPointer(object.GetPointer(), member),
				std::format("unknown member '{}' (expected only {})", member, knownText));
		}

		// A string member `name` of `object`: required, or nullopt when optional and absent.
		[[nodiscard]] static Result<std::optional<std::string>> ReadHelloString(const JsonReader& object, std::string_view name, bool required)
		{
			const std::optional<JsonReader> member = object.FindMember(name);
			const std::string pointer = JsonReader::AppendPointer(object.GetPointer(), name);
			if (!member.has_value())
			{
				if (!required)
					return std::optional<std::string>();
				return MakeHelloError(pointer, std::format("missing member '{}'", name));
			}
			Result<std::string> text = member->ReadString();
			if (!text)
				return MakeHelloError(pointer, std::format("'{}' must be a string", name));
			return std::optional<std::string>(std::move(*text));
		}

		[[nodiscard]] static bool IsPrintableAscii(std::string_view text)
		{
			for (const char character : text)
			{
				if (character < 0x20 || character > 0x7e)
					return false;
			}
			return true;
		}

	}

	Result<std::string> GenerateAuthToken()
	{
		std::array<std::byte, AuthTokenBytes> bytes{};
		ENGINE_TRY(SecureRandom::Fill(bytes));
		constexpr std::string_view Digits = "0123456789abcdef";
		std::string token;
		token.reserve(AuthTokenLength);
		for (const std::byte value : bytes)
		{
			const auto byte = static_cast<uint8_t>(value);
			token.push_back(Digits[byte >> 4]);
			token.push_back(Digits[byte & 0x0f]);
		}
		return token;
	}

	bool AuthTokensEqual(std::string_view expected, std::string_view given)
	{
		// Every byte of `expected` is compared, whatever `given` holds, so the time taken never depends on where the first
		// difference lies; a length difference is folded into the result after the same loop.
		uint32_t difference = expected.size() == given.size() ? 0u : 1u;
		for (size_t index = 0; index < expected.size(); ++index)
		{
			const auto expectedByte = static_cast<uint8_t>(expected[index]);
			const auto givenByte = given.empty() ? static_cast<uint8_t>(~expectedByte) : static_cast<uint8_t>(given[index % given.size()]);
			difference |= static_cast<uint32_t>(expectedByte ^ givenByte);
		}
		return difference == 0;
	}

	Result<HelloRequest> ParseHelloRequest(const Json& params)
	{
		const JsonReader reader(params);
		if (!reader.IsObject())
			return Utils::MakeHelloError("", std::format("params must be an object, got {}", JsonTypeToString(reader.GetType())));

		HelloRequest hello;
		ENGINE_TRY_ASSIGN(std::optional<std::string> token, Utils::ReadHelloString(reader, "token", true));
		hello.Token = std::move(*token);

		ENGINE_TRY_ASSIGN(std::optional<std::string> versionText, Utils::ReadHelloString(reader, "protocolVersion", true));
		const std::optional<ProtocolVersion> version = ProtocolVersion::Parse(*versionText);
		if (!version.has_value())
			return Utils::MakeHelloError("/protocolVersion", std::format("protocolVersion must be \"<major>.<minor>\", got \"{}\"", *versionText));
		hello.Version = *version;

		const std::optional<JsonReader> client = reader.FindMember("client");
		if (!client.has_value())
			return Utils::MakeHelloError("/client", "missing member 'client' ({name, version})");
		if (!client->IsObject())
			return Utils::MakeHelloError("/client", std::format("client must be an object, got {}", JsonTypeToString(client->GetType())));

		ENGINE_TRY_ASSIGN(std::optional<std::string> name, Utils::ReadHelloString(*client, "name", true));
		if (name->empty() || name->size() > Utils::MaxClientNameLength || !Utils::IsPrintableAscii(*name))
		{
			return Utils::MakeHelloError("/client/name",
				std::format("client name must be 1 to {} printable ASCII characters", Utils::MaxClientNameLength));
		}
		hello.ClientName = std::move(*name);

		ENGINE_TRY_ASSIGN(std::optional<std::string> clientVersion, Utils::ReadHelloString(*client, "version", false));
		hello.ClientVersion = clientVersion.value_or(std::string());

		constexpr std::array<std::string_view, 2> ClientMembers = { "name", "version" };
		ENGINE_TRY(Utils::CheckHelloMembers(*client, ClientMembers, "name and version"));
		constexpr std::array<std::string_view, 3> HelloMembers = { "token", "protocolVersion", "client" };
		ENGINE_TRY(Utils::CheckHelloMembers(reader, HelloMembers, "token, protocolVersion and client"));
		return hello;
	}

	Status CheckHello(const HelloRequest& hello, std::string_view expectedToken, ProtocolVersion serverVersion)
	{
		// The token first: a client without it learns nothing about the server, not even its version.
		if (!AuthTokensEqual(expectedToken, hello.Token))
			return MakeError(ErrorCode::PermissionDenied, "invalid token: read it from the editor's session file");
		if (hello.Version.Major != serverVersion.Major)
		{
			return MakeError(ErrorCode::Unsupported, "protocol version {} is not compatible with {}", hello.Version.ToString(),
				serverVersion.ToString());
		}
		return {};
	}

}
