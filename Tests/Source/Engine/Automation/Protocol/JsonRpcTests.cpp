#include "TestsPCH.h"

#include "Engine/Automation/Protocol/JsonRpc.h"

#include "Engine/Automation/Protocol/Handshake.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Support/Fuzz.h"

namespace Engine {

	static Json ParseRpcTestJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("JsonRpc: a request parses with its id, method, params and transcript line")
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

		TEST_CASE("JsonRpc: 1,000 seeded mutations of a request and of hello params never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			// The seeds: requests of every shape a client sends, the session.hello of the Python client among them. A
			// parsed request's params that a hello reads go through ParseHelloRequest, as the I/O thread does.
			const std::array<std::string_view, 5> seeds = {
				R"({"jsonrpc":"2.0","id":1,"method":"session.hello","params":{"token":"0123456789abcdef0123456789abcdef","protocolVersion":"1.0","client":{"name":"engine-tests","version":"1.2"}}})",
				R"({"jsonrpc":"2.0","id":17,"method":"entity.create","params":{"name":"Board","_meta":{"transcriptLine":42}}})",
				R"({"jsonrpc":"2.0","method":"session.info"})",
				R"({"jsonrpc":"2.0","id":"a-1","method":"scene.tree","params":{"depth":2,"root":"/Game"}})",
				R"([{"jsonrpc":"2.0","id":2,"method":"session.info"}])",
			};
			const Test::FuzzRun run = Test::RunFuzzCase(0x7C9C, 1000, [&seeds](Random& random, uint64_t /*index*/)
			{
				std::string mutated(seeds[static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(seeds.size()) - 1))]);
				Test::MutateText(mutated, random);
				const auto request = ParseRpcRequest(mutated);
				if (!request.has_value())
				{
					CHECK((request.error().Code == RpcErrorCode::ParseError || request.error().Code == RpcErrorCode::InvalidRequest));
					CHECK((request.error().Id.is_null() || request.error().Id.is_number_integer() || request.error().Id.is_string()));
					return false;
				}
				CHECK(request->Params.is_object());
				CHECK(request->IsNotification == request->Id.is_null());
				const Result<HelloRequest> hello = ParseHelloRequest(request->Params);
				if (!hello.has_value())
					CHECK(hello.error().GetCode() == ErrorCode::InvalidArgument);
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
			{
				CHECK(run.Counts.Rejected > 0);
				CHECK(run.Counts.Accepted > 0);
			}
		}

		TEST_CASE("JsonRpc: invalid JSON is ParseError and a non-request is InvalidRequest with the id recovered")
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

		TEST_CASE("JsonRpc: error codes map as documented")
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
			CHECK(ToRpcErrorCode(ErrorCode::OutOfMemory) == RpcErrorCode::Internal); // M18 (C18.4): no new protocol code
			CHECK(ToRpcErrorCode(ErrorCode::Unknown) == RpcErrorCode::Internal);
			CHECK(RpcErrorCodeToString(RpcErrorCode::Busy) == "Busy");
			CHECK(RpcErrorCodeToString(RpcErrorCode::InvalidParams) == "InvalidParams");
		}

		TEST_CASE("JsonRpc: error responses carry errorCode, issues, extra data and _meta")
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

		TEST_CASE("JsonRpc: results carry _meta inside the result object")
		{
			Json response = MakeResultResponse(Json(4), Json{ { "entity", Json{ { "id", "5d1c9a7e33b04f12" } } } }, Json{ { "revision", 9 } });
			CHECK(response["result"]["entity"]["id"] == Json("5d1c9a7e33b04f12"));
			CHECK(response["result"]["_meta"]["revision"] == Json(9));
			CHECK_FALSE(response.contains("error"));
		}

		TEST_CASE("JsonRpc: ProtocolVersion parses and prints major.minor")
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

		TEST_CASE("JsonRpc: the Busy response names the phase")
		{
			Json busy = MakeBusyResponse(Json(12), "Automation:debug.stall", 5200);
			CHECK(busy["error"]["code"] == Json(-32007));
			CHECK(busy["error"]["data"]["errorCode"] == Json("Busy"));
			CHECK(busy["error"]["data"]["phase"] == Json("Automation:debug.stall"));
			CHECK(busy["error"]["data"]["request"] == Json(""));
			CHECK(busy["error"]["data"]["stalledMilliseconds"] == Json(5200));
			CHECK_FALSE(busy["error"]["data"].contains("_meta"));
			CHECK_FALSE(busy["error"]["data"].contains("hint"));
		}

		// Skerries' brief S20: a second client refused while a run held the main thread was told only to "retry later".
		TEST_CASE("JsonRpc: a Busy response names the request whose step holds the main thread and to retry once that step ends")
		{
			const Json busy = MakeBusyResponse(Json(3), "run case", 6179, "test.run");
			const Json& data = busy["error"]["data"];
			CHECK(data["request"] == Json("test.run"));
			CHECK(data["phase"] == Json("run case"));
			const std::string detail = JsonReader(data["detail"]).ReadString().value_or(std::string());
			CHECK(detail.contains("6179 ms"));
			CHECK(detail.contains("a test.run request"));
			CHECK(detail.contains("retry once that step ends"));
			CHECK(busy["error"]["message"] == data["detail"]);
			CHECK(JsonReader(data["hint"]).ReadString().value_or(std::string()).contains("session.hello"));
		}

		TEST_CASE("JsonRpc: a progress notification carries the request's id and what the operation reports")
		{
			const PendingProgress progress{ .Work = 42, .Stage = PendingProgressStage::Verify, .Subject = "Record.Hops", .Tick = 120, .FinalTick = 480, .Step = 1, .Steps = 1, .Runs = 3 };
			const Json notification = MakeProgressNotification(Json("named"), "test.run", progress);
			CHECK(notification["jsonrpc"] == Json("2.0"));
			CHECK(notification["method"] == Json("$/progress"));
			CHECK_FALSE(notification.contains("id"));
			const Json& params = notification["params"];
			CHECK(params["id"] == Json("named"));
			CHECK(params["method"] == Json("test.run"));
			CHECK(params["work"] == Json(42));
			CHECK(params["stage"] == Json("verify"));
			CHECK(params["subject"] == Json("Record.Hops"));
			CHECK(params["tick"] == Json(120));
			CHECK(params["finalTick"] == Json(480));
			CHECK(params["step"] == Json(1));
			CHECK(params["steps"] == Json(1));
			CHECK(params["runs"] == Json(3));
			CHECK(params["message"] == Json("recording Record.Hops, run 2 of 3 (replayed strictly with its input): tick 120 of 480"));
		}

		TEST_CASE("JsonRpc: a progress message names the stage, its subject, a recording's run and the tick")
		{
			const auto describe = [](PendingProgressStage stage, std::string subject, uint64_t tick, uint64_t finalTick, uint32_t step, uint32_t steps, uint32_t runs = 0)
			{
				return DescribePendingProgress({ .Work = 1, .Stage = stage, .Subject = std::move(subject), .Tick = tick, .FinalTick = finalTick, .Step = step, .Steps = steps, .Runs = runs });
			};
			CHECK(describe(PendingProgressStage::Suite, "Board/clears a line", 60, 0, 2, 5) == "suite 2 of 5, Board/clears a line: tick 60");
			CHECK(describe(PendingProgressStage::Suite, "Board", 0, 0, 1, 1) == "suite Board: tick 0");
			CHECK(describe(PendingProgressStage::Replay, "Assets/Tests/Replays/Opening.replay", 300, 600, 4, 5) == "replay 4 of 5, Assets/Tests/Replays/Opening.replay: tick 300 of 600");
			CHECK(describe(PendingProgressStage::Replay, "Assets/Tests/Replays/Opening.replay", 3, 600, 0, 0) == "replay Assets/Tests/Replays/Opening.replay: tick 3 of 600");
			// The recorded run, before its input events (and so its runs) are known; then the runs of one with input
			// events and of one without, whose verification ends it.
			CHECK(describe(PendingProgressStage::Record, "Record.Hops/hop", 30, 0, 1, 1) == "recording Record.Hops/hop, run 1 (recorded): tick 30");
			CHECK(describe(PendingProgressStage::Verify, "Assets/Tests/Replays/Hops.replay", 0, 80, 0, 0, 3) == "recording Assets/Tests/Replays/Hops.replay, run 2 of 3 (replayed strictly with its input): tick 0 of 80");
			CHECK(describe(PendingProgressStage::InputCheck, "Record.Hops", 80, 80, 1, 1, 3) == "recording Record.Hops, run 3 of 3 (replayed without its input): tick 80 of 80");
			CHECK(describe(PendingProgressStage::Verify, "Record.Idle", 40, 80, 1, 1, 2) == "recording Record.Idle, run 2 of 2 (replayed strictly with its input): tick 40 of 80");
			for (const PendingProgressStage stage : { PendingProgressStage::Suite, PendingProgressStage::Replay, PendingProgressStage::Record, PendingProgressStage::Verify, PendingProgressStage::InputCheck })
				CHECK_FALSE(PendingProgressStageToString(stage).empty());
			CHECK(PendingProgressStageToString(PendingProgressStage::InputCheck) == "inputCheck");
		}
	}

}
