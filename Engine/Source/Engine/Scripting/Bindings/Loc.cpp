#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Scripting/ScriptApiRegistry.h"

// The Loc module (M22, C22.1, R-UI-L10N; Docs/Decisions/0033-m22-contract.md decision 11): localized strings from string
// tables (Asset/StringTableData.h) in the VM's locale (ScriptEngine::GetLocale, the project's Localization.Locale). The
// registration is final; the functions are stubs of the contract that stream L implements.

namespace Engine {

	namespace Utils {

		static int LocGet(ScriptCall& call)
		{
			ENGINE_CONTRACT_STUB();
			return Lua::RaiseError(call, "Loc.Get is not implemented yet (Docs/Decisions/0033-m22-contract.md decision 11, stream L)");
		}

		static int LocHas(ScriptCall& call)
		{
			ENGINE_CONTRACT_STUB();
			return Lua::RaiseError(call, "Loc.Has is not implemented yet (Docs/Decisions/0033-m22-contract.md decision 11, stream L)");
		}

		static int LocGetLocale(ScriptCall& call)
		{
			ENGINE_CONTRACT_STUB();
			return Lua::RaiseError(call, "Loc.GetLocale is not implemented yet (Docs/Decisions/0033-m22-contract.md decision 11, stream L)");
		}

	}

	namespace ScriptBindings {

		Status RegisterLoc(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			api.Module("Loc",
				   "Localized strings from string tables (.stringtable assets) in the project's locale (Localization.Locale): a table's locale equal "
				   "to it, else its language's, else the table's first (fallback) locale; an entry without a string for that locale gives the "
				   "fallback's.")
				.Function("Get", &Utils::LocGet, "(table: AssetRef, key: string) -> string",
					"Returns the string of key in the locale. A key the table lacks, an asset that is not a string table, or a table that fails "
					"to load raises a located error naming them (with the closest keys as a hint).",
					read)
				.Function("Has", &Utils::LocHas, "(table: AssetRef, key: string) -> boolean",
					"Returns whether the string table has the key. An asset that is not a string table, or a table that fails to load, "
					"raises a located error naming it, as in Get.",
					read)
				.Function("GetLocale", &Utils::LocGetLocale, "() -> string",
					"Returns the project's locale (Localization.Locale) that lookups use; empty when it is unset, so every table reads its "
					"fallback locale.",
					read);
			return {};
		}

	}

}
