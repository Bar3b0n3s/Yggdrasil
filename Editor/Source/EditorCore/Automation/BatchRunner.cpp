#include "EditorPCH.h"
#include "EditorCore/Automation/BatchRunner.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/TranscriptLog.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Automation/Protocol/JsonReference.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"

#include <nlohmann/json.hpp>

#include <array>
#include <format>
#include <utility>

namespace Engine {

	namespace Utils {

		// The Error an error response describes: data.errorCode, data.detail and data.issues.
		[[nodiscard]] static Error MakeResponseError(const Json& response)
		{
			const JsonReader reader(response);
			const std::optional<JsonReader> error = reader.FindMember("error");
			std::optional<JsonReader> data;
			std::string message = "the request failed";
			if (error.has_value())
			{
				message = error->ReadMember<std::string>("message").value_or(message);
				data = error->FindMember("data");
			}
			if (!data.has_value())
				return Error(ErrorCode::Unknown, std::move(message));

			Error failure(ErrorCodeFromString(data->ReadMember<std::string>("errorCode").value_or(std::string())).value_or(ErrorCode::Unknown),
				data->ReadMember<std::string>("detail").value_or(message));
			const std::string hint = data->ReadMember<std::string>("hint").value_or(std::string());
			if (!hint.empty())
				failure = std::move(failure).WithHint(hint);
			if (const std::optional<JsonReader> issues = data->FindMember("issues"))
			{
				std::vector<ErrorIssue> collected;
				const size_t count = issues->GetArraySize().value_or(0);
				for (size_t index = 0; index < count; ++index)
				{
					const Result<JsonReader> issue = issues->GetElement(index);
					if (!issue.has_value())
						continue;
					collected.push_back(ErrorIssue{
						.JsonPointer = issue->ReadMember<std::string>("pointer").value_or(std::string()),
						.Message = issue->ReadMember<std::string>("message").value_or(std::string()),
						.Hint = issue->ReadMember<std::string>("hint").value_or(std::string()),
						.Suggestions = {},
					});
				}
				failure = std::move(failure).WithIssues(std::move(collected));
			}
			return failure;
		}

		// "line 3 of the batch file" or "request 3", the context of a request's errors.
		[[nodiscard]] static std::string DescribeRequest(const BatchRequest& request, size_t index)
		{
			return request.Line != 0 ? std::format("at line {} of the batch file ({})", request.Line, request.Method)
									 : std::format("in request {} ({})", index + 1, request.Method);
		}

		// A Validation or Parse error at `line` of the batch file (the caller names the file it passed).
		[[nodiscard]] static std::unexpected<Error> MakeBatchFileError(ErrorCode code, uint32_t line, std::string message)
		{
			ErrorLocation location;
			location.Line = line;
			return std::unexpected(Error(code, std::move(message)).WithLocation(std::move(location)));
		}

	}

	struct BatchRunner::State
	{
		ClientId Client = NoClient;
		size_t NextRequest = 0;
		bool InFlight = false;
		bool Finished = false;
		int64_t RequestId = 0;                    // the JSON-RPC id of the request in flight
		std::optional<uint64_t> TranscriptLine{}; // the transcript line of the request in flight
	};

	Result<std::vector<BatchRequest>> BatchRunner::LoadFile(const std::filesystem::path& file)
	{
		ENGINE_TRY_ASSIGN(const std::string text, FileSystem::ReadText(file));
		std::vector<BatchRequest> requests;
		size_t start = 0;
		uint32_t line = 0;
		while (start < text.size())
		{
			++line;
			const size_t end = text.find('\n', start);
			std::string_view content = std::string_view(text).substr(start, end == std::string::npos ? std::string::npos : end - start);
			start = end == std::string::npos ? text.size() : end + 1;
			if (!content.empty() && content.back() == '\r')
				content.remove_suffix(1);
			if (content.find_first_not_of(" \t\r") == std::string_view::npos)
				return Utils::MakeBatchFileError(ErrorCode::Validation, line, "an empty line: every line of a batch file is one request");

			Result<Json> parsed = JsonReader::Parse(content);
			if (!parsed)
			{
				return Utils::MakeBatchFileError(ErrorCode::Parse, line,
					std::format("line {} is not JSON: {}", line, parsed.error().GetMessageText()));
			}
			const JsonReader reader(*parsed);
			if (!reader.IsObject())
				return Utils::MakeBatchFileError(ErrorCode::Validation, line, "a request line must be an object {\"method\": ..., \"params\": ...}");
			constexpr std::array<std::string_view, 2> Members = { "method", "params" };
			const Result<std::vector<std::string>> unknown = reader.FindUnknownMembers(Members);
			if (unknown.has_value() && !unknown->empty())
			{
				return Utils::MakeBatchFileError(ErrorCode::Validation, line,
					std::format("unknown member '{}': a request line has only \"method\" and \"params\"", unknown->front()));
			}
			Result<std::string> method = reader.ReadMember<std::string>("method");
			if (!method)
				return Utils::MakeBatchFileError(ErrorCode::Validation, line, method.error().GetMessageText());

			BatchRequest request;
			request.Method = std::move(*method);
			request.Line = line;
			if (const std::optional<JsonReader> params = reader.FindMember("params"))
			{
				if (!params->IsObject() && !params->IsNull())
					return Utils::MakeBatchFileError(ErrorCode::Validation, line, "\"params\" must be an object");
				request.Params = VariantValue(params->GetValue());
			}
			requests.push_back(std::move(request));
		}
		return requests;
	}

	BatchRunner::BatchRunner(std::vector<BatchRequest> requests, BatchRunOptions options)
		: m_Requests(std::move(requests)), m_Options(std::move(options)), m_State(CreateScope<State>())
	{
	}

	BatchRunner::~BatchRunner() = default;

	void BatchRunner::Advance(AutomationServer& server)
	{
		State& state = *m_State;
		if (state.Finished)
			return;
		const auto finish = [&state, &server]()
		{
			state.Finished = true;
			server.DisconnectInProcess(state.Client);
		};
		if (state.Client == NoClient)
		{
			// Results stay inline, so a "$ref" into a large result finds the data rather than an offload summary.
			state.Client = server.ConnectInProcess(m_Options.ClientName, false);
		}

		if (state.InFlight)
		{
			std::optional<Json> response;
			for (Json& message : server.TakeInProcessResponses(state.Client))
			{
				if (message.contains("id") && message["id"] == Json(state.RequestId))
					response = std::move(message);
			}
			if (!response.has_value())
				return; // still running (a pending operation)
			state.InFlight = false;
			const BatchRequest& request = m_Requests[state.NextRequest];

			const bool succeeded = response->contains("result");
			if (m_Options.WriteTranscript && state.TranscriptLine.has_value())
			{
				std::string summary = succeeded ? "ok" : Utils::MakeResponseError(*response).GetMessageText();
				const Json error = succeeded ? Json() : (*response)["error"];
				Status written = TranscriptLog::AppendResponse(server.GetEditor().GetVfs(), m_Options.ClientName, Json(state.RequestId),
					*state.TranscriptLine, summary, error);
				if (!written)
				{
					m_Failure = std::move(written).error().WithContext(std::format("writing the transcript {}", Utils::DescribeRequest(request, state.NextRequest)));
					finish();
					return;
				}
			}
			if (!succeeded)
			{
				m_FailedResponse = *response;
				m_Failure = Utils::MakeResponseError(*response).WithContext(Utils::DescribeRequest(request, state.NextRequest));
				finish();
				return;
			}
			m_Results.push_back(std::move((*response)["result"]));
			++state.NextRequest;
		}

		if (state.NextRequest >= m_Requests.size())
		{
			finish();
			return;
		}

		const BatchRequest& request = m_Requests[state.NextRequest];
		Json params = request.Params.IsNull() ? Json::object() : request.Params.Get();
		const Status substituted = SubstituteReferences(params, m_Results, 1);
		if (!substituted)
		{
			m_Failure = Error(substituted.error()).WithContext(Utils::DescribeRequest(request, state.NextRequest));
			finish();
			return;
		}

		state.RequestId = static_cast<int64_t>(state.NextRequest) + 1;
		state.TranscriptLine.reset();
		if (m_Options.WriteTranscript)
		{
			Result<uint64_t> line = TranscriptLog::AppendRequest(server.GetEditor().GetVfs(), m_Options.ClientName, Json(state.RequestId), request.Method, params);
			if (!line)
			{
				m_Failure = std::move(line).error().WithContext(std::format("writing the transcript {}", Utils::DescribeRequest(request, state.NextRequest)));
				finish();
				return;
			}
			state.TranscriptLine = *line;
		}
		server.SubmitInProcess(state.Client, RpcRequest{
												 .Id = Json(state.RequestId),
												 .IsNotification = false,
												 .Method = request.Method,
												 .Params = std::move(params),
												 .TranscriptLine = state.TranscriptLine,
											 });
		state.InFlight = true;
	}

	bool BatchRunner::IsFinished() const
	{
		return m_State->Finished;
	}

}
