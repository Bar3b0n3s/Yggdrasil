#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Engine/Scripting/ScriptProxy.h"

#include <algorithm>
#include <format>
#include <string>
#include <utility>

// The sprite proxies' methods (M21, Docs/Decisions/0032-m21-contract.md decision 13): SpriteRenderer:SetFrameByName, and
// SpriteAnimator:Play and Stop, which set the playback state the SpriteAnimation phase advances (Scene/SpriteAnimationSystem.h,
// decision 5). Every write goes through the reflected field path (ScriptProxy::WriteField), so it is validated, recorded and
// counted as a field write from a script is.

namespace Engine {

	namespace Utils {

		// The receiver of a sprite method, which writes: a component of `expected` on a live entity (ValidateComponent refuses
		// one pending destruction).
		static ScriptProxyIdentity SpriteSelf(ScriptCall& call, std::string_view expected)
		{
			const auto proxy = Lua::Check<ScriptProxyIdentity>(call, 1);
			IScriptHost& host = call.Engine->GetHost();
			if (const Status valid = ScriptProxy::ValidateComponent(host, proxy); !valid)
				Lua::RaiseError(call, valid.error());
			if (host.GetTypes().GetComponents()[proxy.ComponentTypeIndex]->GetName() != expected)
				Lua::RaiseError(call, "wrong component receiver");
			return proxy;
		}

		static void SpriteWrite(ScriptCall& call, ScriptProxyIdentity proxy, std::string_view field, const Value& value)
		{
			if (const Status written = ScriptProxy::WriteField(call, proxy, field, value); !written)
				Lua::RaiseError(call, written.error());
		}

		// SpriteRenderer:SetFrameByName(name): Frame becomes the index of the sheet's frame named `name` (frame names are unique
		// when not empty, SpriteSheetData.h). NotFound, naming the sheet, when it has no such frame or the renderer has no sheet.
		static int SpriteSetFrameByName(ScriptCall& call)
		{
			const ScriptProxyIdentity proxy = SpriteSelf(call, "SpriteRenderer");
			const std::string name = Lua::Check<std::string>(call, 2);
			IScriptHost& host = call.Engine->GetHost();
			const Entity entity = host.GetScene().FindEntityByID(proxy.Entity.ID);
			const AssetHandle handle = entity.GetComponent<SpriteRendererComponent>().Sheet.GetHandle();
			if (!handle.IsValid())
			{
				return Lua::RaiseError(call,
					Error(ErrorCode::NotFound, std::format("the SpriteRenderer has no sprite sheet, so it has no frame named '{}'", name))
						.WithHint("set the SpriteRenderer's Sheet first"));
			}
			AssetManager* assets = host.GetAssets();
			if (assets == nullptr)
				return Lua::RaiseError(call, "asset service is unavailable");
			const Result<AssetRef<Asset>> loaded = Detail::LoadAssetAsHost(call, *assets, handle);
			if (!loaded)
				return Lua::RaiseError(call, loaded.error());
			const AssetRef<SpriteSheetData> sheet = AssetCast<SpriteSheetData>(*loaded);
			if (sheet == nullptr)
				return Lua::RaiseError(call, std::format("the SpriteRenderer's Sheet '{}' is not a sprite sheet", assets->GetReferencePath(handle)));
			const auto frame = std::ranges::find(sheet->Frames, name, &SpriteFrame::Name);
			if (name.empty() || frame == sheet->Frames.end())
			{
				return Lua::RaiseError(call,
					Error(ErrorCode::NotFound, std::format("the sprite sheet '{}' has no frame named '{}'", assets->GetReferencePath(handle), name))
						.WithHint("name a frame of the sheet, or set Frame to its index"));
			}
			SpriteWrite(call, proxy, "Frame", Value::FromUInt32(static_cast<uint32_t>(frame - sheet->Frames.begin())));
			return 0;
		}

		// SpriteAnimator:Play(clip): Clip = clip, Step and StepTicks 0, Playing true. The clip's first step shows at the next
		// SpriteAnimation phase; a name the sheet's clips lack plays nothing there (SPRITE_UNKNOWN_CLIP).
		static int SpritePlay(ScriptCall& call)
		{
			const ScriptProxyIdentity proxy = SpriteSelf(call, "SpriteAnimator");
			std::string clip = Lua::CheckBoundedString(call, 2, MaxScriptComponentStringBytes);
			SpriteWrite(call, proxy, "Clip", Value::FromString(std::move(clip)));
			SpriteWrite(call, proxy, "Step", Value::FromUInt32(0));
			SpriteWrite(call, proxy, "StepTicks", Value::FromUInt32(0));
			SpriteWrite(call, proxy, "Playing", Value::FromBool(true));
			return 0;
		}

		// SpriteAnimator:Stop(): Playing false; the SpriteRenderer keeps the frame shown.
		static int SpriteStop(ScriptCall& call)
		{
			const ScriptProxyIdentity proxy = SpriteSelf(call, "SpriteAnimator");
			SpriteWrite(call, proxy, "Playing", Value::FromBool(false));
			return 0;
		}

	}

	namespace ScriptBindings {

		Status RegisterSprites(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions write{};
			api.Type("SpriteRenderer")
				.Method("SetFrameByName", Utils::SpriteSetFrameByName, "(self: SpriteRenderer, name: string) -> ()",
					"Set Frame to the index of the sprite sheet's frame named name; an error names the sheet when it has no such frame.", write);
			api.Type("SpriteAnimator")
				.Method("Play", Utils::SpritePlay, "(self: SpriteAnimator, clip: string) -> ()",
					"Play the sheet's clip named clip from its first step, which shows at the next fixed step's SpriteAnimation phase.", write)
				.Method("Stop", Utils::SpriteStop, "(self: SpriteAnimator) -> ()", "Stop advancing the clip; the frame shown stays.", write);
			return {};
		}

	}

}
