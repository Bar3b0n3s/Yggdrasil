#include "TestsPCH.h"

#include "Engine/Core/Profiler.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Support/DeathTest.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <latch>
#include <thread>

namespace Engine {

	namespace {

		// Gives a test case its own profiler initialization and restores the previous state afterwards, whether or not
		// the Tests main initialized the profiler. Every test that uses it is the only thread recording while it is
		// constructed and destroyed, as the Initialize/Shutdown ordering rule requires (threads it starts are joined
		// inside its lifetime).
		class ScopedProfiler
		{
		public:
			explicit ScopedProfiler(const ProfilerSpecification& specification = {})
				: m_WasInitialized(Profiler::IsInitialized())
			{
				Profiler::Shutdown();
				Profiler::Initialize(specification);
			}

			~ScopedProfiler()
			{
				Profiler::Shutdown();
				if (m_WasInitialized)
					Profiler::Initialize();
			}

			ScopedProfiler(const ScopedProfiler&) = delete;
			ScopedProfiler& operator=(const ScopedProfiler&) = delete;
		private:
			bool m_WasInitialized = false;
		};

	}

	// The zone names the stress test records with; the zone counter picks one.
	static constexpr std::array<const char*, 3> StressZoneNames = { "ProfilerTests.StressA", "ProfilerTests.StressB",
		"ProfilerTests.StressC" };

	// True when `zone` is one the stress test could have recorded: every field derives from the thread number and
	// counter encoded in BeginNs.
	static bool IsConsistentStressZone(const ProfileZone& zone, uint64_t threadCount)
	{
		const uint64_t thread = zone.BeginNs >> 32;
		const uint64_t counter = (zone.BeginNs & 0xffffffffu) / 2;
		return thread >= 1 && thread <= threadCount && zone.EndNs == zone.BeginNs + 1 && zone.Depth == static_cast<uint32_t>(counter % 5)
			&& zone.Name == StressZoneNames[counter % StressZoneNames.size()];
	}

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
		// Initializes the profiler first when the Tests main has not, so the second call is always the repeated one.
		if (!Profiler::IsInitialized())
			Profiler::Initialize();
		Profiler::Initialize();
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("Profiler: scopes record nested zones on the calling thread")
		{
			const ScopedProfiler profiler;
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

		TEST_CASE("Profiler: zones of other threads get their own thread index")
		{
			const ScopedProfiler profiler;
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
			CHECK(other[0].Depth == 0);

			for (size_t index = 1; index < zones.size(); ++index)
				CHECK(zones[index - 1].BeginNs <= zones[index].BeginNs);
		}

		TEST_CASE("Profiler: zones of exited threads stay collectable")
		{
			constexpr size_t ThreadCount = 4;
			const ScopedProfiler profiler;
			// Short-lived threads, like the workers of the many JobSystems a test run creates, one after the other.
			for (size_t thread = 0; thread < ThreadCount; ++thread)
			{
				std::thread worker([]()
				{
					ENGINE_PROFILE_SCOPE("ProfilerTests.Exited");
				});
				worker.join();
			}

			const std::vector<ProfileZone> exited = ZonesNamed(Profiler::CollectZones(), "ProfilerTests.Exited");
			REQUIRE(exited.size() == ThreadCount);
			std::vector<uint32_t> threadIndices;
			for (const ProfileZone& zone : exited)
				threadIndices.push_back(zone.ThreadIndex);
			std::ranges::sort(threadIndices);
			CHECK(std::ranges::adjacent_find(threadIndices) == threadIndices.end());
		}

		TEST_CASE("Profiler: a full ring keeps the newest zones")
		{
			const ScopedProfiler profiler(ProfilerSpecification{ .ZonesPerThread = 4 });
			for (uint64_t index = 0; index < 10; ++index)
				Profiler::RecordZone("ProfilerTests.Ring", 100 + index * 10, 105 + index * 10, 0);

			const std::vector<ProfileZone> zones = ZonesNamed(Profiler::CollectZones(), "ProfilerTests.Ring");
			REQUIRE(zones.size() == 4);
			for (size_t index = 0; index < zones.size(); ++index)
			{
				CHECK(zones[index].BeginNs == 160 + index * 10);
				CHECK(zones[index].EndNs == 165 + index * 10);
			}
		}

		TEST_CASE("Profiler: CollectZones keeps zones that end at or after the given time, in the documented order")
		{
			const ScopedProfiler profiler;
			Profiler::RecordZone("ProfilerTests.EndsEarly", 10, 20, 0);
			Profiler::RecordZone("ProfilerTests.EndsAtCutoff", 15, 30, 1);
			Profiler::RecordZone("ProfilerTests.SameBeginDeeper", 40, 45, 2);
			Profiler::RecordZone("ProfilerTests.SameBegin", 40, 50, 1);

			const std::vector<ProfileZone> zones = Profiler::CollectZones(30);
			REQUIRE(zones.size() == 3);
			CHECK(zones[0].Name == "ProfilerTests.EndsAtCutoff");
			CHECK(zones[1].Name == "ProfilerTests.SameBegin"); // equal BeginNs and thread: lower depth first
			CHECK(zones[2].Name == "ProfilerTests.SameBeginDeeper");
		}

		TEST_CASE("Profiler: concurrent recording and collection never yields a torn zone")
		{
			// Each thread records zones whose fields are all derived from one counter, into rings small enough to wrap
			// thousands of times while the main thread collects. A zone mixing two writes would break the derivation.
			constexpr size_t ThreadCount = 4;
			constexpr uint64_t ZonesPerRecorder = 200000;
			constexpr size_t RingSize = 64;
			const ScopedProfiler profiler(ProfilerSpecification{ .ZonesPerThread = RingSize });

			// The recorders start together, once the main thread is about to collect.
			std::latch start(1);
			std::atomic<size_t> finished = 0;
			std::vector<std::thread> recorders;
			for (size_t thread = 1; thread <= ThreadCount; ++thread)
			{
				recorders.emplace_back([&start, &finished, thread]()
				{
					start.wait();
					for (uint64_t counter = 0; counter < ZonesPerRecorder; ++counter)
					{
						const uint64_t beginNs = (static_cast<uint64_t>(thread) << 32) | (counter * 2);
						Profiler::RecordZone(StressZoneNames[counter % StressZoneNames.size()], beginNs, beginNs + 1,
							static_cast<uint32_t>(counter % 5));
					}
					++finished;
				});
			}

			// Counted rather than checked one by one: a collection loop runs thousands of times.
			size_t collections = 0;
			size_t oversizedCollections = 0;
			size_t tornZones = 0;
			start.count_down();
			while (finished.load() < ThreadCount)
			{
				const std::vector<ProfileZone> zones = Profiler::CollectZones();
				if (zones.size() > ThreadCount * RingSize)
					++oversizedCollections;
				tornZones += static_cast<size_t>(std::ranges::count_if(zones, [](const ProfileZone& zone)
				{
					return !IsConsistentStressZone(zone, ThreadCount);
				}));
				++collections;
			}
			for (std::thread& recorder : recorders)
				recorder.join();
			CHECK(collections > 0);
			CHECK(oversizedCollections == 0);
			CHECK(tornZones == 0);

			// Afterwards every ring holds exactly its thread's newest RingSize zones.
			const std::vector<ProfileZone> zones = Profiler::CollectZones();
			REQUIRE(zones.size() == ThreadCount * RingSize);
			std::array<uint64_t, ThreadCount + 1> perThread{};
			for (const ProfileZone& zone : zones)
			{
				REQUIRE(IsConsistentStressZone(zone, ThreadCount));
				const uint64_t counter = (zone.BeginNs & 0xffffffffu) / 2;
				CHECK(counter >= ZonesPerRecorder - RingSize);
				++perThread[static_cast<size_t>(zone.BeginNs >> 32)];
			}
			for (size_t thread = 1; thread <= ThreadCount; ++thread)
				CHECK(perThread[thread] == RingSize);
		}

		TEST_CASE("Profiler: GPU zones are merged with the GPU thread index")
		{
			const ScopedProfiler profiler;
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

		TEST_CASE("Profiler: GPU zones that end before they begin are dropped")
		{
			const ScopedProfiler profiler;
			const std::array<ProfileZone, 2> zones = {
				ProfileZone{ .Name = "ProfilerTests.Backwards", .BeginNs = 50, .EndNs = 40 },
				ProfileZone{ .Name = "ProfilerTests.Forwards", .BeginNs = 50, .EndNs = 60 },
			};
			Profiler::SubmitGpuZones(zones);

			const std::vector<ProfileZone> collected = Profiler::CollectZones();
			CHECK(ZonesNamed(collected, "ProfilerTests.Backwards").empty());
			CHECK(ZonesNamed(collected, "ProfilerTests.Forwards").size() == 1);
		}

		TEST_CASE("Profiler: the Chrome trace export is JSON with complete events")
		{
			const ScopedProfiler profiler;
			const uint64_t since = Profiler::GetTimeNs();
			// A thread of its own, so the name does not stick to the test runner's thread.
			std::thread named([]()
			{
				Profiler::SetThreadName("ProfilerTests exporter");
				ENGINE_PROFILE_SCOPE("ProfilerTests.Exported");
			});
			named.join();
			const ProfileZone gpuZone{ .Name = "ProfilerTests.\"Quoted\" pass", .BeginNs = since + 1500, .EndNs = since + 4250 };
			Profiler::SubmitGpuZones(std::span<const ProfileZone>(&gpuZone, 1));

			// Read through JsonReader, so the export also passes the engine's strict parser (no duplicate keys, valid UTF-8).
			const std::string trace = Profiler::ExportChromeTrace(since);
			const Result<Json> document = JsonReader::Parse(trace);
			REQUIRE(document.has_value());
			const Result<JsonReader> events = JsonReader(*document).GetMember("traceEvents");
			REQUIRE(events.has_value());
			const Result<size_t> eventCount = events->GetArraySize();
			REQUIRE(eventCount.has_value());

			bool foundCpu = false;
			bool foundGpu = false;
			bool foundThreadName = false;
			for (size_t index = 0; index < *eventCount; ++index)
			{
				const Result<JsonReader> event = events->GetElement(index);
				REQUIRE(event.has_value());
				const Result<std::string> name = event->ReadMember<std::string>("name");
				const Result<std::string> phase = event->ReadMember<std::string>("ph");
				const Result<uint32_t> threadIndex = event->ReadMember<uint32_t>("tid");
				REQUIRE(name.has_value());
				REQUIRE(phase.has_value());
				REQUIRE(threadIndex.has_value());
				REQUIRE(event->ReadMember<uint32_t>("pid").has_value());
				if (*phase == "X")
				{
					const Result<double> timestamp = event->ReadMember<double>("ts");
					const Result<double> duration = event->ReadMember<double>("dur");
					const Result<std::string> category = event->ReadMember<std::string>("cat");
					REQUIRE(timestamp.has_value());
					REQUIRE(duration.has_value());
					REQUIRE(category.has_value());
					if (*name == "ProfilerTests.Exported")
					{
						foundCpu = true;
						CHECK(*category == "cpu");
					}
					if (*name == "ProfilerTests.\"Quoted\" pass")
					{
						foundGpu = true;
						CHECK(*category == "gpu");
						CHECK(*duration == 2.75); // 2750 ns in microseconds, written exactly as "2.750"
						CHECK(*threadIndex == Profiler::GpuThreadIndex);
					}
				}
				else if (*phase == "M" && *name == "thread_name")
				{
					const Result<JsonReader> arguments = event->GetMember("args");
					REQUIRE(arguments.has_value());
					const Result<std::string> threadName = arguments->ReadMember<std::string>("name");
					REQUIRE(threadName.has_value());
					if (*threadName == "ProfilerTests exporter")
						foundThreadName = true;
				}
			}
			CHECK(foundCpu);
			CHECK(foundGpu);
			CHECK(foundThreadName);
		}

		TEST_CASE("Profiler: Shutdown is idempotent and zones recorded while shut down are discarded")
		{
			// This test thread is the only one recording, as the Initialize/Shutdown ordering rule requires.
			const ScopedProfiler profiler;
			Profiler::Shutdown();
			Profiler::Shutdown();
			const bool initializedAfterShutdown = Profiler::IsInitialized();
			const uint64_t timeWhileShutDown = Profiler::GetTimeNs();
			{
				ENGINE_PROFILE_SCOPE("ProfilerTests.Discarded");
			}
			Profiler::RecordZone("ProfilerTests.DiscardedDirect", 1, 2, 0);
			Profiler::Initialize();
			{
				ENGINE_PROFILE_SCOPE("ProfilerTests.AfterReinitialize");
			}

			CHECK_FALSE(initializedAfterShutdown);
			CHECK(timeWhileShutDown == 0);
			const std::vector<ProfileZone> zones = Profiler::CollectZones();
			CHECK(ZonesNamed(zones, "ProfilerTests.Discarded").empty());
			CHECK(ZonesNamed(zones, "ProfilerTests.DiscardedDirect").empty());
			// The thread's ring of the previous initialization was freed; the next zone registers a new one.
			CHECK(ZonesNamed(zones, "ProfilerTests.AfterReinitialize").size() == 1);
		}

		TEST_CASE("Profiler: a scope open across a re-initialization records nothing")
		{
			const ScopedProfiler profiler;
			{
				ENGINE_PROFILE_SCOPE("ProfilerTests.SpansShutdown");
				Profiler::Shutdown();
				Profiler::Initialize();
			}
			{
				ENGINE_PROFILE_SCOPE("ProfilerTests.AfterSpan");
			}

			const std::vector<ProfileZone> zones = Profiler::CollectZones();
			CHECK(ZonesNamed(zones, "ProfilerTests.SpansShutdown").empty());
			const std::vector<ProfileZone> after = ZonesNamed(zones, "ProfilerTests.AfterSpan");
			REQUIRE(after.size() == 1);
			CHECK(after[0].Depth == 0); // the abandoned scope still closed its nesting level
		}

		TEST_CASE("Profiler: Initialize while initialized is a programmer error")
		{
			ENGINE_CHECK_DEATH("Core/ProfilerInitializeTwice", "Assertion failed");
		}

		TEST_CASE("Profiler: ENGINE_PROFILE_SCOPE accepts a name held in a pointer to a string literal")
		{
			// Compiles in every configuration: in Dist the macro still references `name` (no unused variable there).
			const ScopedProfiler profiler;
			const uint64_t since = Profiler::GetTimeNs();
			const char* const name = "ProfilerTests.NamedByPointer";
			{
				ENGINE_PROFILE_SCOPE(name);
			}
			CHECK(ZonesNamed(Profiler::CollectZones(since), "ProfilerTests.NamedByPointer").size() == 1);
		}
	}

}
