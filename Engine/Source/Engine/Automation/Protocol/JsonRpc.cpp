#include "EnginePCH.h"
#include "Engine/Automation/Protocol/JsonRpc.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"

#include <format>
#include <utility>

namespace Engine {

	namespace Utils {

		// The longest accepted component of a protocol version, in digits (ProtocolVersion::Parse).
		constexpr size_t MaxVersionDigits = 9;

		// One component of "<major>.<minor>": decimal digits without sign, at most MaxVersionDigits of them, and no leading
		// zero unless the component is "0" itself.
		static std::optional<uint32_t> ParseVersionComponent(std::string_view text)
		{
			if (text.empty() || text.size() > MaxVersionDigits)
				return std::nullopt;
			if (text.size() > 1 && text.front() == '0')
				return std::nullopt;
			uint32_t value = 0;
			for (const char character : text)
			{
				if (character < '0' || character > '9')
					return std::nullopt;
				value = value * 10 + static_cast<uint32_t>(character - '0');
			}
			return value;
		}

		// The standard JSON-RPC text of the standard codes; empty for the engine's own codes.
		static std::string_view GetStandardMessage(RpcErrorCode code)
		{
			switch (code)
			{
				case RpcErrorCode::ParseError:     return "Parse error";
				case RpcErrorCode::InvalidRequest: return "Invalid Request";
				case RpcErrorCode::MethodNotFound: return "Method not found";
				case RpcErrorCode::InvalidParams:  return "Invalid params";
				case RpcErrorCode::Internal:
				case RpcErrorCode::NotFound:
				case RpcErrorCode::InvalidState:
				case RpcErrorCode::ValidationFailed:
				case RpcErrorCode::Conflict:
				case RpcErrorCode::Timeout:
				case RpcErrorCode::ScriptError:
				case RpcErrorCode::Busy:
				case RpcErrorCode::Unauthorized:
				case RpcErrorCode::Unsupported:
				case RpcErrorCode::Cancelled:
					return {};
			}
			return {};
		}

		// {"jsonrpc": "2.0", "id": id}: the members every response starts with.
		static Json MakeResponseHead(const Json& id)
		{
			Json response = Json::object();
			response["jsonrpc"] = "2.0";
			response["id"] = id;
			return response;
		}

		// The "location" member of an error's data: the members of `location` that are set.
		static Json MakeLocationJson(const ErrorLocation& location)
		{
			Json json = Json::object();
			if (!location.File.empty())
				json["file"] = location.File;
			if (location.Line != 0)
				json["line"] = location.Line;
			if (location.Column != 0)
				json["column"] = location.Column;
			if (location.JsonPointer.has_value())
				json["pointer"] = *location.JsonPointer;
			if (location.Entity.IsValid())
				json["entity"] = location.Entity.ToString();
			return json;
		}

		// The "issues" member of an error's data (§13.3): each {pointer, message, hint?, suggestions?}.
		static Json MakeIssuesJson(const std::vector<ErrorIssue>& issues)
		{
			Json json = Json::array();
			for (const ErrorIssue& issue : issues)
			{
				Json entry = Json::object();
				entry["pointer"] = issue.JsonPointer;
				entry["message"] = issue.Message;
				if (!issue.Hint.empty())
					entry["hint"] = issue.Hint;
				if (!issue.Suggestions.empty())
				{
					Json suggestions = Json::array();
					for (const std::string& suggestion : issue.Suggestions)
						suggestions.push_back(suggestion);
					entry["suggestions"] = std::move(suggestions);
				}
				json.push_back(std::move(entry));
			}
			return json;
		}

		// An InvalidRequest failure answered with `id`.
		static std::unexpected<RpcParseFailure> MakeInvalidRequest(Json id, std::string message)
		{
			return std::unexpected(RpcParseFailure{
				.Code = RpcErrorCode::InvalidRequest,
				.Failure = Error(ErrorCode::InvalidArgument, std::move(message)),
				.Id = std::move(id),
			});
		}

	}

	std::string ProtocolVersion::ToString() const
	{
		return std::format("{}.{}", Major, Minor);
	}

	std::optional<ProtocolVersion> ProtocolVersion::Parse(std::string_view text)
	{
		const size_t dot = text.find('.');
		if (dot == std::string_view::npos)
			return std::nullopt;
		const std::optional<uint32_t> major = Utils::ParseVersionComponent(text.substr(0, dot));
		const std::optional<uint32_t> minor = Utils::ParseVersionComponent(text.substr(dot + 1));
		if (!major.has_value() || !minor.has_value())
			return std::nullopt;
		return ProtocolVersion{ *major, *minor };
	}

	std::string_view RpcErrorCodeToString(RpcErrorCode code)
	{
		switch (code)
		{
			case RpcErrorCode::ParseError:       return "ParseError";
			case RpcErrorCode::InvalidRequest:   return "InvalidRequest";
			case RpcErrorCode::MethodNotFound:   return "MethodNotFound";
			case RpcErrorCode::InvalidParams:    return "InvalidParams";
			case RpcErrorCode::Internal:         return "Internal";
			case RpcErrorCode::NotFound:         return "NotFound";
			case RpcErrorCode::InvalidState:     return "InvalidState";
			case RpcErrorCode::ValidationFailed: return "ValidationFailed";
			case RpcErrorCode::Conflict:         return "Conflict";
			case RpcErrorCode::Timeout:          return "Timeout";
			case RpcErrorCode::ScriptError:      return "ScriptError";
			case RpcErrorCode::Busy:             return "Busy";
			case RpcErrorCode::Unauthorized:     return "Unauthorized";
			case RpcErrorCode::Unsupported:      return "Unsupported";
			case RpcErrorCode::Cancelled:        return "Cancelled";
		}
		return "Internal";
	}

	RpcErrorCode ToRpcErrorCode(ErrorCode code)
	{
		switch (code)
		{
			case ErrorCode::InvalidArgument:    return RpcErrorCode::InvalidParams;
			case ErrorCode::NotFound:           return RpcErrorCode::NotFound;
			case ErrorCode::InvalidState:       return RpcErrorCode::InvalidState;
			case ErrorCode::Validation:
			case ErrorCode::Parse:
			case ErrorCode::UnsupportedVersion:
			case ErrorCode::ImportFailed:       return RpcErrorCode::ValidationFailed;
			case ErrorCode::AlreadyExists:
			case ErrorCode::Conflict:           return RpcErrorCode::Conflict;
			case ErrorCode::Timeout:            return RpcErrorCode::Timeout;
			case ErrorCode::Script:
			case ErrorCode::CompileFailed:      return RpcErrorCode::ScriptError;
			case ErrorCode::PermissionDenied:   return RpcErrorCode::Unauthorized;
			case ErrorCode::Unsupported:        return RpcErrorCode::Unsupported;
			case ErrorCode::Cancelled:          return RpcErrorCode::Cancelled;
			case ErrorCode::Unknown:
			case ErrorCode::Io:
			case ErrorCode::Gpu:                return RpcErrorCode::Internal;
		}
		return RpcErrorCode::Internal;
	}

	std::expected<RpcRequest, RpcParseFailure> ParseRpcRequest(std::string_view payload)
	{
		Result<Json> parsed = JsonReader::Parse(payload);
		if (!parsed)
		{
			return std::unexpected(RpcParseFailure{
				.Code = RpcErrorCode::ParseError,
				.Failure = std::move(parsed).error(),
				.Id = Json(),
			});
		}
		Json& document = *parsed;
		if (document.is_array())
			return Utils::MakeInvalidRequest(Json(), "batch requests (a top-level array) are not supported: send one request per frame");
		if (!document.is_object())
		{
			return Utils::MakeInvalidRequest(Json(),
				std::format("a request must be an object, got {}", JsonTypeToString(GetJsonType(document))));
		}

		// The id first, so every later failure is answered with it.
		RpcRequest request;
		const auto id = document.find("id");
		if (id == document.end())
		{
			request.IsNotification = true;
		}
		else if (id->is_number_integer() || id->is_string())
		{
			request.Id = *id;
		}
		else
		{
			return Utils::MakeInvalidRequest(Json(),
				std::format("\"id\" must be an integer or a string, got {} (a notification omits \"id\")", JsonTypeToString(GetJsonType(*id))));
		}

		const auto version = document.find("jsonrpc");
		if (version == document.end() || !version->is_string() || *version != "2.0")
			return Utils::MakeInvalidRequest(std::move(request.Id), "\"jsonrpc\" must be \"2.0\"");

		const auto method = document.find("method");
		if (method == document.end() || !method->is_string())
			return Utils::MakeInvalidRequest(std::move(request.Id), "\"method\" must be a string");
		Result<std::string> methodName = JsonReader(*method, "/method").ReadString();
		if (!methodName)
			return Utils::MakeInvalidRequest(std::move(request.Id), methodName.error().GetMessageText());
		request.Method = std::move(*methodName);

		const auto params = document.find("params");
		if (params == document.end())
		{
			request.Params = Json::object();
		}
		else if (params->is_object())
		{
			request.Params = std::move(*params);
		}
		else
		{
			return Utils::MakeInvalidRequest(std::move(request.Id),
				std::format("\"params\" must be an object of named params, got {}", JsonTypeToString(GetJsonType(*params))));
		}

		const auto meta = request.Params.find("_meta");
		if (meta != request.Params.end())
		{
			if (!meta->is_object())
				return Utils::MakeInvalidRequest(std::move(request.Id), "\"params._meta\" must be an object");
			const JsonReader metaReader(*meta, "/params/_meta");
			if (const std::optional<JsonReader> line = metaReader.FindMember("transcriptLine"))
			{
				const Result<uint64_t> value = line->ReadUInt64();
				if (!value || *value == 0)
					return Utils::MakeInvalidRequest(std::move(request.Id), "\"params._meta.transcriptLine\" must be an integer >= 1");
				request.TranscriptLine = *value;
			}
			request.Params.erase(meta);
		}
		return request;
	}

	Json MakeResultResponse(const Json& id, Json result, const Json& meta)
	{
		ENGINE_CORE_ASSERT(result.is_object(), "A JSON-RPC result of the automation protocol is an object");
		if (!result.is_object())
			result = Json::object();
		if (!meta.is_null())
			result["_meta"] = meta;
		Json response = Utils::MakeResponseHead(id);
		response["result"] = std::move(result);
		return response;
	}

	Json MakeErrorResponse(const Json& id, RpcErrorCode code, const Error& error, const Json& extraData, const Json& meta)
	{
		Json data = Json::object();
		data["errorCode"] = std::string(ErrorCodeToString(error.GetCode()));
		data["detail"] = error.GetMessageText();
		if (!error.GetHint().empty())
			data["hint"] = error.GetHint();
		if (!error.GetContexts().empty())
		{
			Json contexts = Json::array();
			for (const std::string& context : error.GetContexts())
				contexts.push_back(context);
			data["contexts"] = std::move(contexts);
		}
		if (error.GetLocation().IsSet())
			data["location"] = Utils::MakeLocationJson(error.GetLocation());
		data["issues"] = Utils::MakeIssuesJson(error.GetIssues());
		if (extraData.is_object())
		{
			for (auto member = extraData.begin(); member != extraData.end(); ++member)
				data[member.key()] = member.value();
		}
		if (!meta.is_null())
			data["_meta"] = meta;

		const std::string_view standard = Utils::GetStandardMessage(code);
		Json body = Json::object();
		body["code"] = static_cast<int32_t>(code);
		body["message"] = standard.empty() ? error.GetMessageText() : std::string(standard);
		body["data"] = std::move(data);

		Json response = Utils::MakeResponseHead(id);
		response["error"] = std::move(body);
		return response;
	}

	Json MakeErrorResponse(const Json& id, const Error& error, const Json& extraData, const Json& meta)
	{
		return MakeErrorResponse(id, ToRpcErrorCode(error.GetCode()), error, extraData, meta);
	}

	Json MakeBusyResponse(const Json& id, std::string_view phase, uint64_t stalledMilliseconds)
	{
		const std::string message = std::format("the main thread has not responded for {} ms (phase '{}'); retry later",
			stalledMilliseconds, phase);
		Json data = Json::object();
		data["errorCode"] = "Busy";
		data["detail"] = message;
		data["phase"] = std::string(phase);
		data["stalledMilliseconds"] = stalledMilliseconds;
		data["issues"] = Json::array();

		Json body = Json::object();
		body["code"] = static_cast<int32_t>(RpcErrorCode::Busy);
		body["message"] = message;
		body["data"] = std::move(data);

		Json response = Utils::MakeResponseHead(id);
		response["error"] = std::move(body);
		return response;
	}

}
