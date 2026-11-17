#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Scene/AudioSystem.h"
#include "Engine/Scene/SessionMusic.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace Engine {

	namespace Utils {

		// The session's AudioSystem, or null in a play session without an audio engine (it has the session's music, M40):
		// there the mixer calls check their arguments and are then ignored, GetGroupVolume reading 1, the default mix
		// (Docs/Decisions/0057-m40-music.md decision 6: test discovery's scratch sessions run each scene's startup callbacks
		// without audio). "audio is unavailable" outside a play session.
		static AudioSystem* ScriptAudio(ScriptCall& call)
		{
			auto& host = call.Engine->GetHost();
			AudioSystem* audio = host.GetAudio();
			if (audio == nullptr && host.GetMusic() == nullptr)
				Lua::RaiseError(call, "audio is unavailable");
			return audio;
		}
		static int AudioPlayOneShot(ScriptCall& call)
		{
			AudioSystem* audio = ScriptAudio(call);
			const auto clip = Lua::Check<AssetHandle>(call, 1);
			const auto position = Lua::IsNoneOrNil(call, 2) ? std::nullopt : std::optional{ Lua::Check<glm::vec3>(call, 2) };
			const float volume = Lua::IsNoneOrNil(call, 3) ? 1.0f : Lua::Check<float>(call, 3);
			const auto group = static_cast<AudioGroup>(Lua::CheckEnum(call, 4, "AudioGroup"));
			if (!clip.IsValid() || volume < 0)
				return Lua::RaiseError(call, "one-shot needs a valid clip and nonnegative volume");
			// The checks AudioSystem::PlayOneShot and the engine make, so a call is refused alike with or without an engine.
			if (!std::isfinite(volume) || (position.has_value() && !(std::isfinite(position->x) && std::isfinite(position->y) && std::isfinite(position->z))))
				return Lua::RaiseError(call, "one-shot needs a finite volume and position");
			auto permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			if (audio == nullptr)
				return 0;
			auto voice = audio->PlayOneShot(clip, position, volume, group);
			if (!voice)
				return Lua::RaiseError(call, voice.error());
			return 0;
		}
		static int AudioSetGroupVolume(ScriptCall& call)
		{
			AudioSystem* audio = ScriptAudio(call);
			const auto group = static_cast<AudioGroup>(Lua::CheckEnum(call, 1, "AudioGroup"));
			const float volume = Lua::Check<float>(call, 2);
			if (volume < 0)
				return Lua::RaiseError(call, "volume must be nonnegative");
			if (!std::isfinite(volume))
				return Lua::RaiseError(call, "volume must be finite");
			auto permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			if (audio == nullptr)
				return 0;
			auto result = audio->SetGroupVolume(group, volume);
			if (!result)
				return Lua::RaiseError(call, result.error());
			return 0;
		}
		static int AudioGetGroupVolume(ScriptCall& call)
		{
			AudioSystem* audio = ScriptAudio(call);
			const auto group = static_cast<AudioGroup>(Lua::CheckEnum(call, 1, "AudioGroup"));
			Lua::Push(call, audio != nullptr ? audio->GetGroupVolume(group) : 1.0f);
			return 1;
		}

		// M40 (Docs/Decisions/0057-m40-music.md decision 6): the session's music (a session without an audio engine has one
		// too), or "audio is unavailable" outside a play session (the edit evaluation, fixtures without one).
		static SessionMusic& ScriptMusic(ScriptCall& call)
		{
			SessionMusic* music = call.Engine->GetHost().GetMusic();
			if (music == nullptr)
				Lua::RaiseError(call, "audio is unavailable");
			return *music;
		}

		// The keys of a MusicOptions table, and the longest key an error names in full.
		constexpr std::array<std::string_view, 4> MusicOptionKeys = { "Volume", "FadeSeconds", "Loop", "Intro" };
		constexpr size_t MaxShownMusicOptionKeyBytes = 32;

		// The MusicOptions member `name` of the table at `index`, read raw: nil leaves the request's default; a value of
		// another type than `type` raises, naming the member. Leaves the value on the stack when it returns true.
		static bool PushMusicOption(ScriptCall& call, int index, const char* name, int type, std::string_view typeName)
		{
			lua_rawgetfield(call.State, index, name);
			if (lua_isnil(call.State, -1))
			{
				lua_pop(call.State, 1);
				return false;
			}
			if (lua_type(call.State, -1) != type)
				Lua::RaiseError(call, std::format("options.{} expected {}, got {}", name, typeName, luaL_typename(call.State, -1)));
			return true;
		}

		// Audio.PlayMusic's options (argument `index`): nil, or a table holding only MusicOptionKeys, whose values have
		// their types (the ranges are ValidateMusicRequest's).
		static void ReadMusicOptions(ScriptCall& call, int index, MusicRequest& request)
		{
			if (Lua::IsNoneOrNil(call, index))
				return;
			if (!lua_istable(call.State, index))
				Lua::RaiseError(call, std::format("argument #{} expected MusicOptions table, got {}", index, luaL_typename(call.State, index)));
			lua_pushnil(call.State);
			while (lua_next(call.State, index) != 0)
			{
				if (lua_type(call.State, -2) != LUA_TSTRING)
					Lua::RaiseError(call, std::format("options has a {} key; MusicOptions keys are Volume, FadeSeconds, Loop and Intro", luaL_typename(call.State, -2)));
				size_t length = 0;
				const char* text = lua_tolstring(call.State, -2, &length);
				const std::string_view key(text, length);
				if (std::ranges::find(MusicOptionKeys, key) == MusicOptionKeys.end())
				{
					const std::string shown = length <= MaxShownMusicOptionKeyBytes ? std::string(key) : std::format("a key of {} bytes", length);
					Lua::RaiseError(call, std::format("unknown MusicOptions key '{}': expected Volume, FadeSeconds, Loop or Intro", shown));
				}
				lua_pop(call.State, 1);
			}
			if (PushMusicOption(call, index, "Volume", LUA_TNUMBER, "number"))
			{
				// Kept as asked (ValidateMusicRequest checks its range), so Audio.GetMusic returns the very number.
				request.Volume = lua_tonumber(call.State, -1);
				lua_pop(call.State, 1);
			}
			if (PushMusicOption(call, index, "FadeSeconds", LUA_TNUMBER, "number"))
			{
				request.FadeSeconds = lua_tonumber(call.State, -1);
				lua_pop(call.State, 1);
			}
			if (PushMusicOption(call, index, "Loop", LUA_TBOOLEAN, "boolean"))
			{
				request.Loop = lua_toboolean(call.State, -1) != 0;
				lua_pop(call.State, 1);
			}
			if (PushMusicOption(call, index, "Intro", LUA_TUSERDATA, "AssetRef"))
			{
				// Another engine value (an Entity, a Color) is a userdata too: only an AssetRef is an intro.
				if (lua_userdatatag(call.State, -1) != std::to_underlying(Detail::ScriptValueTag::AssetRef))
					Lua::RaiseError(call, "options.Intro expected AssetRef, got another engine value");
				request.Intro = Lua::Check<AssetHandle>(call, lua_gettop(call.State));
				lua_pop(call.State, 1);
			}
		}

		static int AudioPlayMusic(ScriptCall& call)
		{
			SessionMusic& music = ScriptMusic(call);
			MusicRequest request;
			request.Clip = Lua::Check<AssetHandle>(call, 1);
			ReadMusicOptions(call, 2, request);
			// Every check, the intro's format included, before the host mutation, so a refused call changes nothing.
			if (const Status valid = music.Check(request); !valid)
				return Lua::RaiseError(call, valid.error());
			const auto permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			if (const Status played = music.Play(request); !played)
				return Lua::RaiseError(call, played.error());
			return 0;
		}

		static int AudioStopMusic(ScriptCall& call)
		{
			SessionMusic& music = ScriptMusic(call);
			const double fadeSeconds = Lua::IsNoneOrNil(call, 1) ? 0.0 : Lua::Check<double>(call, 1);
			if (const Status valid = ValidateMusicFade(fadeSeconds); !valid)
				return Lua::RaiseError(call, valid.error());
			const auto permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			if (const Status stopped = music.Stop(fadeSeconds); !stopped)
				return Lua::RaiseError(call, stopped.error());
			return 0;
		}

		static int AudioGetMusic(ScriptCall& call)
		{
			const std::optional<MusicState>& state = ScriptMusic(call).GetState();
			if (!state.has_value())
			{
				Lua::PushNil(call);
				return 1;
			}
			lua_createtable(call.State, 0, 4);
			Lua::Push(call, state->Clip);
			lua_setfield(call.State, -2, "Clip");
			// nil for a request without an intro (an invalid handle pushes nil).
			Lua::Push(call, state->Intro);
			lua_setfield(call.State, -2, "Intro");
			Lua::Push(call, state->Volume);
			lua_setfield(call.State, -2, "Volume");
			Lua::Push(call, state->Loop);
			lua_setfield(call.State, -2, "Loop");
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterAudio(ScriptApiRegistry& api)
		{
			// M40 (Docs/Decisions/0057-m40-music.md decision 6): the session's music.
			ENGINE_TRY(api.RegisterAlias("MusicOptions", "{Volume: number?, FadeSeconds: number?, Loop: boolean?, Intro: AssetRef?}",
				std::format("Audio.PlayMusic's options: Volume, the music's linear level (finite, at least 0; default 1); FadeSeconds, the "
							"fade to it (0 to {}; default 0, at once); Loop (default true), which loops the clip after its intro; Intro, a "
							"clip played once before the clip, which follows it from the first frame after its last (both of one sample rate "
							"and channel count). No other key is accepted.",
					MaxMusicFadeSeconds)));
			ENGINE_TRY(api.RegisterAlias("MusicState", "{Clip: AssetRef, Intro: AssetRef?, Volume: number, Loop: boolean}",
				"The session's music as scripts last asked for it with Audio.PlayMusic: its clip, intro, Volume and Loop."));
			const ScriptMemberOptions read{ .Mutates = false };
			api.Module("Audio", "Session-scoped mixing, one-shot playback and the session's music.")
				.Function("PlayOneShot", Utils::AudioPlayOneShot, "(clip: AssetRef, position: vector?, volume: number?, group: AudioGroup?) -> ()",
					std::format("Play a non-spatial one-shot when position is nil, otherwise spatial; volume defaults 1 and group Sfx. At most {} one-shots start per frame; a later call in the frame raises an error. A play session without audio (a test run's discovery of its suites) checks the call and plays nothing.", MaxAudioOneShotsPerFrame),
					{ .EnumParameters = { { 4, "AudioGroup", true, "Sfx" } } })
				.Function("SetGroupVolume", Utils::AudioSetGroupVolume, "(group: AudioGroup, volume: number) -> ()", "Set nonnegative session group gain; the prior mix is restored when the audio system stops. A play session without audio checks the call and ignores it.", { .EnumParameters = { { 1, "AudioGroup", false, {} } } })
				.Function("GetGroupVolume", Utils::AudioGetGroupVolume, "(group: AudioGroup) -> number", "Read the session group gain; 1, the default mix, in a play session without audio.", { .Mutates = false, .EnumParameters = { { 1, "AudioGroup", false, {} } } })
				.Function("PlayMusic", Utils::AudioPlayMusic, "(clip: AssetRef, options: MusicOptions?) -> ()",
					std::format("Plays clip as the session's music: non-spatial, in the Music group, at music priority (effects never steal "
								"it), owned by the play session, so it plays on through Scene.Load and Scene.Reload. With no music playing, "
								"clip starts, its level rising from 0 to Volume over FadeSeconds. When clip is the music already playing, its "
								"voice is kept, never restarted: its level fades from where it is to Volume (which also ducks it) and Loop is "
								"applied, so every scene of a chain calls PlayMusic with its track and the track goes on through each "
								"doorway. Another clip crossfades over FadeSeconds: the playing voice fades out and stops while clip fades "
								"in, and a clip still fading out is taken back and fades up again. At most {} music voices play at once (the "
								"current one and those still fading out; the oldest fading one stops first). A clip played once (Loop false) stays the "
								"session's music until StopMusic or another PlayMusic, and starts again when asked for after it ended. With "
								"Intro, a voice that starts plays the intro once, then the clip from the first frame after the intro's last, "
								"sample-accurate, Loop looping the clip (an opening and its loop as two files); the intro matters only when a "
								"voice starts, and a missing one plays the clip alone. Fades "
								"follow the voice's mixed frames: simulation time in lockstep, test runs and replays. Callable in every "
								"callback, phase and test case; the engine's own failures (a voice pool full of higher-priority voices) are "
								"logged, not raised; a session without audio (a test run's discovery of its suites) records the request and "
								"plays nothing. Raises for a nil clip, an unknown option or one out of range, an intro whose sample rate or "
								"channel count differs from the clip's, and \"audio is unavailable\" outside a play session (edit "
								"evaluations).",
						MaxMusicVoices))
				.Function("StopMusic", Utils::AudioStopMusic, "(fadeSeconds: number?) -> ()",
					std::format("Fades the session's music out over fadeSeconds (0 to {}; default 0, at once) and stops it; Audio.GetMusic returns "
								"nil from then on. Does nothing without music. Raises for a fade out of range and \"audio is unavailable\" "
								"outside a play session.",
						MaxMusicFadeSeconds))
				.Function("GetMusic", Utils::AudioGetMusic, "() -> MusicState?",
					"Returns the session's music as scripts last asked for it with Audio.PlayMusic (its clip, intro, Volume and Loop), or nil before "
					"the first PlayMusic and after StopMusic. It never reports the voice's progress, so it reads the same in live play, "
					"lockstep, test runs and replays, with or without audio. Raises \"audio is unavailable\" outside a play session.",
					read);
			return {};
		}

	}
}
