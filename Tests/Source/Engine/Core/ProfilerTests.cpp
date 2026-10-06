#include "TestsPCH.h"

#include "Engine/Core/Profiler.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Support/DeathTest.h"

#include <nlohmann/json.hpp>

#include <thread>

namespace Engine {

	static std::vector<ProfileZone> ZonesNamed(const std::vector<ProfileZone>& zones, std::string_view name)
	{
		std::vector<ProfileZone> matching;
		for (const ProfileZone& zone : zones)
		{
			if (zone.Name == name)
				matching.push_back(zone);
		}
		return matching;
	}

	ENGINE_DEATH_TEST("Core/ProfilerInitializeTwice")
	{
		// The Tests main has initialized the profiler already.
		Profiler::Initialize();
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("Profiler: scopes record nested zones on the calling thread" * doctest::skip(true))
		{
			REQUIRE(Profiler::IsInitialized());
			const uint64_t since = Profiler::GetTimeNs();
			{
				ENGINE_PROFILE_SCOPE("ProfilerTests.Outer");
				{
					ENGINE_PROFILE_SCOPE("ProfilerTests.Inner");
				}
			}

			const std::vector<ProfileZone> zones = Profiler::CollectZones(since);
			const std::vector<ProfileZone> outer = ZonesNamed(zones, "ProfilerTests.Outer");
			const std::vector<ProfileZone> inner = ZonesNamed(zones, "ProfilerTests.Inner");
			REQUIRE(outer.size() == 1);
			REQUIRE(inner.size() == 1);
			CHECK(outer[0].Depth + 1 == inner[0].Depth);
			CHECK(outer[0].ThreadIndex == inner[0].ThreadIndex);
			CHECK(outer[0].BeginNs <= inner[0].BeginNs);
			CHECK(inner[0].EndNs <= outer[0].EndNs);
			CHECK(inner[0].BeginNs <= inner[0].EndNs);
		}

		TEST_CASE("Profiler: zones of other threads get their own thread index" * doctest::skip(true))
		{
			const uint64_t since = Profiler::GetTimeNs();
			{
				ENGINE_PROFILE_SCOPE("ProfilerTests.Main");
			}
			std::thread worker([]()
			{
				Profiler::SetThreadName("ProfilerTests worker");
				ENGINE_PROFILE_SCOPE("ProfilerTests.Worker");
			});
			worker.join();

			const std::vector<ProfileZone> zones = Profiler::CollectZones(since);
			const std::vector<ProfileZone> onMain = ZonesNamed(zones, "ProfilerTests.Main");
			const std::vector<ProfileZone> other = ZonesNamed(zones, "ProfilerTests.Worker");
			REQUIRE(onMain.size() == 1);
			REQUIRE(other.size() == 1);
			CHECK(onMain[0].ThreadIndex != other[0].ThreadIndex);

			for (size_t index = 1; index < zones.size(); ++index)
				CHECK(zones[index - 1].BeginNs <= zones[index].BeginNs);
		}

		TEST_CASE("Profiler: GPU zones are merged with the GPU thread index" * doctest::skip(true))
		{
			const uint64_t since = Profiler::GetTimeNs();
			std::string passName = "ProfilerTests.ShadowPass";
			ProfileZone gpuZone;
			gpuZone.Name = passName;
			gpuZone.BeginNs = since + 10;
			gpuZone.EndNs = since + 20;
			Profiler::SubmitGpuZones(std::span<const ProfileZone>(&gpuZone, 1));
			passName = "overwritten after submission";

			const std::vector<ProfileZone> gpu = ZonesNamed(Profiler::CollectZones(since), "ProfilerTests.ShadowPass");
			REQUIRE(gpu.size() == 1);
			CHECK(gpu[0].ThreadIndex == Profiler::GpuThreadIndex);
			CHECK(gpu[0].EndNs - gpu[0].BeginNs == 10);
		}

		TEST_CASE("Profiler: the Chrome trace export is JSON with complete events" * doctest::skip(true))
		{
			const uint64_t since = Profiler::GetTimeNs();
			{
				ENGINE_PROFILE_SCOPE("ProfilerTests.Exported");
			}

			const std::string trace = Profiler::ExportChromeTrace(since);
			Result<Json> document = JsonReader::Parse(trace);
			REQUIRE(document.has_value());
			const JsonReader root(*document);
			Result<JsonReader> events = root.GetMember("traceEvents");
			REQUIRE(events.has_value());
			const Result<size_t> count = events->GetArraySize();
			REQUIRE(count.has_value());

			bool found = false;
			for (size_t index = 0; index < *count; ++index)
			{
				Result<JsonReader> event = events->GetElement(index);
				REQUIRE(event.has_value());
				if (event->ReadMember<std::string>("name") == std::string("ProfilerTests.Exported"))
				{
					found = true;
					CHECK(event->ReadMember<std::string>("ph") == std::string("X"));
					CHECK(event->HasMember("ts"));
					CHECK(event->HasMember("dur"));
					CHECK(event->HasMember("tid"));
				}
			}
			CHECK(found);
		}

		TEST_CASE("Profiler: Shutdown is idempotent and zones recorded while shut down are discarded" * doctest::skip(true))
		{
			// This test thread is the only one recording, as the Initialize/Shutdown ordering rule requires.
			Profiler::Shutdown();
			Profiler::Shutdown();
			const bool initializedAfterShutdown = Profiler::IsInitialized();
			const uint64_t timeWhileShutDown = Profiler::GetTimeNs();
			{
				ENGINE_PROFILE_SCOPE("ProfilerTests.Discarded");
			}
			Profiler::Initialize();
			{
				ENGINE_PROFILE_SCOPE("ProfilerTests.AfterReinitialize");
			}

			CHECK_FALSE(initializedAfterShutdown);
			CHECK(timeWhileShutDown == 0);
			const std::vector<ProfileZone> zones = Profiler::CollectZones();
			CHECK(ZonesNamed(zones, "ProfilerTests.Discarded").empty());
			// The thread's ring of the previous initialization was freed; the next zone registers a new one.
			CHECK(ZonesNamed(zones, "ProfilerTests.AfterReinitialize").size() == 1);
		}

		TEST_CASE("Profiler: Initialize while initialized is a programmer error" * doctest::skip(true))
		{
			Test::CheckDeath("Core/ProfilerInitializeTwice", "Assertion failed");
		}

		TEST_CASE("Profiler: ENGINE_PROFILE_SCOPE accepts a name held in a pointer to a string literal" * doctest::skip(true))
		{
			// Compiles in every configuration: in Dist the macro still references `name` (no unused variable there).
			const uint64_t since = Profiler::GetTimeNs();
			const char* const name = "ProfilerTests.NamedByPointer";
			{
				ENGINE_PROFILE_SCOPE(name);
			}
			CHECK(ZonesNamed(Profiler::CollectZones(since), "ProfilerTests.NamedByPointer").size() == 1);
		}
	}

}
