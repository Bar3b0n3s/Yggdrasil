#include "EnginePCH.h"
#include "Engine/AssetPipeline/AssetHotReloader.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Log.h"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace Engine {

	namespace Utils {

		// The net change of a held change `earlier` followed by a newer change `later` of the same path (race rule 4: held
		// changes are delivered merged per path), or nullopt when they cancel out. The watcher reports each change relative to
		// the state it reported before, so the pairs below are the only ones that can occur; others keep the newer change.
		static std::optional<FileChangeKind> MergeFileChanges(FileChangeKind earlier, FileChangeKind later)
		{
			if (earlier == FileChangeKind::Created && later == FileChangeKind::Deleted)
				return std::nullopt; // never existed as far as the listener knows
			if (earlier == FileChangeKind::Created)
				return FileChangeKind::Created; // created, then modified: still new to the listener
			if (earlier == FileChangeKind::Deleted && later == FileChangeKind::Created)
				return FileChangeKind::Modified; // deleted and recreated: the listener knew the file, its content changed
			return later;
		}

	}

	struct AssetHotReloader::State
	{
		// What the reloader shares with its poll job and the job's main-thread continuation, which may outlive the reloader
		// in the MainThreadQueue.
		struct PollShared
		{
			std::mutex Mutex;                 // guards IsJobRunning
			std::condition_variable Finished; // signalled when the poll job is done with the watcher
			bool IsJobRunning = false;
			// The reloader's state; set to null by the reloader's destructor, after which a queued continuation does nothing.
			// Main thread only (the destructor and the continuation both run there).
			State* Owner = nullptr;
		};

		State(const VirtualFileSystem& vfs, JobSystem& jobs, const AssetHotReloaderSpecification& specification)
			: Jobs(&jobs), Specification(specification), Watcher(vfs, { .Root = specification.Root, .DebounceSeconds = specification.DebounceSeconds })
		{
		}

		JobSystem* Jobs = nullptr; // documented back-reference
		AssetHotReloaderSpecification Specification;
		PollingFileWatcher Watcher;
		Ref<PollShared> PollContext = CreateRef<PollShared>();
		ChangeListener Listener;
		bool IsRunning = false;
		bool HasPolled = false;       // a poll was submitted since Start
		double LastPollSeconds = 0.0; // when the last poll was submitted
		// Advanced by Start and Stop: a continuation of a poll submitted under another epoch is dropped.
		uint64_t PollEpoch = 0;
		std::set<std::string> LoggedPollErrors; // each failed poll's message is logged once
		bool IsDeferred = false;
		std::map<VfsPath, FileChangeKind> HeldChanges; // while deferred, merged per path

		mutable std::mutex GenerationMutex;            // guards Generations (tickets may be checked from import jobs)
		std::map<AssetHandle, uint64_t> Generations{}; // the newest generation requested per handle

		// True while the poll job uses the watcher.
		[[nodiscard]] bool IsPollRunning() const
		{
			std::scoped_lock lock(PollContext->Mutex);
			return PollContext->IsJobRunning;
		}

		// Blocks until the poll job (if any) no longer uses the watcher. Its continuation may still be queued.
		void WaitForPoll() const
		{
			std::unique_lock lock(PollContext->Mutex);
			PollContext->Finished.wait(lock, [context = PollContext.get()]()
			{
				return !context->IsJobRunning;
			});
		}

		// The changes of one poll, on the main thread: held while deferred, else delivered to the listener.
		void Deliver(std::vector<FileChange> changes)
		{
			if (changes.empty())
				return;
			if (IsDeferred)
			{
				for (const FileChange& change : changes)
				{
					const auto held = HeldChanges.find(change.Path);
					if (held == HeldChanges.end())
					{
						HeldChanges.emplace(change.Path, change.Kind);
						continue;
					}
					if (const std::optional<FileChangeKind> merged = Utils::MergeFileChanges(held->second, change.Kind))
						held->second = *merged;
					else
						HeldChanges.erase(held);
				}
				return;
			}
			if (Listener)
				Listener(changes);
		}

		// A poll's outcome, on the main thread.
		void CompletePoll(Result<std::vector<FileChange>> changes)
		{
			if (!changes)
			{
				const std::string message = changes.error().ToString();
				if (LoggedPollErrors.insert(message).second)
					ENGINE_CORE_WARN("Hot reload could not poll '{}' (retrying every {} s): {}", Specification.Root.ToString(), Specification.PollIntervalSeconds, message);
				return;
			}
			Deliver(std::move(*changes));
		}
	};

	AssetHotReloader::AssetHotReloader(const VirtualFileSystem& vfs, JobSystem& jobs, AssetHotReloaderSpecification specification)
		: m_State(CreateScope<State>(vfs, jobs, specification))
	{
		m_State->PollContext->Owner = m_State.get();
	}

	AssetHotReloader::~AssetHotReloader()
	{
		// A queued continuation must find no reloader; a running poll must be done with the watcher before it goes.
		m_State->PollContext->Owner = nullptr;
		m_State->WaitForPoll();
	}

	Status AssetHotReloader::Start()
	{
		State& state = *m_State;
		state.WaitForPoll();
		ENGINE_TRY(state.Watcher.Start());
		++state.PollEpoch;
		state.IsRunning = true;
		state.HasPolled = false;
		state.LoggedPollErrors.clear();
		return {};
	}

	void AssetHotReloader::Stop()
	{
		State& state = *m_State;
		if (!state.IsRunning)
			return;
		state.WaitForPoll();
		++state.PollEpoch;
		state.IsRunning = false;
	}

	bool AssetHotReloader::IsRunning() const
	{
		return m_State->IsRunning;
	}

	PollingFileWatcher& AssetHotReloader::GetWatcher()
	{
		return m_State->Watcher;
	}

	void AssetHotReloader::SetChangeListener(ChangeListener listener)
	{
		m_State->Listener = std::move(listener);
	}

	void AssetHotReloader::Update(double nowSeconds)
	{
		State& state = *m_State;
		if (!state.IsRunning || state.IsPollRunning())
			return;
		if (state.HasPolled && nowSeconds - state.LastPollSeconds < state.Specification.PollIntervalSeconds)
			return;
		state.HasPolled = true;
		state.LastPollSeconds = nowSeconds;

		Ref<State::PollShared> context = state.PollContext;
		{
			std::scoped_lock lock(context->Mutex);
			context->IsJobRunning = true;
		}
		PollingFileWatcher* watcher = &state.Watcher;
		JobHandle<std::vector<FileChange>> poll = state.Jobs->Submit([context, watcher, nowSeconds]() -> Result<std::vector<FileChange>>
		{
			Result<std::vector<FileChange>> changes = watcher->Poll(nowSeconds);
			{
				std::scoped_lock lock(context->Mutex);
				context->IsJobRunning = false;
			}
			context->Finished.notify_all();
			return changes;
		});
		const uint64_t epoch = state.PollEpoch;
		state.Jobs->ContinueOnMainThread(poll, [context, epoch](Result<std::vector<FileChange>> changes)
		{
			State* owner = context->Owner;
			if (owner == nullptr || owner->PollEpoch != epoch)
				return; // the reloader is gone, or was stopped or restarted: this poll's result is dropped
			owner->CompletePoll(std::move(changes));
		});
	}

	AssetReimportTicket AssetHotReloader::BeginReimport(AssetHandle handle, uint64_t contentHash)
	{
		std::scoped_lock lock(m_State->GenerationMutex);
		const uint64_t generation = ++m_State->Generations[handle];
		return AssetReimportTicket{ .Handle = handle, .ContentHash = contentHash, .Generation = generation };
	}

	bool AssetHotReloader::IsCurrent(const AssetReimportTicket& ticket) const
	{
		std::scoped_lock lock(m_State->GenerationMutex);
		const auto found = m_State->Generations.find(ticket.Handle);
		return found != m_State->Generations.end() && found->second == ticket.Generation;
	}

	void AssetHotReloader::SetDeferred(bool deferred)
	{
		State& state = *m_State;
		if (state.IsDeferred == deferred)
			return;
		state.IsDeferred = deferred;
		if (deferred || state.HeldChanges.empty())
			return;
		std::vector<FileChange> held;
		held.reserve(state.HeldChanges.size());
		for (const auto& [path, kind] : state.HeldChanges)
			held.push_back(FileChange{ .Path = path, .Kind = kind });
		state.HeldChanges.clear();
		state.Deliver(std::move(held));
	}

	bool AssetHotReloader::IsDeferred() const
	{
		return m_State->IsDeferred;
	}

	std::vector<FileChange> AssetHotReloader::GetDeferredChanges() const
	{
		std::vector<FileChange> held;
		held.reserve(m_State->HeldChanges.size());
		for (const auto& [path, kind] : m_State->HeldChanges)
			held.push_back(FileChange{ .Path = path, .Kind = kind });
		return held;
	}

}
