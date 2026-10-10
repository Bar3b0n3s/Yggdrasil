#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"

namespace Engine {

	namespace Utils {

		static AssetManager& BindingAssets(ScriptCall& call)
		{
			auto* assets = call.Engine->GetHost().GetAssets();
			if (assets == nullptr)
				Lua::RaiseError(call, "asset service is unavailable");
			return *assets;
		}

		static int AssetsLoad(ScriptCall& call)
		{
			const std::string reference = Lua::Check<std::string>(call, 1);
			if (!IsValidUtf8(reference) || reference.find('\0') != std::string::npos)
				return Lua::RaiseError(call, "reference must be UTF-8 without embedded NULs");
			auto& assets = BindingAssets(call);
			const auto handle = assets.Resolve(reference);
			if (!handle)
			{
				Lua::PushNil(call);
				return 1;
			}
			const auto loaded = assets.Load(*handle);
			if (!loaded)
				return Lua::RaiseError(call, loaded.error());
			Lua::Push(call, *handle);
			return 1;
		}

		static int AssetsValid(ScriptCall& call)
		{
			const AssetHandle handle = Lua::IsNoneOrNil(call, 1) ? AssetHandle{} : Lua::Check<AssetHandle>(call, 1);
			Lua::Push(call, handle.IsValid() && BindingAssets(call).GetAssetType(handle) != AssetType::None);
			return 1;
		}

		static int AssetsPath(ScriptCall& call)
		{
			const AssetHandle handle = Lua::Check<AssetHandle>(call, 1);
			auto& assets = BindingAssets(call);
			Lua::Push(call, assets.GetAssetType(handle) == AssetType::None ? std::string{} : assets.GetReferencePath(handle));
			return 1;
		}

		static int AssetsType(ScriptCall& call)
		{
			const AssetHandle handle = Lua::Check<AssetHandle>(call, 1);
			Lua::PushString(call, AssetTypeToString(BindingAssets(call).GetAssetType(handle)));
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterAssets(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			static_cast<void>(api.Type("AssetRef", "An opaque immutable asset handle; resolve metadata through Assets."));
			api.Module("Assets", "Resolves and loads project or built-in assets through the active asset manager.")
				.Function("Load", &Utils::AssetsLoad, "(reference: string) -> AssetRef?", "Loads a path or hexadecimal reference; missing references return nil and loading failures raise a located error.", read)
				.Function("IsValid", &Utils::AssetsValid, "(asset: AssetRef?) -> boolean", "Returns whether the handle is currently registered; nil is false.", read)
				.Function("GetPath", &Utils::AssetsPath, "(asset: AssetRef) -> string", "Returns the readable reference path, or an empty string for an unknown handle.", read)
				.Function("GetType", &Utils::AssetsType, "(asset: AssetRef) -> string", "Returns the asset type name, or None for an unknown handle.", read);
			return {};
		}

	}

}
