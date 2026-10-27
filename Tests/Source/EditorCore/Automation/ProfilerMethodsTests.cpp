#include "TestsPCH.h"

#include "EditorCore/Automation/ProfilerMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Profiler.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// profiler.capture (M18, R-PROF; Docs/Decisions/0029-m18-contract.md decision 11) through the in-process server, which
// has no renderer: the trace holds CPU zones only. The GPU zones are checked against a rendering editor
// (Tests/Automation/test_renderer_tools.py).

namespace Engine {

	namespace {

		// Gives a test case a profiler with rings of `zonesPerThread` zones and restores the default one afterwards, whether
		// or not the Tests main initialized the profiler. The case records only on its own thread while it is constructed and
		// destroyed, and every thread its fixture starts is joined inside its lifetime (Profiler.h's ordering rule).
		class ScopedSmallProfiler
		{
		public:
			explicit ScopedSmallProfiler(size_t zonesPerThread)
				: m_WasInitialized(Profiler::IsInitialized())
			{
				Profiler::Shutdown();
				Profiler::Initialize(ProfilerSpecification{ .ZonesPerThread = zonesPerThread });
			}

			~ScopedSmallProfiler()
			{
				Profiler::Shutdown();
				if (m_WasInitialized)
					Profiler::Initialize();
			}

			ScopedSmallProfiler(const ScopedSmallProfiler&) = delete;
			ScopedSmallProfiler& operator=(const ScopedSmallProfiler&) = delete;
		private:
			bool m_WasInitialized = false;
		};

	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("ProfilerMethods: profiler.capture writes one Chrome trace of the zones of the next frames")
		{
			REQUIRE(Profiler::IsInitialized());
			Test::AutomationFixture fixture("ProfilerCapture");
			const Result<Json> result = fixture.Call("profiler.capture", Json{ { "frames", 3 } });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK((*result)["frames"] == 3);
			CHECK((*result)["gpuZones"] == 0);
			const Result<std::string> path = JsonReader((*result)["path"]).ReadString();
			REQUIRE(path.has_value());
			CHECK(std::filesystem::path(*path).extension() == ".json");
			const Result<std::string> text = FileSystem::ReadText(std::filesystem::path(*path));
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			const Json trace = Json::parse(*text, nullptr, false);
			REQUIRE_FALSE(trace.is_discarded());
			REQUIRE(trace.contains("traceEvents"));
			REQUIRE(trace["traceEvents"].is_array());
			// Every zone is one complete ("X") event.
			size_t completeEvents = 0;
			for (const Json& event : trace["traceEvents"])
				completeEvents += event.contains("ph") && event["ph"] == "X" ? 1U : 0U;
			const Result<uint64_t> cpuZones = JsonReader((*result)["cpuZones"]).ReadUInt64();
			REQUIRE(cpuZones.has_value());
			CHECK(completeEvents == *cpuZones);
			// Three frames fit the profiler's rings: the trace is complete, and says so.
			CHECK((*result)["droppedZones"] == 0);
			REQUIRE(trace.contains("otherData"));
			REQUIRE(trace["otherData"].contains("droppedZones"));
			CHECK(trace["otherData"]["droppedZones"] == 0);
		}

		TEST_CASE("ProfilerMethods: a capture whose zones overflow the profiler's rings reports the zones it dropped")
		{
			// The profiler gets rings of 64 zones per thread for this case, and the main thread records 200 zones before each
			// pump while a capture of 2 frames runs: its ring overwrites at least 136 zones of the capture before the trace is
			// written, which the result and the trace report instead of passing the truncated trace off as complete.
			const ScopedSmallProfiler profiler(64);
			Test::AutomationFixture fixture("ProfilerCaptureDropped");
			Test::AutomationTestClient& client = fixture.GetClient();
			AutomationServer& server = client.GetServer();
			const int64_t id = client.Submit("profiler.capture", Json{ { "frames", 2 } });
			std::vector<Json> responses;
			for (uint32_t pump = 0; pump < 1000 && responses.empty(); ++pump)
			{
				const uint64_t now = Profiler::GetTimeNs();
				for (uint32_t zone = 0; zone < 200; ++zone)
					Profiler::RecordZone("ProfilerMethodsTests.Zone", now, now, 0);
				server.Pump();
				responses = server.TakeInProcessResponses(client.GetClient());
			}
			REQUIRE(responses.size() == 1);
			CHECK(responses.front()["id"] == Json(id));
			REQUIRE(responses.front().contains("result"));
			const Json& result = responses.front()["result"];
			REQUIRE(result.contains("droppedZones"));
			const Result<uint64_t> dropped = JsonReader(result["droppedZones"]).ReadUInt64();
			REQUIRE(dropped.has_value());
			CHECK(*dropped >= 136);
			const Result<std::string> path = JsonReader(result["path"]).ReadString();
			REQUIRE(path.has_value());
			const Result<std::string> text = FileSystem::ReadText(std::filesystem::path(*path));
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			const Json trace = Json::parse(*text, nullptr, false);
			REQUIRE_FALSE(trace.is_discarded());
			REQUIRE(trace.contains("otherData"));
			REQUIRE(trace["otherData"].contains("droppedZones"));
			CHECK(trace["otherData"]["droppedZones"] == result["droppedZones"]);
		}

		TEST_CASE("ProfilerMethods: profiler.capture refuses a frame count outside 1 to 600")
		{
			Test::AutomationFixture fixture("ProfilerCaptureFrames");
			for (const uint32_t frames : { 0U, MaxProfilerCaptureFrames + 1 })
			{
				CAPTURE(frames);
				const Result<Json> refused = fixture.Call("profiler.capture", Json{ { "frames", frames } });
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			}
			const auto* method = fixture.GetClient().GetServer().GetMethods().Find("profiler.capture");
			REQUIRE(method != nullptr);
			CHECK_FALSE(method->Specification.ExposeAsTool);
			CHECK_FALSE(method->Specification.Mutates);
			CHECK_FALSE(method->Specification.AvailableInRuntime);
		}
	}

}
