#include "TestsPCH.h"
#include "Support/AutomationTestClient.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Support/TestData.h"

namespace Engine {

	namespace Test {

		AutomationServerSpecification MakeTestServerSpecification()
		{
			return {
				.Listen = false,
				.TestHooks = true,
				.Headless = true,
				.RendererName = "none",
				.DocsRoot = GetRepositoryRoot(),
			};
		}

		AutomationTestClient::AutomationTestClient(EditorContext& editor, AutomationServerSpecification specification, bool offloadLargeResults)
		{
			Result<Scope<AutomationServer>> server = AutomationServer::Create(editor, specification);
			REQUIRE_MESSAGE(server.has_value(), server.error().ToString());
			m_Server = std::move(*server);
			m_Client = m_Server->ConnectInProcess("test", offloadLargeResults);
			REQUIRE(m_Client != NoClient);
		}

		AutomationTestClient::~AutomationTestClient()
		{
			if (m_Server != nullptr && m_Client != NoClient)
				m_Server->DisconnectInProcess(m_Client);
		}

		int64_t AutomationTestClient::Submit(std::string_view method, const Json& params)
		{
			const int64_t id = m_NextId++;
			m_Server->SubmitInProcess(m_Client, RpcRequest{
													.Id = Json(id),
													.IsNotification = false,
													.Method = std::string(method),
													.Params = params.is_null() ? Json::object() : params,
													.TranscriptLine = std::nullopt,
												});
			return id;
		}

		Json AutomationTestClient::Request(std::string_view method, const Json& params, uint32_t maxPumps)
		{
			const int64_t id = Submit(method, params);
			for (uint32_t pump = 0; pump < maxPumps; ++pump)
			{
				m_Server->Pump();
				for (Json& response : m_Server->TakeInProcessResponses(m_Client))
				{
					if (response.contains("id") && response["id"] == Json(id))
						return std::move(response);
				}
			}
			FAIL_CHECK("no response to '" << std::string(method) << "' after " << maxPumps << " pumps");
			return Json();
		}

		AutomationFixture::AutomationFixture(std::string_view label, bool openScene, std::optional<AudioEngineSpecification> audio)
			: m_Fixture(label, {}, nullptr, audio)
		{
			m_Fixture.CreateAndOpenProject();
			if (openScene)
				m_Fixture.CreateAndOpenScene();
			m_Client = CreateScope<AutomationTestClient>(m_Fixture.GetEditor());
		}

		Result<Json> AutomationTestClient::Call(std::string_view method, const Json& params)
		{
			Json response = Request(method, params);
			if (response.contains("result"))
				return std::move(response["result"]);
			if (!response.contains("error"))
				return MakeError(ErrorCode::Unknown, "'{}' got neither a result nor an error", method);

			const JsonReader error(response["error"]);
			const std::optional<JsonReader> data = error.FindMember("data");
			std::string codeName;
			std::string detail;
			if (data.has_value())
			{
				codeName = data->ReadMember<std::string>("errorCode").value_or(std::string());
				detail = data->ReadMember<std::string>("detail").value_or(std::string());
			}
			// The issues are collected and attached once: `failure = std::move(failure).WithIssue(...)` would move-assign the
			// error to itself, which leaves its members empty outside MSVC's standard library (Error.h, WithIssues).
			std::vector<ErrorIssue> collected;
			if (data.has_value())
			{
				if (const std::optional<JsonReader> issues = data->FindMember("issues"); issues.has_value())
				{
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
				}
			}
			return std::unexpected(Error(ErrorCodeFromString(codeName).value_or(ErrorCode::Unknown), std::move(detail)).WithIssues(std::move(collected)));
		}

	}

}
