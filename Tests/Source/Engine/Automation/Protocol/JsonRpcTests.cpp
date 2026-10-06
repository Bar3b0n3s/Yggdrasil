#include "TestsPCH.h"

#include "Engine/Automation/Protocol/JsonRpc.h"

#include "Engine/Core/Json/JsonReader.h"

namespace Engine {

	static Json ParseRpcTestJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("JsonRpc: a request parses with its id, method, params and transcript line" * doctest::skip(true))
		{
			const auto request = ParseRpcRequest(
				R"({"jsonrpc":"2.0","id":17,"method":"entity.create","params":{"name":"Board","_meta":{"transcriptLine":42}}})");
			REQUIRE(request.has_value());
			CHECK(request->Id == Json(17));
			CHECK_FALSE(request->IsNotification);
			CHECK(request->Method == "entity.create");
			CHECK(request->Params == ParseRpcTestJson(R"({"name":"Board"})"));
			CHECK(request->TranscriptLine == 42u);

			const auto notification = ParseRpcRequest(R"({"jsonrpc":"2.0","method":"session.info"})");
			REQUIRE(notification.has_value());
			CHECK(notification->IsNotification);
			CHECK(notification->Params == Json::object());

			const auto stringId = ParseRpcRequest(R"({"jsonrpc":"2.0","id":"a-1","method":"session.info","params":{}})");
			REQUIRE(stringId.has_value());
			CHECK(stringId->Id == Json("a-1"));
		}

		TEST_CASE("JsonRpc: invalid JSON is ParseError and a non-request is InvalidRequest with the id recovered" * doctest::skip(true))
		{
			const auto notJson = ParseRpcRequest("{\"jsonrpc\":");
			REQUIRE_FALSE(notJson.has_value());
			CHECK(notJson.error().Code == RpcErrorCode::ParseError);
			CHECK(notJson.error().Id.is_null());

			const auto badVersion = ParseRpcRequest(R"({"jsonrpc":"1.0","id":3,"method":"session.info"})");
			REQUIRE_FALSE(badVersion.has_value());
			CHECK(badVersion.error().Code == RpcErrorCode::InvalidRequest);
			CHECK(badVersion.error().Id == Json(3));

			const std::array<std::string_view, 7> invalid = {
				R"([{"jsonrpc":"2.0","id":1,"method":"a.b"}])",
				R"("text")",
				R"({"jsonrpc":"2.0","id":1})",
				R"({"jsonrpc":"2.0","id":1,"method":"a.b","params":[1]})",
				R"({"jsonrpc":"2.0","id":null,"method":"a.b"})",
				R"({"jsonrpc":"2.0","id":1.5,"method":"a.b"})",
				R"({"jsonrpc":"2.0","id":1,"method":"a.b","params":{"_meta":{"transcriptLine":0}}})",
			};
			for (const std::string_view payload : invalid)
			{
				INFO(std::string(payload));
				const auto failure = ParseRpcRequest(payload);
				REQUIRE_FALSE(failure.has_value());
				CHECK(failure.error().Code == RpcErrorCode::InvalidRequest);
			}
		}

		TEST_CASE("JsonRpc: error codes map as documented" * doctest::skip(true))
		{
			CHECK(ToRpcErrorCode(ErrorCode::InvalidArgument) == RpcErrorCode::InvalidParams);
			CHECK(ToRpcErrorCode(ErrorCode::NotFound) == RpcErrorCode::NotFound);
			CHECK(ToRpcErrorCode(ErrorCode::InvalidState) == RpcErrorCode::InvalidState);
			CHECK(ToRpcErrorCode(ErrorCode::Validation) == RpcErrorCode::ValidationFailed);
			CHECK(ToRpcErrorCode(ErrorCode::Parse) == RpcErrorCode::ValidationFailed);
			CHECK(ToRpcErrorCode(ErrorCode::UnsupportedVersion) == RpcErrorCode::ValidationFailed);
			CHECK(ToRpcErrorCode(ErrorCode::ImportFailed) == RpcErrorCode::ValidationFailed);
			CHECK(ToRpcErrorCode(ErrorCode::AlreadyExists) == RpcErrorCode::Conflict);
			CHECK(ToRpcErrorCode(ErrorCode::Conflict) == RpcErrorCode::Conflict);
			CHECK(ToRpcErrorCode(ErrorCode::Timeout) == RpcErrorCode::Timeout);
			CHECK(ToRpcErrorCode(ErrorCode::Script) == RpcErrorCode::ScriptError);
			CHECK(ToRpcErrorCode(ErrorCode::CompileFailed) == RpcErrorCode::ScriptError);
			CHECK(ToRpcErrorCode(ErrorCode::PermissionDenied) == RpcErrorCode::Unauthorized);
			CHECK(ToRpcErrorCode(ErrorCode::Unsupported) == RpcErrorCode::Unsupported);
			CHECK(ToRpcErrorCode(ErrorCode::Cancelled) == RpcErrorCode::Cancelled);
			CHECK(ToRpcErrorCode(ErrorCode::Io) == RpcErrorCode::Internal);
			CHECK(ToRpcErrorCode(ErrorCode::Gpu) == RpcErrorCode::Internal);
			CHECK(ToRpcErrorCode(ErrorCode::Unknown) == RpcErrorCode::Internal);
			CHECK(RpcErrorCodeToString(RpcErrorCode::Busy) == "Busy");
			CHECK(RpcErrorCodeToString(RpcErrorCode::InvalidParams) == "InvalidParams");
		}

		TEST_CASE("JsonRpc: error responses carry errorCode, issues, extra data and _meta" * doctest::skip(true))
		{
			const Error error = Error(ErrorCode::Validation, "2 invalid fields")
									.WithHint("check the field names")
									.WithIssue({ .JsonPointer = "/components/RigidBody/Mas",
										.Message = "unknown field 'Mas' on 'RigidBody'",
										.Hint = "did you mean 'Mass'?",
										.Suggestions = { "Mass" } });
			Json response = MakeErrorResponse(Json(8), RpcErrorCode::InvalidParams, error, Json{ { "failedOp", 3 } }, Json{ { "revision", 5 } });
			CHECK(response["jsonrpc"] == Json("2.0"));
			CHECK(response["id"] == Json(8));
			CHECK(response["error"]["code"] == Json(-32602));
			CHECK(response["error"]["message"] == Json("Invalid params"));
			Json& data = response["error"]["data"];
			CHECK(data["errorCode"] == Json("Validation"));
			CHECK(data["detail"] == Json("2 invalid fields"));
			CHECK(data["hint"] == Json("check the field names"));
			CHECK(data["issues"][0]["pointer"] == Json("/components/RigidBody/Mas"));
			CHECK(data["issues"][0]["hint"] == Json("did you mean 'Mass'?"));
			CHECK(data["failedOp"] == Json(3));
			CHECK(data["_meta"]["revision"] == Json(5));

			Json engineCode = MakeErrorResponse(Json("x"), Error(ErrorCode::NotFound, "no entity '/Game'"), Json(), Json());
			CHECK(engineCode["error"]["code"] == Json(-32001));
			CHECK(engineCode["error"]["message"] == Json("no entity '/Game'"));
			CHECK_FALSE(engineCode["error"]["data"].contains("_meta"));
			CHECK(engineCode["error"]["data"]["issues"] == Json::array());
		}

		TEST_CASE("JsonRpc: results carry _meta inside the result object" * doctest::skip(true))
		{
			Json response = MakeResultResponse(Json(4), Json{ { "entity", Json{ { "id", "5d1c9a7e33b04f12" } } } }, Json{ { "revision", 9 } });
			CHECK(response["result"]["entity"]["id"] == Json("5d1c9a7e33b04f12"));
			CHECK(response["result"]["_meta"]["revision"] == Json(9));
			CHECK_FALSE(response.contains("error"));
		}

		TEST_CASE("JsonRpc: ProtocolVersion parses and prints major.minor" * doctest::skip(true))
		{
			CHECK(CurrentProtocolVersion.ToString() == "1.0");
			CHECK(ProtocolVersion::Parse("1.0") == ProtocolVersion{ 1, 0 });
			CHECK(ProtocolVersion::Parse("2.13") == ProtocolVersion{ 2, 13 });
			const std::array<std::string_view, 11> bad = { "", "1", "1.", ".0", "01.0", "1.00", "-1.0", "1.0.0", " 1.0", "1.0 ", "1234567890.0" };
			for (const std::string_view text : bad)
			{
				INFO(std::string(text));
				CHECK_FALSE(ProtocolVersion::Parse(text).has_value());
			}
		}

		TEST_CASE("JsonRpc: the Busy response names the phase" * doctest::skip(true))
		{
			Json busy = MakeBusyResponse(Json(12), "Automation:debug.stall", 5200);
			CHECK(busy["error"]["code"] == Json(-32007));
			CHECK(busy["error"]["data"]["errorCode"] == Json("Busy"));
			CHECK(busy["error"]["data"]["phase"] == Json("Automation:debug.stall"));
			CHECK(busy["error"]["data"]["stalledMilliseconds"] == Json(5200));
			CHECK_FALSE(busy["error"]["data"].contains("_meta"));
		}
	}

}
