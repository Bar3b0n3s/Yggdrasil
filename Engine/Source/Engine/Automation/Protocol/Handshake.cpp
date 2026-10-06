#include "EnginePCH.h"
#include "Engine/Automation/Protocol/Handshake.h"

// M4 contract stub (Roadmap rule 3): stream B (protocol) implements token generation and the handshake checks.

namespace Engine {

	Result<std::string> GenerateAuthToken()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GenerateAuthToken is an M4 contract stub");
	}

	bool AuthTokensEqual(std::string_view /*expected*/, std::string_view /*given*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	Result<HelloRequest> ParseHelloRequest(const Json& /*params*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ParseHelloRequest is an M4 contract stub");
	}

	Status CheckHello(const HelloRequest& /*hello*/, std::string_view /*expectedToken*/, ProtocolVersion /*serverVersion*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "CheckHello is an M4 contract stub");
	}

}
