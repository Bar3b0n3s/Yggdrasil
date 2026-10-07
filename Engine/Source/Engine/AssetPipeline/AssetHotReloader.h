#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UniqueFunction.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Platform/PollingFileWatcher.h"

#include <cstdint>
#include <map>
#include <span>
#include <vector>

// Hot reload (Architecture §7.5): a polling watcher (every 500 ms on a job: size + mtime, confirmed by content hash, 200 ms
// debounce) over project://Assets, generation-checked reimports, and the deferral of reloads while a deterministic session
// runs. The EditorAssetManager owns one per open project and drives it from EditorContext::Update once per frame; tests drive
// the same object through a MemoryMount and chosen times.

namespace Engine {

	class JobSystem;
	class VirtualFileSystem;

	struct AssetHotReloaderSpecification
	{
		VfsPath Root{};                   // the watched directory, "project://Assets"
		double PollIntervalSeconds = 0.5; // §7.5: every 500 ms
		double DebounceSeconds = 0.2;     // §7.5: 200 ms (PollingFileWatcherSpecification::DebounceSeconds)
	};

	// One requested reimport (§7.5 race rule 2 "newest wins"): import jobs carry (handle, contentHash, generation), and the
	// main thread drops a completion whose generation is older than the newest one requested for that handle.
	struct AssetReimportTicket
	{
		AssetHandle Handle{};
		uint64_t ContentHash = 0; // XXH64 of the source the job imports
		uint64_t Generation = 0;  // 1, 2, 3 ... per handle

		bool operator==(const AssetReimportTicket&) const = default;
	};

	// Main thread only, except the poll job it runs itself. Not copyable.
	class AssetHotReloader
	{
	public:
		using ChangeListener = UniqueFunction<void(std::span<const FileChange> changes)>;

		// `vfs` and `jobs` are documented back-references that outlive the reloader.
		AssetHotReloader(const VirtualFileSystem& vfs, JobSystem& jobs, AssetHotReloaderSpecification specification);
		// Waits for a poll job still running (its result is dropped).
		~AssetHotReloader();

		AssetHotReloader(const AssetHotReloader&) = delete;
		AssetHotReloader& operator=(const AssetHotReloader&) = delete;

		// Takes the watcher's baseline (PollingFileWatcher::Start) and starts polling. Errors: those of Start.
		[[nodiscard]] Status Start();
		// Stops polling (a running poll's result is dropped); the generations are kept.
		void Stop();
		[[nodiscard]] bool IsRunning() const;

		// The watcher, for AssetWriter::SetWatcher (no echo, race rule 1).
		[[nodiscard]] PollingFileWatcher& GetWatcher();

		// Receives the changes of each poll on the main thread, sorted by path; while deferred they are held instead.
		void SetChangeListener(ChangeListener listener);

		// Once per frame with a monotonic time in seconds (EditorContext::Update; tests pass chosen values): when the poll
		// interval has passed since the last poll and none is running, submits a poll job (PollingFileWatcher::Poll at
		// `nowSeconds`) whose changes reach the listener through the MainThreadQueue. A failed poll is logged once at Warn per
		// error message and retried at the next interval.
		void Update(double nowSeconds);

		// Starts a reimport of `handle` and returns its ticket, whose generation is newer than every earlier ticket of that
		// handle.
		[[nodiscard]] AssetReimportTicket BeginReimport(AssetHandle handle, uint64_t contentHash);
		// True when `ticket` is the newest requested for its handle: its completion may be published; an older one is dropped
		// ("HotReload: a stale job completion is dropped").
		[[nodiscard]] bool IsCurrent(const AssetReimportTicket& ticket) const;

		// Race rule 4 "deterministic sessions are frozen": while lockstep, a replay, a recording or a test run is active (M7,
		// M13), changes are held instead of delivered; SetDeferred(false) delivers the held changes, merged per path, at once.
		void SetDeferred(bool deferred);
		[[nodiscard]] bool IsDeferred() const;
		// The held changes, sorted by path (empty when not deferred).
		[[nodiscard]] std::vector<FileChange> GetDeferredChanges() const;
	private:
		// The watcher, the job state, the listener, the generations and the held changes (AssetHotReloader.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
