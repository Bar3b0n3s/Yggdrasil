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
		};

	}

	namespace Utils {

		static DiagnosticKey MakeDiagnosticKey(const AssetDiagnostic& diagnostic)
		{
			return { diagnostic.Code, diagnostic.Asset, diagnostic.Subject, diagnostic.Asset.IsValid() ? std::string() : diagnostic.Path };
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

	}

	struct AssetManager::State
	{
		SharedState Shared;
		// Shared.Recorded and Shared.Scan merged and sorted, the view GetDiagnostics returns; rebuilt after every change.
		std::vector<AssetDiagnostic> Merged;
		std::map<AssetHandle, AssetRef<Asset>> ProceduralBuiltins;
		// The Font placeholder when the Default font cannot be loaded: an empty font, created once.
		AssetRef<FontData> EmptyFont;
		std::optional<SharedState> Saved;

		void RebuildMerged()
		{
			Merged.clear();
			Merged.reserve(Shared.Recorded.size() + Shared.Scan.size());
			Merged.insert(Merged.end(), Shared.Recorded.begin(), Shared.Recorded.end());
			Merged.insert(Merged.end(), Shared.Scan.begin(), Shared.Scan.end());
			std::ranges::stable_sort(Merged, Utils::DiagnosticLess);
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
		const DiagnosticKey key = Utils::MakeDiagnosticKey(diagnostic);
		if (m_State->Shared.Logged.insert(key).second)
		{
			if (diagnostic.Severity == DiagnosticSeverity::Error)
				ENGINE_CORE_ERROR("{}", AssetDiagnosticToString(diagnostic));
			else
				ENGINE_CORE_WARN("{}", AssetDiagnosticToString(diagnostic));
		}

		std::vector<AssetDiagnostic>& recorded = m_State->Shared.Recorded;
		const auto existing = std::ranges::find_if(recorded, [&key](const AssetDiagnostic& candidate)
		{
			return Utils::MakeDiagnosticKey(candidate) == key;
		});
		if (existing != recorded.end())
		{
			if (*existing == diagnostic)
				return;
			*existing = std::move(diagnostic);
		}
		else
		{
			recorded.push_back(std::move(diagnostic));
		}
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
		// A registry scan ends the reference type mismatches recorded so far (AssetManager.h): a use that is still wrong
		// records its diagnostic again.
		m_State->RemoveRecorded([](const AssetDiagnostic& diagnostic)
		{
			return diagnostic.Code == AssetTypeMismatchCode;
		});
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

}
