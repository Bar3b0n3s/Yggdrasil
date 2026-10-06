#include "EnginePCH.h"
#include "Engine/Core/Profiler.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/Json.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <format>
#include <set>
#include <tuple>
#include <unordered_map>

// Synchronization. Each thread records into its own ThreadRing without locking: the ring has a single writer, and the
// collectors read it concurrently through a seqlock-style protocol on atomics (ThreadRing::Write, AppendZones). The
// profiler mutex guards everything else: the ring list, thread names, the GPU zones and the generation bookkeeping.
// Initialize and Shutdown free rings, so they rely on the ordering rule of Profiler.h (no other thread records); every
// other path is safe at any time.

namespace Engine {

	namespace {

		// The rings of this many exited threads are kept so their zones can still be collected; older ones are freed. Long
		// lived threads (the main thread, JobSystem workers) never exit before Shutdown, but test binaries create and
		// join many short-lived workers, and each ring is ZonesPerThread zones large (2 MiB by default).
		constexpr size_t MaxRetiredRings = 16;

		constexpr uint32_t ChromeTraceProcessID = 1;

		// One zone of a ring. Every field is atomic, so a collector may read a slot while its thread overwrites it; the
		// ring's counters tell the collector which copies it can trust.
		struct ZoneSlot
		{
			std::atomic<const char*> Name{ nullptr };
			std::atomic<uint64_t> BeginNs{ 0 };
			std::atomic<uint64_t> EndNs{ 0 };
			std::atomic<uint32_t> Depth{ 0 };
		};

		// The ring buffer of one thread in one initialization. Write is called only by the owning thread; everything
		// else runs under the profiler mutex.
		class ThreadRing
		{
		public:
			ThreadRing(size_t capacity, uint32_t threadIndex, std::string name)
				: m_Slots(std::make_unique<ZoneSlot[]>(capacity)), m_Capacity(capacity), m_ThreadIndex(threadIndex), m_Name(std::move(name))
			{
			}

			ThreadRing(const ThreadRing&) = delete;
			ThreadRing& operator=(const ThreadRing&) = delete;

			// Owning thread only, lock-free. m_Begun announces the slot about to be overwritten before any of its fields
			// change, and the release fence orders that announcement before the field stores: a collector that read any
			// new field value then also reads the announcement after its acquire fence, and discards the slot.
			// m_Published (release) makes a complete slot visible.
			void Write(const char* name, uint64_t beginNs, uint64_t endNs, uint32_t depth)
			{
				const uint64_t index = m_Published.load(std::memory_order_relaxed);
				m_Begun.store(index + 1, std::memory_order_relaxed);
				std::atomic_thread_fence(std::memory_order_release);

				ZoneSlot& slot = m_Slots[static_cast<size_t>(index % m_Capacity)];
				slot.Name.store(name, std::memory_order_relaxed);
				slot.BeginNs.store(beginNs, std::memory_order_relaxed);
				slot.EndNs.store(endNs, std::memory_order_relaxed);
				slot.Depth.store(depth, std::memory_order_relaxed);

				m_Published.store(index + 1, std::memory_order_release);
			}

			// Appends the held zones that ended at or after `sinceNs`, oldest first. Any thread; may run while the owner
			// writes.
			void AppendZones(uint64_t sinceNs, std::vector<ProfileZone>& zones) const
			{
				// Zone n (0-based) lives in slot n % capacity. Every zone below `published` was completely written before
				// the acquire load, so each copy holds zone n's values or, where the owner overwrote the slot meanwhile,
				// values of a later zone.
				const uint64_t published = m_Published.load(std::memory_order_acquire);
				const uint64_t first = published > m_Capacity ? published - m_Capacity : 0;
				const size_t base = zones.size();
				for (uint64_t index = first; index < published; ++index)
				{
					const ZoneSlot& slot = m_Slots[static_cast<size_t>(index % m_Capacity)];
					const char* name = slot.Name.load(std::memory_order_relaxed);
					zones.push_back(ProfileZone{
						.Name = name != nullptr ? std::string_view(name) : std::string_view(),
						.BeginNs = slot.BeginNs.load(std::memory_order_relaxed),
						.EndNs = slot.EndNs.load(std::memory_order_relaxed),
						.ThreadIndex = m_ThreadIndex,
						.Depth = slot.Depth.load(std::memory_order_relaxed),
					});
				}

				// The owner announced zone begun - 1 before touching its slot, which held zone begun - 1 - capacity. Zones
				// from begun - capacity on are intact; older copies may mix in fields of a later zone.
				std::atomic_thread_fence(std::memory_order_acquire);
				const uint64_t begun = m_Begun.load(std::memory_order_relaxed);
				const uint64_t oldestIntact = begun > m_Capacity ? begun - m_Capacity : 0;
				const uint64_t overwritten = std::min(published, std::max(oldestIntact, first)) - first;

				const auto copied = zones.begin() + static_cast<std::ptrdiff_t>(base);
				zones.erase(copied, copied + static_cast<std::ptrdiff_t>(overwritten));
				const auto kept = std::remove_if(zones.begin() + static_cast<std::ptrdiff_t>(base), zones.end(),
					[sinceNs](const ProfileZone& zone)
				{
					return zone.EndNs < sinceNs;
				});
				zones.erase(kept, zones.end());
			}

			[[nodiscard]] uint32_t GetThreadIndex() const { return m_ThreadIndex; }
			[[nodiscard]] const std::string& GetName() const { return m_Name; }
			void SetName(std::string name) { m_Name = std::move(name); }
		private:
			std::unique_ptr<ZoneSlot[]> m_Slots;
			size_t m_Capacity = 0;
			uint32_t m_ThreadIndex = 0;
			std::string m_Name;                     // profiler mutex
			std::atomic<uint64_t> m_Begun{ 0 };     // zones whose write has started
			std::atomic<uint64_t> m_Published{ 0 }; // zones completely written
		};

		struct ProfilerState
		{
			std::mutex Mutex; // guards every member that is not atomic
			// The current initialization's number, 0 while shut down. Generations are never reused, so a thread whose
			// cached ring has another number knows the ring is gone. Written under Mutex.
			std::atomic<uint64_t> ActiveGeneration{ 0 };
			std::atomic<std::chrono::steady_clock::rep> StartTicks{ 0 };
			uint64_t LastGeneration = 0;
			size_t ZonesPerThread = 0;
			uint32_t NextThreadIndex = 0;
			std::vector<Scope<ThreadRing>> Rings;        // registration order
			std::deque<ThreadRing*> RetiredRings;        // rings in Rings whose thread exited, oldest first
			std::vector<ProfileZone> GpuZones;           // a ring of ZonesPerThread zones
			uint64_t GpuZonesWritten = 0;                // GpuZones[n % ZonesPerThread] holds GPU zone n
			std::set<std::string, std::less<>> GpuNames; // interned, so GPU zone names stay valid until Shutdown
		};

		// The calling thread's cached ring and scope depth. The destructor runs when the thread exits and retires the ring,
		// so its zones stay collectable after the thread is gone.
		struct ThreadState
		{
			ThreadState() = default;
			~ThreadState();

			ThreadState(const ThreadState&) = delete;
			ThreadState& operator=(const ThreadState&) = delete;

			ThreadRing* Ring = nullptr; // valid while Generation is the active generation
			uint64_t Generation = 0;
			uint32_t Depth = 0; // open ProfileScopes on this thread
			std::string Name;   // from SetThreadName, applied to every ring this thread registers
		};

	}

	static thread_local ThreadState s_Thread;

	// Process-level state (Docs/Decisions/0003-m1-contract-decisions.md, decision 6). A function-local static, so it is
	// constructed on first use and outlives every thread's ThreadState.
	static ProfilerState& GetState()
	{
		static ProfilerState s_State;
		return s_State;
	}

	ThreadState::~ThreadState()
	{
		if (Ring == nullptr)
			return;

		ProfilerState& state = GetState();
		std::scoped_lock lock(state.Mutex);
		if (state.ActiveGeneration.load(std::memory_order_relaxed) != Generation)
			return; // Shutdown freed the ring already

		state.RetiredRings.push_back(Ring);
		Ring = nullptr;
		while (state.RetiredRings.size() > MaxRetiredRings)
		{
			const ThreadRing* oldest = state.RetiredRings.front();
			state.RetiredRings.pop_front();
			std::erase_if(state.Rings, [oldest](const Scope<ThreadRing>& ring)
			{
				return ring.get() == oldest;
			});
		}
	}

	namespace Utils {

		// The number of the current initialization, 0 while shut down.
		static uint64_t GetActiveGeneration()
		{
			return GetState().ActiveGeneration.load(std::memory_order_acquire);
		}

		// The calling thread's ring in `generation`, registering one on first use. nullptr when that generation is no
		// longer active.
		static ThreadRing* GetThreadRing(ProfilerState& state, uint64_t generation)
		{
			if (s_Thread.Generation == generation && s_Thread.Ring != nullptr)
				return s_Thread.Ring;

			std::scoped_lock lock(state.Mutex);
			if (state.ActiveGeneration.load(std::memory_order_relaxed) != generation)
				return nullptr;

			Scope<ThreadRing> ring = CreateScope<ThreadRing>(state.ZonesPerThread, state.NextThreadIndex++, s_Thread.Name);
			s_Thread.Ring = ring.get();
			s_Thread.Generation = generation;
			state.Rings.push_back(std::move(ring));
			return s_Thread.Ring;
		}

		// Records a zone that began in `generation`; it is discarded when that initialization is over (or never was).
		static void RecordZoneInGeneration(uint64_t generation, const char* name, uint64_t beginNs, uint64_t endNs, uint32_t depth)
		{
			if (generation == 0 || GetActiveGeneration() != generation)
				return;
			if (name == nullptr || beginNs > endNs)
				return;

			ThreadRing* ring = GetThreadRing(GetState(), generation);
			if (ring != nullptr)
				ring->Write(name, beginNs, endNs, depth);
		}

		// Appends the held GPU zones that ended at or after `sinceNs`, oldest first. Requires the profiler mutex.
		static void AppendGpuZones(const ProfilerState& state, uint64_t sinceNs, std::vector<ProfileZone>& zones)
		{
			const size_t count = state.GpuZones.size();
			const size_t oldest = count < state.ZonesPerThread ? 0 : static_cast<size_t>(state.GpuZonesWritten % count);
			for (size_t offset = 0; offset < count; ++offset)
			{
				const ProfileZone& zone = state.GpuZones[(oldest + offset) % count];
				if (zone.EndNs >= sinceNs)
					zones.push_back(zone);
			}
		}

		// Every held zone that ended at or after `sinceNs`, in the documented order. Requires the profiler mutex.
		static std::vector<ProfileZone> CollectZonesLocked(const ProfilerState& state, uint64_t sinceNs)
		{
			std::vector<ProfileZone> zones;
			if (state.ActiveGeneration.load(std::memory_order_relaxed) == 0)
				return zones;

			for (const Scope<ThreadRing>& ring : state.Rings)
				ring->AppendZones(sinceNs, zones);
			AppendGpuZones(state, sinceNs, zones);

			// Stable, so zones that tie on every key keep their recording order.
			std::stable_sort(zones.begin(), zones.end(), [](const ProfileZone& lhs, const ProfileZone& rhs)
			{
				return std::tie(lhs.BeginNs, lhs.ThreadIndex, lhs.Depth) < std::tie(rhs.BeginNs, rhs.ThreadIndex, rhs.Depth);
			});
			return zones;
		}

		// A JSON string literal for `text`, quotes included. Invalid UTF-8 (a thread or GPU pass name is caller data)
		// becomes U+FFFD instead of making the trace unloadable.
		static std::string ToJsonString(std::string_view text)
		{
			return Json(std::string(text)).dump(-1, ' ', false, Json::error_handler_t::replace);
		}

		// Nanoseconds as microseconds with three exact decimals, the unit of Chrome trace timestamps.
		static std::string FormatMicroseconds(uint64_t nanoseconds)
		{
			return std::format("{}.{:03}", nanoseconds / 1000, nanoseconds % 1000);
		}

	}

	void Profiler::Initialize(const ProfilerSpecification& specification)
	{
		ProfilerState& state = GetState();
		std::scoped_lock lock(state.Mutex);
		ENGINE_CORE_ASSERT(state.ActiveGeneration.load(std::memory_order_relaxed) == 0,
			"Profiler::Initialize called while the profiler is initialized");
		ENGINE_CORE_ASSERT(specification.ZonesPerThread >= 1, "ProfilerSpecification::ZonesPerThread must be at least 1");
		if (state.ActiveGeneration.load(std::memory_order_relaxed) != 0)
			return;

		state.ZonesPerThread = std::max<size_t>(specification.ZonesPerThread, 1);
		state.NextThreadIndex = 0;
		state.GpuZonesWritten = 0;
		state.StartTicks.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
		++state.LastGeneration;
		state.ActiveGeneration.store(state.LastGeneration, std::memory_order_release);
	}

	void Profiler::Shutdown()
	{
		ProfilerState& state = GetState();
		std::scoped_lock lock(state.Mutex);
		state.ActiveGeneration.store(0, std::memory_order_release);
		state.StartTicks.store(0, std::memory_order_relaxed);
		state.Rings.clear();
		state.RetiredRings.clear();
		std::vector<ProfileZone>().swap(state.GpuZones);
		state.GpuZonesWritten = 0;
		state.GpuNames.clear();
	}

	bool Profiler::IsInitialized()
	{
		return Utils::GetActiveGeneration() != 0;
	}

	void Profiler::SetThreadName(std::string_view name)
	{
		s_Thread.Name.assign(name);
		if (s_Thread.Ring == nullptr)
			return; // the next ring this thread registers takes the name

		ProfilerState& state = GetState();
		std::scoped_lock lock(state.Mutex);
		if (state.ActiveGeneration.load(std::memory_order_relaxed) == s_Thread.Generation)
			s_Thread.Ring->SetName(s_Thread.Name);
	}

	void Profiler::RecordZone(const char* name, uint64_t beginNs, uint64_t endNs, uint32_t depth)
	{
		ENGINE_CORE_ASSERT(name != nullptr, "Profiler::RecordZone needs a zone name");
		ENGINE_CORE_ASSERT(beginNs <= endNs, "Profiler zone '{}' ends ({} ns) before it begins ({} ns)", name != nullptr ? name : "",
			endNs, beginNs);
		Utils::RecordZoneInGeneration(Utils::GetActiveGeneration(), name, beginNs, endNs, depth);
	}

	void Profiler::SubmitGpuZones(std::span<const ProfileZone> zones)
	{
		ProfilerState& state = GetState();
		std::scoped_lock lock(state.Mutex);
		if (state.ActiveGeneration.load(std::memory_order_relaxed) == 0)
			return;

		for (const ProfileZone& zone : zones)
		{
			// GPU timestamps come from the driver: a zone that ends before it begins is dropped, never asserted.
			if (zone.BeginNs > zone.EndNs)
				continue;

			auto name = state.GpuNames.find(zone.Name);
			if (name == state.GpuNames.end())
				name = state.GpuNames.emplace(zone.Name).first;

			const ProfileZone stored{
				.Name = *name,
				.BeginNs = zone.BeginNs,
				.EndNs = zone.EndNs,
				.ThreadIndex = GpuThreadIndex,
				.Depth = zone.Depth,
			};
			if (state.GpuZones.size() < state.ZonesPerThread)
				state.GpuZones.push_back(stored);
			else
				state.GpuZones[static_cast<size_t>(state.GpuZonesWritten % state.ZonesPerThread)] = stored;
			++state.GpuZonesWritten;
		}
	}

	std::vector<ProfileZone> Profiler::CollectZones(uint64_t sinceNs)
	{
		ProfilerState& state = GetState();
		std::scoped_lock lock(state.Mutex);
		return Utils::CollectZonesLocked(state, sinceNs);
	}

	std::string Profiler::ExportChromeTrace(uint64_t sinceNs)
	{
		std::vector<ProfileZone> zones;
		std::vector<std::pair<uint32_t, std::string>> threadNames;
		{
			ProfilerState& state = GetState();
			std::scoped_lock lock(state.Mutex);
			zones = Utils::CollectZonesLocked(state, sinceNs);
			for (const Scope<ThreadRing>& ring : state.Rings)
			{
				if (!ring->GetName().empty())
					threadNames.emplace_back(ring->GetThreadIndex(), ring->GetName());
			}
		}
		const bool hasGpuZones = std::ranges::any_of(zones, [](const ProfileZone& zone)
		{
			return zone.ThreadIndex == GpuThreadIndex;
		});
		if (hasGpuZones)
			threadNames.emplace_back(GpuThreadIndex, "GPU");

		std::string trace = "{\"traceEvents\":[";
		bool isFirstEvent = true;
		const auto beginEvent = [&trace, &isFirstEvent]()
		{
			if (!isFirstEvent)
				trace += ',';
			isFirstEvent = false;
		};

		for (const auto& [threadIndex, name] : threadNames)
		{
			beginEvent();
			trace += std::format("{{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":{},\"tid\":{},\"args\":{{\"name\":{}}}}}",
				ChromeTraceProcessID, threadIndex, Utils::ToJsonString(name));
		}

		// Zone names repeat (every frame, every job), so each distinct name is escaped once.
		std::unordered_map<std::string_view, std::string> escapedNames;
		for (const ProfileZone& zone : zones)
		{
			auto escaped = escapedNames.find(zone.Name);
			if (escaped == escapedNames.end())
				escaped = escapedNames.emplace(zone.Name, Utils::ToJsonString(zone.Name)).first;

			beginEvent();
			trace += std::format("{{\"name\":{},\"cat\":\"{}\",\"ph\":\"X\",\"ts\":{},\"dur\":{},\"pid\":{},\"tid\":{}}}",
				escaped->second, zone.ThreadIndex == GpuThreadIndex ? "gpu" : "cpu", Utils::FormatMicroseconds(zone.BeginNs),
				Utils::FormatMicroseconds(zone.EndNs - zone.BeginNs), ChromeTraceProcessID, zone.ThreadIndex);
		}
		trace += "]}";
		return trace;
	}

	uint64_t Profiler::GetTimeNs()
	{
		const ProfilerState& state = GetState();
		if (Utils::GetActiveGeneration() == 0)
			return 0;

		using Ticks = std::chrono::steady_clock::duration;
		const Ticks elapsed = std::chrono::steady_clock::now().time_since_epoch() - Ticks(state.StartTicks.load(std::memory_order_relaxed));
		const int64_t nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
		return nanoseconds > 0 ? static_cast<uint64_t>(nanoseconds) : 0;
	}

	ProfileScope::ProfileScope(const char* name)
		: m_Name(name), m_Generation(Utils::GetActiveGeneration()), m_BeginNs(Profiler::GetTimeNs()), m_Depth(s_Thread.Depth++)
	{
	}

	ProfileScope::~ProfileScope()
	{
		const uint64_t endNs = Profiler::GetTimeNs();
		--s_Thread.Depth;
		Utils::RecordZoneInGeneration(m_Generation, m_Name, m_BeginNs, endNs, m_Depth);
	}

}
