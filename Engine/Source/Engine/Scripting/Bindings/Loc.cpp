#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/StringTableData.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"

#include <format>
#include <string>
#include <string_view>
#include <vector>

// The Loc module (M22, C22.1, R-UI-L10N; Docs/Decisions/0033-m22-contract.md decision 11): localized strings from string
// tables (Asset/StringTableData.h) in the VM's locale (ScriptEngine::GetLocale: the session's locale, which it starts in and
// Loc.SetLocale changes, M38, Docs/Decisions/0055-m38-runtime-locale.md decisions 1 and 2). Every lookup is byte-wise and
// deterministic (FindLocalizedString), so a table reads the same strings in every configuration.

namespace Engine {

	namespace Utils {

		// A string table argument: the loaded table and its reference path (named in errors).
		struct CheckedStringTable
		{
			AssetRef<StringTableData> Table{};
			std::string Path{};
		};

		// The string table `handle` names, loaded as host time (Detail::LoadAssetAsHost: an uncached import counts no callback
		// time). Raises a located error naming the asset when it is not a registered string table or fails to load: there is no
		// placeholder table.
		static CheckedStringTable LoadStringTable(ScriptCall& call, AssetHandle handle)
		{
			AssetManager* assets = call.Engine->GetHost().GetAssets();
			if (assets == nullptr)
			{
				Lua::RaiseError(call, "asset service is unavailable");
				return {};
			}
			const AssetType type = assets->GetAssetType(handle);
			const std::string path = type == AssetType::None ? handle.ToString() : assets->GetReferencePath(handle);
			if (type == AssetType::None)
			{
				Lua::RaiseError(call, Error(ErrorCode::NotFound, std::format("no asset '{}' is registered, so it is not a string table", path)).WithHint("pass the AssetRef Assets.Load returns for a .stringtable file"));
				return {};
			}
			if (type != AssetType::StringTable)
			{
				Lua::RaiseError(call, Error(ErrorCode::InvalidArgument, std::format("'{}' is a {}, not a StringTable", path, AssetTypeToString(type))).WithHint("pass the AssetRef Assets.Load returns for a .stringtable file"));
				return {};
			}
			const Result<AssetRef<Asset>> loaded = Detail::LoadAssetAsHost(call, *assets, handle);
			if (!loaded.has_value())
			{
				Lua::RaiseError(call, Error(loaded.error()).WithContext(std::format("loading the string table '{}'", path)));
				return {};
			}
			AssetRef<StringTableData> table = AssetCast<StringTableData>(*loaded);
			if (table == nullptr)
			{
				Lua::RaiseError(call, std::format("the string table '{}' did not load as a string table", path));
				return {};
			}
			return CheckedStringTable{ .Table = std::move(table), .Path = path };
		}

		// The keys of `table` close to `key` (FuzzySuggest), as a "did you mean" hint. Only keys whose length is within the
		// suggestion distance are compared, since the edit distance is at least the length difference.
		static std::string SuggestStringTableKeys(const StringTableData& table, std::string_view key)
		{
			const size_t limit = GetMaxSuggestionDistance(key);
			std::vector<std::string_view> candidates;
			for (const StringTableEntry& entry : table.Entries)
			{
				const size_t difference = entry.Key.size() > key.size() ? entry.Key.size() - key.size() : key.size() - entry.Key.size();
				if (difference <= limit)
					candidates.push_back(entry.Key);
			}
			const std::vector<std::string> suggestions = FuzzySuggest(key, candidates);
			return MakeDidYouMeanHint(suggestions);
		}

		static int LocGet(ScriptCall& call)
		{
			const AssetHandle handle = Lua::Check<AssetHandle>(call, 1);
			const std::string key = Lua::Check<std::string>(call, 2);
			const CheckedStringTable table = LoadStringTable(call, handle);
			const std::string* value = FindLocalizedString(*table.Table, key, call.Engine->GetLocale());
			if (value == nullptr)
			{
				// A key no table can hold (too long, or not UTF-8) is named by its length only, so the message stays short and
				// valid text.
				const bool printable = key.size() <= MaxStringTableKeyBytes && IsValidUtf8(key);
				const std::string named = printable ? std::format("the key '{}'", key) : std::format("a key of {} bytes", key.size());
				std::string hint = printable ? SuggestStringTableKeys(*table.Table, key) : std::string();
				if (hint.empty())
					hint = "add the key to the table, or check its spelling (keys are compared byte-wise, case included)";
				return Lua::RaiseError(call, Error(ErrorCode::NotFound, std::format("the string table '{}' has no entry for {}", table.Path, named)).WithHint(std::move(hint)));
			}
			Lua::PushString(call, *value);
			return 1;
		}

		static int LocHas(ScriptCall& call)
		{
			const AssetHandle handle = Lua::Check<AssetHandle>(call, 1);
			const std::string key = Lua::Check<std::string>(call, 2);
			const CheckedStringTable table = LoadStringTable(call, handle);
			Lua::Push(call, FindStringTableEntry(*table.Table, key) != nullptr);
			return 1;
		}

		static int LocGetLocale(ScriptCall& call)
		{
			Lua::PushString(call, call.Engine->GetLocale());
			return 1;
		}

		// M38 (decision 2): the tag first (a located InvalidArgument), then the dispatch's refusals (no change inside
		// OnLocaleChanged or at teardown), then nothing for the locale in force (no callback, no mutation notice); otherwise
		// the host's write permission and recording notice (PrepareHostMutation), the session's change (IScriptHost::SetLocale)
		// and OnLocaleChanged before returning.
		static int LocSetLocale(ScriptCall& call)
		{
			const std::string locale = Lua::Check<std::string>(call, 1);
			if (!locale.empty() && !IsLocaleTag(locale))
			{
				const bool printable = locale.size() <= MaxStringTableLocaleBytes && IsValidUtf8(locale);
				const std::string named = printable ? std::format("'{}'", locale) : std::format("a string of {} bytes", locale.size());
				return Lua::RaiseError(call, Error(ErrorCode::InvalidArgument, std::format("{} is not a locale: Loc.SetLocale takes the empty locale or a tag", named)).WithHint(std::format("a tag is 1 to {} ASCII letters, digits and '-' starting with a letter, such as de, ar or pt-BR", MaxStringTableLocaleBytes)));
			}
			const Status allowed = call.Engine->CheckLocaleChange();
			if (!allowed)
				return Lua::RaiseError(call, allowed.error());
			if (locale == call.Engine->GetLocale())
				return 0;
			const Status permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			const std::string previous(call.Engine->GetLocale());
			const Status changed = call.Engine->GetHost().SetLocale(locale);
			if (!changed)
				return Lua::RaiseError(call, changed.error());
			const Status notified = call.Engine->NotifyLocaleChanged(previous);
			if (!notified)
				return Lua::RaiseError(call, notified.error());
			return 0;
		}

	}

	namespace ScriptBindings {

		Status RegisterLoc(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			api.Module("Loc",
				   "Localized strings from string tables (.stringtable assets) in the session's locale: a table's locale equal to it, else its "
				   "language's, else the table's first (fallback) locale; an entry without a string for that locale gives the fallback's. A session "
				   "starts in the project's Localization.Locale (or play.start's locale, the Runtime's --locale, a suite's Locale, a replay's "
				   "header), and Loc.SetLocale changes it for the rest of the session.")
				.Function("Get", &Utils::LocGet, "(table: AssetRef, key: string) -> string",
					"Returns the string of key in the locale. A key the table lacks, an asset that is not a string table, or a table that fails "
					"to load raises a located error naming them (with the closest keys as a hint).",
					read)
				.Function("Has", &Utils::LocHas, "(table: AssetRef, key: string) -> boolean",
					"Returns whether the string table has the key. An asset that is not a string table, or a table that fails to load, "
					"raises a located error naming it, as in Get.",
					read)
				.Function("GetLocale", &Utils::LocGetLocale, "() -> string",
					"Returns the session's locale that lookups use: the one it started in, or the last Loc.SetLocale's; empty reads every "
					"table's fallback locale.",
					read)
				.Function("SetLocale", &Utils::LocSetLocale, "(locale: string) -> ()",
					"Sets the session's locale, which Get and GetLocale read from the next call and which lasts through Scene.Load and "
					"Scene.Reload: empty (every table's fallback) or a tag such as de, ar or pt-BR, else a located error. Before it returns, "
					"OnLocaleChanged(locale, previous) reaches every started behaviour that defines it, in execution order; the locale in force "
					"changes nothing, and a call inside OnLocaleChanged raises. A game restores its player's saved choice with it at startup.");
			return {};
		}

	}

}
