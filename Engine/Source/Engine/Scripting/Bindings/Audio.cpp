#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Scene/AudioSystem.h"
#include "Engine/Scripting/ScriptEngine.h"

namespace Engine {

	namespace Utils {

		static AudioSystem& ScriptAudio(ScriptCall& call)
		{
			auto* audio = call.Engine->GetHost().GetAudio();
			if (!audio)
				Lua::RaiseError(call, "audio is unavailable");
			return *audio;
		}
		static int AudioPlayOneShot(ScriptCall& call)
		{
			auto& audio = ScriptAudio(call);
			const auto clip = Lua::Check<AssetHandle>(call, 1);
			const auto position = Lua::IsNoneOrNil(call, 2) ? std::nullopt : std::optional{ Lua::Check<glm::vec3>(call, 2) };
			const float volume = Lua::IsNoneOrNil(call, 3) ? 1.0f : Lua::Check<float>(call, 3);
			const auto group = static_cast<AudioGroup>(Lua::CheckEnum(call, 4, "AudioGroup"));
			if (!clip.IsValid() || volume < 0)
				return Lua::RaiseError(call, "one-shot needs a valid clip and nonnegative volume");
			auto permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			auto voice = audio.PlayOneShot(clip, position, volume, group);
			if (!voice)
				return Lua::RaiseError(call, voice.error());
			return 0;
		}
		static int AudioSetGroupVolume(ScriptCall& call)
		{
			auto& audio = ScriptAudio(call);
			const auto group = static_cast<AudioGroup>(Lua::CheckEnum(call, 1, "AudioGroup"));
			const float volume = Lua::Check<float>(call, 2);
			if (volume < 0)
				return Lua::RaiseError(call, "volume must be nonnegative");
			auto permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			auto result = audio.SetGroupVolume(group, volume);
			if (!result)
				return Lua::RaiseError(call, result.error());
			return 0;
		}
		static int AudioGetGroupVolume(ScriptCall& call)
		{
			auto& audio = ScriptAudio(call);
			const auto group = static_cast<AudioGroup>(Lua::CheckEnum(call, 1, "AudioGroup"));
			Lua::Push(call, audio.GetGroupVolume(group));
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterAudio(ScriptApiRegistry& api)
		{
			api.Module("Audio", "Session-scoped mixing and one-shot playback.")
				.Function("PlayOneShot", Utils::AudioPlayOneShot, "(clip: AssetRef, position: vector?, volume: number?, group: AudioGroup?) -> ()", "Play a non-spatial one-shot when position is nil, otherwise spatial; volume defaults 1 and group Sfx.", { .EnumParameters = { { 4, "AudioGroup", true, "Sfx" } } })
				.Function("SetGroupVolume", Utils::AudioSetGroupVolume, "(group: AudioGroup, volume: number) -> ()", "Set nonnegative session group gain; the prior mix is restored when the audio system stops.", { .EnumParameters = { { 1, "AudioGroup", false, {} } } })
				.Function("GetGroupVolume", Utils::AudioGetGroupVolume, "(group: AudioGroup) -> number", "Read the session group gain.", { .Mutates = false, .EnumParameters = { { 1, "AudioGroup", false, {} } } });
			return {};
		}

	}
}
