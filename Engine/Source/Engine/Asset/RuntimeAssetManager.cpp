#include "EnginePCH.h"
#include "Engine/Asset/RuntimeAssetManager.h"

#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/AssetReference.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Jobs/MainThreadQueue.h"
#include "Engine/Core/Log.h"

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		// One cooked-asset entry this manager serves. The entry lives in its reader, which the manager keeps alive.
		struct ServedEntry
		{
			Ref<const PakReader> Pak;
			const PakEntry* Entry = nullptr;
			AssetType Type = AssetType::None;
		};

		// What the manager knows about one handle it was asked for.
		struct LoadSlot
		{
			AssetState State = AssetState::Unloaded;
			AssetRef<Asset> Loaded;       // the published asset while State is Loaded
			std::optional<Error> Failure; // the recorded error while State is Failed
		};

		// Lets publications posted to the main-thread queue find the manager, or learn that it is gone. Main thread only.
		struct PublishTarget
		{
			RuntimeAssetManager* Manager = nullptr;
		};

	}

	struct RuntimeAssetManager::State
	{
		RuntimeAssetManagerSpecification Specification;
		std::vector<Ref<const PakReader>> Paks; // in the order they were added
		std::map<AssetHandle, ServedEntry> Entries;
		std::map<std::string, AssetHandle, std::less<>> Paths; // the entries' readable references
		std::map<AssetHandle, LoadSlot> Slots;
		Ref<PublishTarget> Target;
		size_t PendingPublications = 0; // LoadAsync results posted to the main thread and not yet published
	};

	namespace Utils {

		// The procedural built-in entry of `handle`, or nullptr.
		static const BuiltinAssetEntry* FindProceduralBuiltin(AssetHandle handle)
		{
			const std::span<const BuiltinAssetEntry> entries = GetProceduralBuiltinEntries();
			const auto found = std::ranges::lower_bound(entries, handle, {}, &BuiltinAssetEntry::Handle);
			return found != entries.end() && found->Handle == handle ? &*found : nullptr;
		}

		// Reads, verifies and decodes one pak entry. Pure apart from the reader's verified-entry memo: safe on any job.
		static Result<AssetRef<Asset>> LoadPakEntry(const ServedEntry& served, const AssetLoaderRegistry& loaders, const TypeRegistry* registry,
			AssetHandle handle)
		{
			const std::string context = std::format("while loading asset {} ('{}') from the pak '{}'", handle, served.Entry->Path,
				served.Pak->GetName());
			ENGINE_TRY_ASSIGN(const Buffer bytes, WithContext(served.Pak->ReadEntry(*served.Entry), context));
			ENGINE_TRY_ASSIGN(AssetRef<Asset> asset, WithContext(loaders.Load(bytes, AssetLoadContext{ .Registry = registry, .Handle = handle }), context));
			if (asset == nullptr || asset->GetAssetType() != served.Type)
			{
				return std::unexpected(Error(ErrorCode::Validation, std::format("the entry holds a {}, its TOC names a {}", asset == nullptr ? std::string_view("None") : AssetTypeToString(asset->GetAssetType()), AssetTypeToString(served.Type)))
						.WithContext(context));
			}
			return asset;
		}

		// Records the outcome of loading `handle` (§7.2): the first success publishes version 1 (`bumpVersion`), the first
		// failure is remembered and reported once as an asset diagnostic. An outcome for a handle that is already loaded or
		// failed (an earlier Load or LoadAsync published first) changes nothing.
		template<typename ManagerState, typename BumpVersion>
		static void PublishLoad(ManagerState& state, AssetManager& manager, AssetHandle handle, const Result<AssetRef<Asset>>& result,
			BumpVersion&& bumpVersion)
		{
			LoadSlot& slot = state.Slots[handle];
			if (slot.State == AssetState::Loaded || slot.State == AssetState::Failed)
				return;
			if (result.has_value())
			{
				slot.State = AssetState::Loaded;
				slot.Loaded = *result;
				bumpVersion(handle);
				return;
			}
			slot.State = AssetState::Failed;
			slot.Failure = result.error();
			const auto served = state.Entries.find(handle);
			manager.ReportDiagnostic(AssetDiagnostic{
				.Severity = DiagnosticSeverity::Error,
				.Code = std::string(AssetImportFailedCode),
				.Asset = handle,
				.Path = served != state.Entries.end() ? served->second.Entry->Path : std::string(),
				.Message = result.error().ToString(),
				.Hint = {},
				.Subject = {},
				.AutoFixable = false,
			});
		}

	}

	RuntimeAssetManager::RuntimeAssetManager(const RuntimeAssetManagerSpecification& specification)
		: m_State(CreateScope<State>())
	{
		ENGINE_CORE_ASSERT(specification.Jobs != nullptr && specification.MainThread != nullptr && specification.Registry != nullptr
				&& specification.Loaders != nullptr,
			"RuntimeAssetManager needs its job system, main-thread queue, type registry and loaders");
		m_State->Specification = specification;
		m_State->Target = CreateRef<PublishTarget>(PublishTarget{ .Manager = this });
	}

	RuntimeAssetManager::~RuntimeAssetManager()
	{
		WaitIdle();
		// A publication that is still queued (only after a cancelled job) finds no manager.
		m_State->Target->Manager = nullptr;
	}

	Status RuntimeAssetManager::AddPak(Ref<const PakReader> pak)
	{
		ENGINE_CORE_ASSERT(pak != nullptr, "RuntimeAssetManager::AddPak needs a pak");
		State& state = *m_State;
		// Check everything first, so a refused pak adds nothing.
		for (const PakEntry& entry : pak->GetEntries())
		{
			if (entry.Type == PakFileEntryType)
				continue;
			if (const auto served = state.Entries.find(entry.Handle); served != state.Entries.end())
			{
				return MakeError(ErrorCode::AlreadyExists, "asset {} ('{}') of the pak '{}' is already served by the pak '{}'", entry.Handle,
					entry.Path, pak->GetName(), served->second.Pak->GetName());
			}
			if (const auto path = state.Paths.find(entry.Path); path != state.Paths.end())
			{
				// Every served path belongs to a served entry.
				return MakeError(ErrorCode::AlreadyExists, "the path '{}' of the pak '{}' is already served by the pak '{}'", entry.Path,
					pak->GetName(), state.Entries.find(path->second)->second.Pak->GetName());
			}
		}
		for (const PakEntry& entry : pak->GetEntries())
		{
			if (entry.Type == PakFileEntryType)
				continue;
			// ParsePakToc admits only AssetType names other than None for cooked assets.
			const AssetType type = AssetTypeFromString(entry.Type).value_or(AssetType::None);
			state.Entries.emplace(entry.Handle, ServedEntry{ .Pak = pak, .Entry = &entry, .Type = type });
			state.Paths.emplace(entry.Path, entry.Handle);
		}
		state.Paks.push_back(std::move(pak));
		return {};
	}

	Result<AssetRef<Asset>> RuntimeAssetManager::Load(AssetHandle handle)
	{
		State& state = *m_State;
		if (!handle.IsValid())
			return MakeError(ErrorCode::InvalidArgument, "cannot load the null asset handle");

		if (const BuiltinAssetEntry* builtin = Utils::FindProceduralBuiltin(handle))
		{
			ENGINE_TRY_ASSIGN(AssetRef<Asset> asset,
				WithContext(GetProceduralBuiltin(handle), std::format("while loading asset {} ('{}')", handle, builtin->Path)));
			LoadSlot& slot = state.Slots[handle];
			slot.State = AssetState::Loaded;
			slot.Loaded = asset;
			return asset;
		}

		const auto served = state.Entries.find(handle);
		if (served == state.Entries.end())
			return MakeError(ErrorCode::NotFound, "unknown asset {}: no pak added to the runtime holds it", handle);

		if (const auto existing = state.Slots.find(handle); existing != state.Slots.end())
		{
			if (existing->second.State == AssetState::Loaded)
				return existing->second.Loaded;
			if (existing->second.State == AssetState::Failed)
				return std::unexpected(*existing->second.Failure);
		}

		// Unloaded, or Loading through LoadAsync: decode here; the asynchronous result is ignored when it arrives.
		const Result<AssetRef<Asset>> loaded = Utils::LoadPakEntry(served->second, *state.Specification.Loaders, state.Specification.Registry, handle);
		Utils::PublishLoad(state, *this, handle, loaded, [this](AssetHandle published)
		{
			BumpVersion(published);
		});
		return loaded;
	}

	JobHandle<AssetRef<Asset>> RuntimeAssetManager::LoadAsync(AssetHandle handle)
	{
		State& state = *m_State;
		JobSystem& jobs = *state.Specification.Jobs;
		const auto served = handle.IsValid() ? state.Entries.find(handle) : state.Entries.end();
		const auto slot = state.Slots.find(handle);
		const bool settled = slot != state.Slots.end() && (slot->second.State == AssetState::Loaded || slot->second.State == AssetState::Failed);
		if (served == state.Entries.end() || settled || Utils::FindProceduralBuiltin(handle) != nullptr)
		{
			// Nothing to decode (an invalid, unknown, built-in or settled handle): the job completes with Load's result.
			Result<AssetRef<Asset>> result = Load(handle);
			return jobs.Submit([result = std::move(result)]() -> Result<AssetRef<Asset>>
			{
				return result;
			});
		}

		if (slot == state.Slots.end() || slot->second.State == AssetState::Unloaded)
			state.Slots[handle].State = AssetState::Loading;
		++state.PendingPublications;
		const ServedEntry entry = served->second;
		const AssetLoaderRegistry* loaders = state.Specification.Loaders;
		const TypeRegistry* registry = state.Specification.Registry;
		MainThreadQueue* queue = state.Specification.MainThread;
		Ref<PublishTarget> target = state.Target;
		return jobs.Submit([entry, loaders, registry, queue, target, handle]() -> Result<AssetRef<Asset>>
		{
			Result<AssetRef<Asset>> result = Utils::LoadPakEntry(entry, *loaders, registry, handle);
			queue->Post([target, handle, published = result]()
			{
				RuntimeAssetManager* manager = target->Manager;
				if (manager == nullptr)
					return;
				State& owner = *manager->m_State;
				--owner.PendingPublications;
				Utils::PublishLoad(owner, *manager, handle, published, [manager](AssetHandle bumped)
				{
					manager->BumpVersion(bumped);
				});
			});
			return result;
		});
	}

	AssetState RuntimeAssetManager::GetState(AssetHandle handle) const
	{
		const auto slot = m_State->Slots.find(handle);
		return slot != m_State->Slots.end() ? slot->second.State : AssetState::Unloaded;
	}

	const AssetMetadata* RuntimeAssetManager::GetMetadata(AssetHandle /*handle*/) const
	{
		return nullptr;
	}

	AssetType RuntimeAssetManager::GetAssetType(AssetHandle handle) const
	{
		if (const BuiltinAssetEntry* builtin = Utils::FindProceduralBuiltin(handle))
			return builtin->Type;
		const auto served = m_State->Entries.find(handle);
		return served != m_State->Entries.end() ? served->second.Type : AssetType::None;
	}

	std::optional<AssetHandle> RuntimeAssetManager::Resolve(std::string_view reference) const
	{
		const Result<AssetReference> parsed = ParseAssetReference(reference);
		if (!parsed.has_value())
			return std::nullopt;
		if (parsed->Kind == AssetReferenceKind::Handle)
		{
			if (Utils::FindProceduralBuiltin(parsed->Handle) != nullptr || m_State->Entries.contains(parsed->Handle))
				return parsed->Handle;
			return std::nullopt;
		}

		const std::string canonical = FormatAssetReference(*parsed);
		if (const auto path = m_State->Paths.find(canonical); path != m_State->Paths.end())
			return path->second;
		const std::span<const BuiltinAssetEntry> builtins = GetProceduralBuiltinEntries();
		const auto builtin = std::ranges::find(builtins, canonical, &BuiltinAssetEntry::Path);
		if (builtin != builtins.end())
			return builtin->Handle;
		return std::nullopt;
	}

	std::string RuntimeAssetManager::GetReferencePath(AssetHandle handle) const
	{
		if (const BuiltinAssetEntry* builtin = Utils::FindProceduralBuiltin(handle))
			return builtin->Path;
		const auto served = m_State->Entries.find(handle);
		return served != m_State->Entries.end() ? served->second.Entry->Path : std::string();
	}

	void RuntimeAssetManager::WaitIdle()
	{
		State& state = *m_State;
		while (state.PendingPublications > 0)
		{
			state.Specification.Jobs->WaitIdle();
			const uint32_t ran = state.Specification.MainThread->Drain();
			// Every load job posts its publication before it completes; a job cancelled by a job system that is shutting down
			// posts nothing, and waiting for it would never end.
			if (ran == 0 && state.PendingPublications > 0)
			{
				ENGINE_CORE_WARN("Runtime asset manager: {} asset load(s) never completed (their jobs were cancelled)", state.PendingPublications);
				state.PendingPublications = 0;
			}
		}
	}

}
