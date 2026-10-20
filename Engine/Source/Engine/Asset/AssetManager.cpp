#include "EnginePCH.h"
#include "Engine/Asset/AssetManager.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/FontData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <tuple>

namespace Engine {

	namespace {

		// What identifies one recorded diagnostic, for replacing it and for logging it once: its code, asset and subject,
		// and its path when it concerns no asset (a .meta that cannot be read).
		using DiagnosticKey = std::tuple<std::string, AssetHandle, std::string, std::string>;

		// The part of the state a dry run saves and restores (AssetManager::SaveSharedState).
		struct SharedState
		{
			std::map<AssetHandle, uint64_t> Versions;
			std::vector<AssetDiagnostic> Recorded;
			std::vector<AssetDiagnostic> Scan;
			std::set<DiagnosticKey> Logged;
			// The uses of unknown handles past MaxAssetMissingDiagnostics, which the summary in Recorded counts (0: none).
			uint64_t SuppressedMissingUses = 0;
		};

	}

	namespace Utils {

		static DiagnosticKey MakeDiagnosticKey(const AssetDiagnostic& diagnostic)
		{
			return { diagnostic.Code, diagnostic.Asset, diagnostic.Subject, diagnostic.Asset.IsValid() ? std::string() : diagnostic.Path };
		}

		// MakeDiagnosticKey(left) == MakeDiagnosticKey(right), without copying: a manager may hold over a thousand
		// diagnostics (MaxAssetMissingDiagnostics), and every report looks for its own.
		static bool HasSameKey(const AssetDiagnostic& left, const AssetDiagnostic& right)
		{
			return left.Code == right.Code && left.Asset == right.Asset && left.Subject == right.Subject
				&& (left.Asset.IsValid() || left.Path == right.Path);
		}

		// Reference diagnostics report a use that failed, not the state of an asset (AssetManager.h), and the upload failure
		// is a property of this process's GPU: none of them gates play or export.
		static bool CountsAsAssetError(const AssetDiagnostic& diagnostic)
		{
			return diagnostic.Severity == DiagnosticSeverity::Error && diagnostic.Code != AssetMissingCode
				&& diagnostic.Code != AssetTypeMismatchCode && diagnostic.Code != AssetUploadFailedCode;
		}

		static bool DiagnosticLess(const AssetDiagnostic& left, const AssetDiagnostic& right)
		{
			return std::tie(left.Path, left.Code, left.Subject, left.Asset, left.Message)
				< std::tie(right.Path, right.Code, right.Subject, right.Asset, right.Message);
		}

		// A reference diagnostic of one unknown handle, which MaxAssetMissingDiagnostics bounds.
		static bool IsMissingHandleDiagnostic(const AssetDiagnostic& diagnostic)
		{
			return diagnostic.Code == AssetMissingCode && diagnostic.Asset.IsValid();
		}

		// The summary that counts the uses of unknown handles past MaxAssetMissingDiagnostics (AssetManager.h).
		static bool IsSuppressedMissingSummary(const AssetDiagnostic& diagnostic)
		{
			return diagnostic.Code == AssetMissingCode && !diagnostic.Asset.IsValid() && diagnostic.Path.empty()
				&& diagnostic.Subject == AssetMissingSuppressedSubject;
		}

		static std::string MakeSuppressedMissingMessage(uint64_t uses)
		{
			return std::format("{} more use{} of missing assets {} not recorded: diagnostics are kept for at most {} missing assets", uses,
				uses == 1 ? "" : "s", uses == 1 ? "was" : "were", MaxAssetMissingDiagnostics);
		}

	}

	struct AssetManager::State
	{
		SharedState Shared;
		// Shared.Recorded and Shared.Scan merged and sorted, the view GetDiagnostics returns: always what RebuildMerged makes
		// of them (AppendRecorded inserts in place, every other change rebuilds it).
		std::vector<AssetDiagnostic> Merged;
		std::map<AssetHandle, AssetRef<Asset>> ProceduralBuiltins;
		// The Font placeholder when the Default font cannot be loaded: an empty font, created once.
		AssetRef<FontData> EmptyFont;
		std::optional<SharedState> Saved;
		// SetLoadObserver's observer (empty: none). Not part of a dry run's saved state: the FeatureTestRunner owns it.
		LoadObserver Observer;

		void RebuildMerged()
		{
			Merged.clear();
			Merged.reserve(Shared.Recorded.size() + Shared.Scan.size());
			Merged.insert(Merged.end(), Shared.Recorded.begin(), Shared.Recorded.end());
			Merged.insert(Merged.end(), Shared.Scan.begin(), Shared.Scan.end());
			std::ranges::stable_sort(Merged, Utils::DiagnosticLess);
		}

		// Records a new diagnostic and inserts it into Merged where RebuildMerged would put it, without rebuilding: after
		// every diagnostic that sorts before or equal to it, except the scan's equal ones (stable_sort keeps the recorded
		// diagnostics, in recording order, before the scan's).
		void AppendRecorded(AssetDiagnostic diagnostic)
		{
			const auto equal = [&diagnostic](const AssetDiagnostic& other)
			{
				return !Utils::DiagnosticLess(other, diagnostic) && !Utils::DiagnosticLess(diagnostic, other);
			};
			const auto scanEqual = std::ranges::count_if(Shared.Scan, equal);
			const auto position = std::ranges::upper_bound(Merged, diagnostic, Utils::DiagnosticLess) - scanEqual;
			Merged.insert(position, diagnostic);
			Shared.Recorded.push_back(std::move(diagnostic));
		}

		// Removes the recorded diagnostics `remove` selects and allows them to be logged again. Returns whether any went.
		template<typename Predicate>
		bool RemoveRecorded(Predicate&& remove)
		{
			bool removed = false;
			for (auto iterator = Shared.Recorded.begin(); iterator != Shared.Recorded.end();)
			{
				if (remove(*iterator))
				{
					Shared.Logged.erase(Utils::MakeDiagnosticKey(*iterator));
					iterator = Shared.Recorded.erase(iterator);
					removed = true;
				}
				else
				{
					++iterator;
				}
			}
			return removed;
		}

		// Counts one use of an unknown handle that MaxAssetMissingDiagnostics keeps from being recorded. The first such use
		// records and logs the summary; later ones update its message in place, without a log line. The summary is the only
		// recorded diagnostic with its (Path, Code, Subject, Asset), so the new message keeps Merged sorted.
		void CountSuppressedMissingUse()
		{
			const std::string message = Utils::MakeSuppressedMissingMessage(++Shared.SuppressedMissingUses);
			const auto recorded = std::ranges::find_if(Shared.Recorded, Utils::IsSuppressedMissingSummary);
			if (recorded != Shared.Recorded.end())
			{
				recorded->Message = message;
				const auto merged = std::ranges::find_if(Merged, Utils::IsSuppressedMissingSummary);
				ENGINE_CORE_ASSERT(merged != Merged.end(), "the missing-asset summary is recorded but not merged");
				if (merged != Merged.end())
					merged->Message = message;
				return;
			}
			AssetDiagnostic summary{
				.Severity = DiagnosticSeverity::Error,
				.Code = std::string(AssetMissingCode),
				.Asset = {},
				.Path = {},
				.Message = message,
				.Hint = "restore or reassign the missing assets; project.validate reports every reference that is missing now",
				.Subject = std::string(AssetMissingSuppressedSubject),
				.AutoFixable = false,
			};
			Shared.Logged.insert(Utils::MakeDiagnosticKey(summary));
			ENGINE_CORE_ERROR("{}", AssetDiagnosticToString(summary));
			AppendRecorded(std::move(summary));
		}
	};

	std::string_view AssetStateToString(AssetState state)
	{
		switch (state)
		{
			case AssetState::Unloaded: return "Unloaded";
			case AssetState::Loading:  return "Loading";
			case AssetState::Loaded:   return "Loaded";
			case AssetState::Failed:   return "Failed";
		}
		ENGINE_CORE_ASSERT(false, "Unknown AssetState {}", std::to_underlying(state));
		return "Unloaded";
	}

	AssetManager::AssetManager()
		: m_State(CreateScope<State>())
	{
	}

	AssetManager::~AssetManager() = default;

	Result<uint64_t> AssetManager::GetCookedHash(AssetHandle handle)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, std::format("the cooked hash of asset {} (Docs/Decisions/0030-m19-contract.md decision 2, stream B)", handle)));
	}

	AssetRef<Asset> AssetManager::GetPlaceholder(AssetType type)
	{
		const AssetHandle handle = GetPlaceholderHandle(type);
		ENGINE_CORE_ASSERT(handle.IsValid(), "AssetType {} has no placeholder", AssetTypeToString(type));

		if (type == AssetType::Font)
		{
			// The Default font is a File built-in (Resources/Fonts), which can be unavailable (no engine:// mount, a failed
			// bake); its failure is reported once and an empty font stands in.
			Result<AssetRef<Asset>> font = Load(handle);
			if (font.has_value() && AssetCast<FontData>(*font) != nullptr)
				return *font;
			ReportDiagnostic({
				.Severity = DiagnosticSeverity::Error,
				.Code = std::string(font.has_value() ? AssetTypeMismatchCode : AssetImportFailedCode),
				.Asset = handle,
				.Path = GetReferencePath(handle),
				.Message = font.has_value() ? std::string("the Default font is not a font; text uses an empty font")
											: std::format("the Default font cannot be loaded; text uses an empty font: {}", font.error().ToString()),
				.Hint = "build the engine resources (Editor --headless --bake-engine-assets) or mount engine://",
				.Subject = {},
				.AutoFixable = false,
			});
			if (m_State->EmptyFont == nullptr)
				m_State->EmptyFont = CreateRef<FontData>();
			return m_State->EmptyFont;
		}

		Result<AssetRef<Asset>> builtin = GetProceduralBuiltin(handle);
		ENGINE_CORE_VERIFY(builtin.has_value(), "The procedural placeholder {} of {} cannot fail: {}", handle.ToString(), AssetTypeToString(type),
			builtin.has_value() ? std::string() : builtin.error().ToString());
		return *builtin;
	}

	uint64_t AssetManager::GetVersion(AssetHandle handle) const
	{
		// Built-ins never change: version 1 from their first use, whatever a dry run restored.
		if (m_State->ProceduralBuiltins.contains(handle))
			return 1;
		const auto found = m_State->Shared.Versions.find(handle);
		return found == m_State->Shared.Versions.end() ? 0 : found->second;
	}

	std::span<const AssetDiagnostic> AssetManager::GetDiagnostics() const
	{
		return m_State->Merged;
	}

	bool AssetManager::HasErrorDiagnostics() const
	{
		const auto isError = [](const AssetDiagnostic& diagnostic)
		{
			return diagnostic.Severity == DiagnosticSeverity::Error;
		};
		return std::ranges::any_of(m_State->Shared.Recorded, Utils::CountsAsAssetError) || std::ranges::any_of(m_State->Shared.Scan, isError);
	}

	void AssetManager::ReportDiagnostic(AssetDiagnostic diagnostic)
	{
		std::vector<AssetDiagnostic>& recorded = m_State->Shared.Recorded;
		const auto existing = std::ranges::find_if(recorded, [&diagnostic](const AssetDiagnostic& candidate)
		{
			return Utils::HasSameKey(candidate, diagnostic);
		});
		// Unknown handles are unbounded input (AssetManager.h): past the limit a new one is only counted.
		if (existing == recorded.end() && Utils::IsMissingHandleDiagnostic(diagnostic)
			&& static_cast<size_t>(std::ranges::count_if(recorded, Utils::IsMissingHandleDiagnostic)) >= MaxAssetMissingDiagnostics)
		{
			m_State->CountSuppressedMissingUse();
			return;
		}

		if (m_State->Shared.Logged.insert(Utils::MakeDiagnosticKey(diagnostic)).second)
		{
			if (diagnostic.Severity == DiagnosticSeverity::Error)
				ENGINE_CORE_ERROR("{}", AssetDiagnosticToString(diagnostic));
			else
				ENGINE_CORE_WARN("{}", AssetDiagnosticToString(diagnostic));
		}

		if (existing == recorded.end())
		{
			m_State->AppendRecorded(std::move(diagnostic));
			return;
		}
		if (*existing == diagnostic)
			return;
		*existing = std::move(diagnostic);
		m_State->RebuildMerged();
	}

	void AssetManager::BumpVersion(AssetHandle handle)
	{
		++m_State->Shared.Versions[handle];
		// The handle loads now: a reference to it is no longer missing.
		if (m_State->RemoveRecorded([handle](const AssetDiagnostic& diagnostic)
		{
			return diagnostic.Asset == handle && diagnostic.Code == AssetMissingCode;
		}))
		{
			m_State->RebuildMerged();
		}
	}

	void AssetManager::ClearDiagnostics(AssetHandle handle, std::span<const std::string_view> codes)
	{
		if (m_State->RemoveRecorded([handle, codes](const AssetDiagnostic& diagnostic)
		{
			return diagnostic.Asset == handle && std::ranges::find(codes, std::string_view(diagnostic.Code)) != codes.end();
		}))
		{
			m_State->RebuildMerged();
		}
	}

	void AssetManager::SetScanDiagnostics(std::vector<AssetDiagnostic> diagnostics)
	{
		m_State->Shared.Scan = std::move(diagnostics);
		// A registry scan ends the reference type mismatches recorded so far and the missing-handle summary (AssetManager.h):
		// a use that is still wrong records its diagnostic again.
		m_State->RemoveRecorded([](const AssetDiagnostic& diagnostic)
		{
			return diagnostic.Code == AssetTypeMismatchCode || Utils::IsSuppressedMissingSummary(diagnostic);
		});
		m_State->Shared.SuppressedMissingUses = 0;
		m_State->RebuildMerged();
	}

	Result<AssetRef<Asset>> AssetManager::GetProceduralBuiltin(AssetHandle handle)
	{
		const auto cached = m_State->ProceduralBuiltins.find(handle);
		if (cached != m_State->ProceduralBuiltins.end())
			return cached->second;
		ENGINE_TRY_ASSIGN(AssetRef<Asset> asset, CreateProceduralBuiltinAsset(handle));
		m_State->ProceduralBuiltins.emplace(handle, asset);
		return asset;
	}

	void AssetManager::SaveSharedState()
	{
		ENGINE_CORE_ASSERT(!m_State->Saved.has_value(), "SaveSharedState while a saved copy is kept");
		m_State->Saved = m_State->Shared;
	}

	void AssetManager::RestoreSharedState()
	{
		ENGINE_CORE_ASSERT(m_State->Saved.has_value(), "RestoreSharedState without a saved copy");
		if (!m_State->Saved.has_value())
			return;
		m_State->Shared = std::move(*m_State->Saved);
		m_State->Saved.reset();
		m_State->RebuildMerged();
	}

	void AssetManager::ReportUnavailable(AssetHandle handle, AssetType expectedType, const Error* error)
	{
		const std::string path = GetReferencePath(handle);
		const std::string placeholder(AssetTypeToString(expectedType));
		AssetDiagnostic diagnostic{
			.Severity = DiagnosticSeverity::Error,
			.Code = {},
			.Asset = handle,
			.Path = path,
			.Message = {},
			.Hint = {},
			.Subject = {},
			.AutoFixable = false,
		};
		if (error == nullptr || error->GetCode() == ErrorCode::InvalidArgument)
		{
			// Another type, or a handle that cannot be loaded at all (a dependency file, §6.4).
			const AssetType actual = GetAssetType(handle);
			diagnostic.Code = std::string(AssetTypeMismatchCode);
			diagnostic.Message = actual == AssetType::None
				? std::format("asset {} cannot be used as a {}; the {} placeholder stands in", handle.ToString(), placeholder, placeholder)
				: std::format("asset {} is a {}, not a {}; the {} placeholder stands in", handle.ToString(), AssetTypeToString(actual), placeholder,
					  placeholder);
			diagnostic.Hint = std::format("assign an asset of type {}", placeholder);
		}
		else if (error->GetCode() == ErrorCode::NotFound)
		{
			diagnostic.Code = std::string(AssetMissingCode);
			diagnostic.Message = std::format("asset {} is missing; the {} placeholder stands in", handle.ToString(), placeholder);
			diagnostic.Hint = "restore the asset, or assign another one";
		}
		else
		{
			diagnostic.Code = std::string(AssetImportFailedCode);
			diagnostic.Message = std::format("asset {} failed to load; the {} placeholder stands in: {}", handle.ToString(), placeholder,
				Error(*error).WithHint(std::string()).ToString()); // the hint is the diagnostic's own member
			diagnostic.Hint = error->GetHint();
		}
		ReportDiagnostic(std::move(diagnostic));
	}

	void AssetManager::SetLoadObserver(LoadObserver observer)
	{
		m_State->Observer = std::move(observer);
	}

	bool AssetManager::HasLoadObserver() const
	{
		return static_cast<bool>(m_State->Observer);
	}

	void AssetManager::NotifyLoaded(AssetHandle handle) const
	{
		if (m_State->Observer)
			m_State->Observer(handle);
	}

}
