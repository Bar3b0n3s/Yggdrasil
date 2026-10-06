#include "EnginePCH.h"
#include "Engine/Automation/Protocol/JsonRpc.h"

// M4 contract stub (Roadmap rule 3): stream B (protocol) implements the JSON-RPC messages.

namespace Engine {

	std::string ProtocolVersion::ToString() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::optional<ProtocolVersion> ProtocolVersion::Parse(std::string_view /*text*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::string_view RpcErrorCodeToString(RpcErrorCode /*code*/)
	{
		ENGINE_CONTRACT_STUB();
		return "Internal";
	}

	RpcErrorCode ToRpcErrorCode(ErrorCode /*code*/)
	{
		ENGINE_CONTRACT_STUB();
		return RpcErrorCode::Internal;
	}

	std::expected<RpcRequest, RpcParseFailure> ParseRpcRequest(std::string_view /*payload*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(RpcParseFailure{
			.Code = RpcErrorCode::InvalidRequest,
			.Failure = Error(ErrorCode::Unsupported, "ParseRpcRequest is an M4 contract stub"),
			.Id = Json(),
		});
	}

	Json MakeResultResponse(const Json& /*id*/, Json /*result*/, const Json& /*meta*/)
	{
		ENGINE_CONTRACT_STUB();
		return Json();
	}

	Json MakeErrorResponse(const Json& /*id*/, RpcErrorCode /*code*/, const Error& /*error*/, const Json& /*extraData*/,
		const Json& /*meta*/)
	{
		ENGINE_CONTRACT_STUB();
		return Json();
	}

	Json MakeErrorResponse(const Json& /*id*/, const Error& /*error*/, const Json& /*extraData*/, const Json& /*meta*/)
	{
		ENGINE_CONTRACT_STUB();
		return Json();
	}

	Json MakeBusyResponse(const Json& /*id*/, std::string_view /*phase*/, uint64_t /*stalledMilliseconds*/)
	{
		ENGINE_CONTRACT_STUB();
		return Json();
	}

}
