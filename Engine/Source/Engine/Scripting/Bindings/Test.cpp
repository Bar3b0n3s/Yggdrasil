#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Animation/AnimationPose.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/AudioClipData.h"
#include "Engine/Audio/AudioDecoder.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Platform/Input/KeyCodes.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Scene/Components/AnimationPoseComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/ParticleSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SessionMusic.h"
#include "Engine/Scripting/Private/ScriptEngineAccess.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Engine/Scripting/ScriptProxy.h"
#include "Engine/Scripting/ScriptTestHost.h"

#include <lua.h>
#include <lualib.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

		static IScriptTestHost& Host(ScriptCall& call)
		{
			if (!call.Engine || !call.Engine->IsTestMode() || !call.Engine->GetTestHost())
				Lua::RaiseError(call, "Test is only available in test runs");
			return *call.Engine->GetTestHost();
		}

		static void Checked(ScriptCall& call, Status result)
		{
			if (!result)
				Lua::RaiseError(call, result.error());
		}

		// Reported messages are diagnostics the runner keeps: authored locations, at most MaxScriptLogMessageBytes bytes.
		static std::string Message(ScriptCall& call, int index, std::string_view fallback)
		{
			return Lua::IsNoneOrNil(call, index) ? std::string(fallback) : Lua::CheckDiagnosticText(call, index);
		}

		// The bytes of a string value an expectation's failure row shows; a longer string shows this prefix and its length.
		constexpr size_t MaxExpectationStringBytes = 128;

		// A string value of a failure row: quoted, with '"', '\' and control characters escaped, its first
		// MaxExpectationStringBytes bytes (cut at a UTF-8 boundary) followed by "... (<n> bytes)" when it is longer.
		static std::string QuoteValue(std::string_view text)
		{
			const std::string_view shown = text.substr(0, CompleteUtf8Prefix(text.substr(0, MaxExpectationStringBytes)));
			std::string quoted = "\"";
			for (const char character : shown)
			{
				switch (character)
				{
					case '"':  quoted += "\\\""; break;
					case '\\': quoted += "\\\\"; break;
					case '\n': quoted += "\\n"; break;
					case '\r': quoted += "\\r"; break;
					case '\t': quoted += "\\t"; break;
					default:
						if (static_cast<unsigned char>(character) < 0x20)
							quoted += std::format("\\x{:02x}", static_cast<unsigned char>(character));
						else
							quoted += character;
						break;
				}
			}
			quoted += '"';
			if (shown.size() < text.size())
				quoted += std::format("... ({} bytes)", text.size());
			return quoted;
		}

		// A value of an expectation's failure row, as deterministic text that runs no Luau and shows no address: nil, a boolean
		// or a number (shortest round-trip form) as Luau writes it, a string quoted (QuoteValue), a vector as vector(x, y, z),
		// a Color or Quat by its components, an Entity or AssetRef by its UUID, another engine value by its type name (its
		// metatable's __type) and any other value by its Luau type name.
		static std::string DescribeValue(ScriptCall& call, int index)
		{
			lua_State* state = call.State;
			switch (lua_type(state, index))
			{
				case LUA_TNIL:     return "nil";
				case LUA_TBOOLEAN: return lua_toboolean(state, index) ? "true" : "false";
				case LUA_TNUMBER:  return std::format("{}", lua_tonumber(state, index));
				case LUA_TVECTOR:
				{
					const float* vector = lua_tovector(state, index);
					return std::format("vector({}, {}, {})", vector[0], vector[1], vector[2]);
				}
				case LUA_TSTRING:
				{
					size_t length = 0;
					const char* text = lua_tolstring(state, index, &length);
					return QuoteValue(std::string_view(text, length));
				}
				case LUA_TUSERDATA:
				{
					switch (lua_userdatatag(state, index))
					{
						case std::to_underlying(Detail::ScriptValueTag::Entity):
							return std::format("Entity({})", Lua::Check<ScriptEntityIdentity>(call, index).ID.ToString());
						case std::to_underlying(Detail::ScriptValueTag::AssetRef):
							return std::format("AssetRef({})", Lua::Check<AssetHandle>(call, index).ToString());
						case std::to_underlying(Detail::ScriptValueTag::Color):
						{
							const glm::vec4& color = Lua::CheckColorStorage(call, index);
							return std::format("Color({}, {}, {}, {})", color.r, color.g, color.b, color.a);
						}
						case std::to_underlying(Detail::ScriptValueTag::Quat):
						{
							const glm::quat& rotation = Lua::CheckQuatStorage(call, index);
							return std::format("Quat({}, {}, {}, {})", rotation.x, rotation.y, rotation.z, rotation.w);
						}
						default:
							break;
					}
					if (luaL_getmetafield(state, index, "__type") != 0)
					{
						std::string name = lua_type(state, -1) == LUA_TSTRING ? Lua::Check<std::string>(call, -1) : std::string();
						lua_pop(state, 1);
						if (!name.empty())
							return name.substr(0, CompleteUtf8Prefix(std::string_view(name).substr(0, MaxExpectationStringBytes)));
					}
					return "userdata";
				}
				default:
					return lua_typename(state, lua_type(state, index));
			}
		}

		// The failure row of a value expectation: `message` (Message of argument `index`), then `values`, " (got <actual>,
		// expected <expected>[, epsilon <e>])". A message that leaves no room for the values is cut first, at a UTF-8 boundary,
		// and marked " [truncated: <n> bytes]" like any bounded diagnostic (n the full byte count of the script's text), so
		// the values always fit within MaxScriptLogMessageBytes.
		static std::string WithValues(ScriptCall& call, int index, std::string message, std::string_view values)
		{
			if (message.size() + values.size() > MaxScriptLogMessageBytes)
			{
				size_t fullBytes = message.size();
				if (!Lua::IsNoneOrNil(call, index))
					static_cast<void>(lua_tolstring(call.State, index, &fullBytes));
				const std::string marker = std::format(" [truncated: {} bytes]", fullBytes);
				message.resize(MaxScriptLogMessageBytes - values.size() - marker.size());
				message.resize(CompleteUtf8Prefix(message));
				message += marker;
			}
			message += values;
			return message;
		}

		static int Report(ScriptCall& call, ScriptTestSignal signal, std::string message)
		{
			const auto location = Detail::ScriptCallerLocation(call);
			ScriptTestReport report{ signal, std::move(message), location.File, location.Line };
			if (signal == ScriptTestSignal::ExpectationFailed)
				Host(call).Report(report);
			else
			{
				Checked(call, Detail::ScriptEngineAccess::EndTestCase(call, report));
				if (lua_isyieldable(call.State))
					return lua_yield(call.State, 0);
				return Lua::RaiseError(call, report.Message);
			}
			return 0;
		}

		static int Case(ScriptCall& call)
		{
			static_cast<void>(Host(call));
			const auto name = Lua::CheckBoundedString(call, 1, MaxScriptTestNameBytes);
			if (name.empty() || !lua_isfunction(call.State, 2))
				return Lua::RaiseError(call, "Test.Case needs a nonempty name and a function");
			uint32_t timeout = 0;
			if (!Lua::IsNoneOrNil(call, 3))
			{
				if (!lua_istable(call.State, 3))
					return Lua::RaiseError(call, "Test.Case options must be a table");
				lua_pushnil(call.State);
				while (lua_next(call.State, 3))
				{
					if (lua_type(call.State, -2) != LUA_TSTRING || Lua::Check<std::string>(call, -2) != "TimeoutTicks")
						return Lua::RaiseError(call, "unknown Test.Case option; expected TimeoutTicks");
					timeout = Lua::Check<uint32_t>(call, -1);
					if (timeout == 0)
						return Lua::RaiseError(call, "TimeoutTicks must be a positive integer");
					lua_pop(call.State, 1);
				}
			}
			Checked(call, Detail::ScriptEngineAccess::RegisterTestCase(call, 2, name, timeout, Detail::ScriptCallerLocation(call)));
			return 0;
		}

		static int Expect(ScriptCall& call)
		{
			const bool condition = Lua::Check<bool>(call, 1);
			const auto message = Message(call, 2, "expected condition to be true");
			return condition ? 0 : Report(call, ScriptTestSignal::ExpectationFailed, message);
		}

		static int ExpectEqual(ScriptCall& call)
		{
			if (Lua::GetArgumentCount(call) < 2)
				return Lua::RaiseError(call, "Test.ExpectEqual needs two values");
			auto message = Message(call, 3, "expected values to be equal");
			if (lua_equal(call.State, 1, 2))
				return 0;
			const std::string values = std::format(" (got {}, expected {})", DescribeValue(call, 1), DescribeValue(call, 2));
			return Report(call, ScriptTestSignal::ExpectationFailed, WithValues(call, 3, std::move(message), values));
		}

		static int ExpectNear(ScriptCall& call)
		{
			const double epsilon = Lua::IsNoneOrNil(call, 3) ? 0.00001 : Lua::Check<double>(call, 3);
			if (epsilon < 0)
				return Lua::RaiseError(call, "Test.ExpectNear epsilon must be nonnegative");
			bool near = false;
			if (lua_isvector(call.State, 1) && lua_isvector(call.State, 2))
			{
				const auto a = Lua::Check<glm::vec3>(call, 1), b = Lua::Check<glm::vec3>(call, 2);
				near = std::abs(static_cast<double>(a.x) - b.x) <= epsilon && std::abs(static_cast<double>(a.y) - b.y) <= epsilon && std::abs(static_cast<double>(a.z) - b.z) <= epsilon;
			}
			else
				near = std::abs(Lua::Check<double>(call, 1) - Lua::Check<double>(call, 2)) <= epsilon;
			auto message = Message(call, 4, "values differ by more than epsilon");
			if (near)
				return 0;
			const std::string values = std::format(" (got {}, expected {}, epsilon {})", DescribeValue(call, 1), DescribeValue(call, 2), epsilon);
			return Report(call, ScriptTestSignal::ExpectationFailed, WithValues(call, 4, std::move(message), values));
		}

		static int Fail(ScriptCall& call)
		{
			return Report(call, ScriptTestSignal::Fail, Lua::CheckDiagnosticText(call, 1));
		}
		static int Skip(ScriptCall& call)
		{
			return Report(call, ScriptTestSignal::Skip, Lua::CheckDiagnosticText(call, 1));
		}

		static int Wait(ScriptCall& call, Detail::ScriptTestWait request)
		{
			request.Location = Detail::ScriptCallerLocation(call);
			auto armed = Detail::ScriptEngineAccess::ArmTestWait(call, request);
			if (!request.Predicate.IsNull())
				call.Engine->ReleaseReference(request.Predicate);
			if (!armed)
				return Lua::RaiseError(call, armed.error());
			return *armed < 0 ? lua_yield(call.State, 0) : *armed;
		}

		static uint64_t Ticks(ScriptCall& call, int index)
		{
			const double ticks = Lua::Check<double>(call, index);
			if (ticks < 0 || ticks > 9007199254740991.0 || std::floor(ticks) != ticks)
				Lua::RaiseError(call, "wait ticks must be an exactly representable nonnegative integer");
			return static_cast<uint64_t>(ticks);
		}

		static int WaitTicks(ScriptCall& call)
		{
			Detail::ScriptTestWait request;
			request.Kind = Detail::ScriptWaitKind::Ticks;
			request.Ticks = Ticks(call, 1);
			return Wait(call, std::move(request));
		}

		static int WaitUntil(ScriptCall& call)
		{
			Detail::ScriptTestWait request;
			request.Kind = Detail::ScriptWaitKind::Predicate;
			request.Ticks = Ticks(call, 2);
			auto predicate = Detail::ScriptEngineAccess::RetainFunction(call, 1);
			if (!predicate)
				return Lua::RaiseError(call, predicate.error());
			request.Predicate = *predicate;
			return Wait(call, std::move(request));
		}

		static int ExpectScriptError(ScriptCall& call)
		{
			Detail::ScriptTestWait request;
			request.Kind = Detail::ScriptWaitKind::ExpectedError;
			request.Pattern = Lua::CheckBoundedString(call, 1, MaxScriptErrorPatternBytes);
			request.Ticks = Ticks(call, 2);
			return Wait(call, std::move(request));
		}

		static int CaptureAudio(ScriptCall& call)
		{
			Detail::ScriptTestWait request;
			request.Kind = Detail::ScriptWaitKind::Audio;
			request.Ticks = Lua::Check<uint32_t>(call, 1);
			return Wait(call, std::move(request));
		}

		static int Inject(ScriptCall& call, const Json& value)
		{
			Checked(call, call.CheckWritable());
			Checked(call, Host(call).InjectInput(value));
			return 0;
		}

		// The InputEventState enum (Down, Up, Tap), also the automation input vocabulary of §13.6. Registry dispatch has
		// validated and counted an enum argument before a callback runs; these map the value to its canonical spelling.
		constexpr std::array<std::string_view, 3> InputEventStates{ "Down", "Up", "Tap" };

		static std::string_view EventState(ScriptCall& call, int index)
		{
			const int64_t value = Lua::CheckEnum(call, index, "InputEventState");
			if (value < 0 || value >= static_cast<int64_t>(InputEventStates.size()))
				Lua::RaiseError(call, "invalid InputEventState value");
			return InputEventStates[static_cast<size_t>(value)];
		}

		static int InjectAction(ScriptCall& call)
		{
			Json value = { { "type", "action" }, { "name", Lua::Check<std::string>(call, 1) } };
			if (!Lua::IsNoneOrNil(call, 3))
			{
				// The third argument selects the analog overload. Validate the documented state argument even though the
				// canonical input event contains only Value (State and Value are mutually exclusive in the host parser).
				if (!Lua::IsNoneOrNil(call, 2) && EventState(call, 2) != "Down")
					return Lua::RaiseError(call, "analog Test.InjectAction requires Down or nil state");
				value["value"] = Lua::Check<float>(call, 3);
			}
			else
				value["state"] = EventState(call, 2);
			return Inject(call, value);
		}

		static int InjectKey(ScriptCall& call)
		{
			const auto key = KeyToString(static_cast<Key>(Lua::CheckEnum(call, 1, "Key")));
			return Inject(call, { { "type", "key" }, { "key", key }, { "state", EventState(call, 2) } });
		}

		static int InjectMouse(ScriptCall& call)
		{
			const auto button = MouseButtonToString(static_cast<MouseButton>(Lua::CheckEnum(call, 1, "MouseButton")));
			Json value = { { "type", "mouseButton" }, { "button", button }, { "state", EventState(call, 2) } };
			if (!Lua::IsNoneOrNil(call, 3))
			{
				const auto position = Lua::Check<glm::vec2>(call, 3);
				value["position"] = { position.x, position.y };
			}
			return Inject(call, value);
		}

		static int InjectGamepad(ScriptCall& call)
		{
			// The third argument's type selects the overload, and with it the enum of the second (EnumParameters).
			const auto index = Lua::Check<uint32_t>(call, 1);
			if (lua_type(call.State, 3) == LUA_TNUMBER)
			{
				const auto axis = GamepadAxisToString(static_cast<GamepadAxis>(Lua::CheckEnum(call, 2, "GamepadAxis")));
				return Inject(call, { { "type", "gamepadAxis" }, { "gamepad", index }, { "axis", axis }, { "value", Lua::Check<float>(call, 3) } });
			}
			if (lua_type(call.State, 3) != LUA_TSTRING)
				return Lua::RaiseError(call, std::format("argument #3 expected string or number, got {}", lua_typename(call.State, lua_type(call.State, 3))));
			const auto button = GamepadButtonToString(static_cast<GamepadButton>(Lua::CheckEnum(call, 2, "GamepadButton")));
			return Inject(call, { { "type", "gamepadButton" }, { "gamepad", index }, { "button", button }, { "state", EventState(call, 3) } });
		}

		// The pointer move, the wheel and typed text a recording suite needs for a slider drag, a scroll list and a text
		// field (Docs/Decisions/0033-m22-contract.md decision 52), in input.inject's service forms. Each argument is
		// validated here, before the host, as InjectMouse's position is.
		static int InjectMouseMove(ScriptCall& call)
		{
			const auto position = Lua::Check<glm::vec2>(call, 1);
			return Inject(call, { { "type", "mouseMove" }, { "position", { position.x, position.y } } });
		}

		static int InjectScroll(ScriptCall& call)
		{
			const auto delta = Lua::Check<glm::vec2>(call, 1);
			return Inject(call, { { "type", "scroll" }, { "delta", { delta.x, delta.y } } });
		}

		static int InjectText(ScriptCall& call)
		{
			std::string text = Lua::Check<std::string>(call, 1);
			if (text.empty())
				return Lua::RaiseError(call, "Test.InjectText needs at least one character");
			if (!IsValidUtf8(text))
				return Lua::RaiseError(call, std::format("Test.InjectText's text is not valid UTF-8 (at byte {})", FindInvalidUtf8(text)));
			return Inject(call, { { "type", "text" }, { "text", std::move(text) } });
		}

		// M28 (Docs/Decisions/0043-m28-game-services.md decision 6): a frame-time signal, as a replay plays one back; the input
		// path checks the name (1 to MaxScriptSignalNameBytes bytes of UTF-8).
		static int InjectSignal(ScriptCall& call)
		{
			std::string name = Lua::CheckBoundedString(call, 1, MaxScriptSignalNameBytes);
			return Inject(call, { { "type", "signal" }, { "name", std::move(name) } });
		}

		// M28 (Docs/Decisions/0043-m28-game-services.md decision 8): a suite's save data, which a test run keeps in memory.
		static int SetSaveData(ScriptCall& call)
		{
			static_cast<void>(Host(call));
			const Json values = Lua::CheckJson(call, 1);
			Checked(call, call.PrepareHostMutation());
			Checked(call, call.Engine->GetHost().ReplaceSaveData(values));
			return 0;
		}

		// M40 (Docs/Decisions/0057-m40-music.md decision 6): the session's current music voice as of the last AudioUpdate phase
		// (SessionMusic::GetVoices), or nil without one.
		static int GetMusic(ScriptCall& call)
		{
			static_cast<void>(Host(call));
			const SessionMusic* music = call.Engine->GetHost().GetMusic();
			if (music == nullptr)
				return Lua::RaiseError(call, "audio is unavailable");
			const MusicVoices& voices = music->GetVoices();
			if (!voices.CurrentInfo.has_value())
			{
				Lua::PushNil(call);
				return 1;
			}
			const AudioVoiceInfo& info = *voices.CurrentInfo;
			lua_createtable(call.State, 0, 8);
			Lua::PushString(call, std::format("{:016x}", voices.Current.Voice.GetValue()));
			lua_setfield(call.State, -2, "Voice");
			Lua::Push(call, voices.Current.Clip);
			lua_setfield(call.State, -2, "Clip");
			// nil for a voice without an intro (an invalid handle pushes nil).
			Lua::Push(call, voices.Current.Intro);
			lua_setfield(call.State, -2, "Intro");
			Lua::Push(call, !info.NextClip.IsNull() && !info.InNext);
			lua_setfield(call.State, -2, "InIntro");
			Lua::Push(call, static_cast<double>(info.CursorFrames));
			lua_setfield(call.State, -2, "CursorFrames");
			Lua::Push(call, static_cast<double>(info.LengthFrames));
			lua_setfield(call.State, -2, "LengthFrames");
			Lua::Push(call, info.FadeVolume);
			lua_setfield(call.State, -2, "FadeVolume");
			Lua::Push(call, static_cast<double>(voices.FadingOut.size()));
			lua_setfield(call.State, -2, "FadingOut");
			return 1;
		}

		static int Screenshot(ScriptCall& call)
		{
			IScriptTestHost& host = Host(call);
			const std::string name = Lua::Check<std::string>(call, 1);
			// Rendering and encoding the capture is host time, not the script's (ScriptEngine::RunHostOperation).
			Checked(call, call.Engine->RunHostOperation([&host, &name]()
			{
				return host.CaptureScreenshot(name);
			}));
			return 0;
		}
		static int ReloadScript(ScriptCall& call)
		{
			const auto asset = Lua::Check<AssetHandle>(call, 1);
			Checked(call, Host(call).ReloadScript(asset));
			return 0;
		}
		static int GetStateHash(ScriptCall& call)
		{
			Lua::PushString(call, UUID(Host(call).ComputeStateHash()).ToString());
			return 1;
		}

		static int GetLastExtraction(ScriptCall& call)
		{
			const auto value = Host(call).GetLastExtraction();
			lua_createtable(call.State, 0, 2);
			Lua::Push(call, value.Alpha);
			lua_setfield(call.State, -2, "Alpha");
			Lua::Push(call, static_cast<double>(value.Frame));
			lua_setfield(call.State, -2, "Frame");
			return 1;
		}

		static int GetExtractedPosition(ScriptCall& call)
		{
			const auto entity = Lua::Check<ScriptEntityIdentity>(call, 1);
			Checked(call, ScriptProxy::ValidateEntity(call.Engine->GetHost(), entity));
			const auto position = Host(call).GetExtractedPosition(entity.ID);
			if (position)
				Lua::Push(call, *position);
			else
				Lua::PushNil(call);
			return 1;
		}

		// The XXH64 of a clip's samples as interleaved f32 at its own rate: an encoded clip decoded by DecodeEncodedAudio (an
		// Ogg Opus clip through the fixed-point decoder, so the digest is the same in every configuration), a Pcm16 clip's
		// little-endian samples divided by 32768, the conversion miniaudio applies (exact for every 16-bit value).
		static Result<uint64_t> ComputeAudioClipDigest(const AudioClipData& clip)
		{
			if (clip.Encoding != AudioClipEncoding::Pcm16)
			{
				ENGINE_TRY_ASSIGN(const DecodedAudio decoded, DecodeEncodedAudio(clip.Bytes));
				return XXH64(std::as_bytes(std::span<const float>(decoded.Samples)));
			}
			std::vector<float> samples(clip.Bytes.size() / sizeof(int16_t));
			for (size_t index = 0; index < samples.size(); ++index)
			{
				const auto low = std::to_integer<uint16_t>(clip.Bytes[2 * index]);
				const auto high = std::to_integer<uint16_t>(clip.Bytes[2 * index + 1]);
				samples[index] = static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(low | (high << 8)))) / 32768.0f;
			}
			return XXH64(std::as_bytes(std::span<const float>(samples)));
		}

		static int GetAudioClipDigest(ScriptCall& call)
		{
			const auto handle = Lua::Check<AssetHandle>(call, 1);
			AssetManager* assets = call.Engine->GetHost().GetAssets();
			if (assets == nullptr)
				return Lua::RaiseError(call, "asset service is unavailable");
			// Loading (an import when the clip is not cached) and decoding are host work, outside the callback budget.
			Result<uint64_t> digest = MakeError(ErrorCode::Unsupported, "the audio clip {} was not decoded", handle.ToString());
			Checked(call, call.Engine->RunHostOperation([assets, handle, &digest]() -> Status
			{
				Result<AssetRef<Asset>> loaded = assets->Load(handle);
				if (!loaded)
				{
					digest = std::unexpected(std::move(loaded).error());
					return {};
				}
				const AssetRef<AudioClipData> clip = AssetCast<AudioClipData>(*loaded);
				if (clip == nullptr)
				{
					digest = MakeError(ErrorCode::Validation, "Test.GetAudioClipDigest needs an AudioClip, but {} is a {}", assets->GetReferencePath(handle),
						AssetTypeToString((*loaded)->GetAssetType()));
					return {};
				}
				digest = ComputeAudioClipDigest(*clip);
				return {};
			}));
			if (!digest)
				return Lua::RaiseError(call, digest.error());
			Lua::PushString(call, std::format("{:016x}", *digest));
			return 1;
		}

		// Test.GetPoseDigest(entity): the digest of the entity's sampled pose (ComputeAnimationPoseDigest of its
		// AnimationPoseComponent's joint model matrices and morph weights, M23; Docs/Decisions/0034-m23-contract.md decision
		// 31), what a suite compares across the Editor, Release and Dist runs; nil without a pose.
		static int GetPoseDigest(ScriptCall& call)
		{
			const auto identity = Lua::Check<ScriptEntityIdentity>(call, 1);
			IScriptHost& host = call.Engine->GetHost();
			Checked(call, ScriptProxy::ValidateEntityForRead(host, identity));
			const ConstEntity entity = host.GetScene().FindReadableEntityByID(identity.ID);
			const AnimationPoseComponent* pose = entity.TryGetComponent<AnimationPoseComponent>();
			if (pose == nullptr)
			{
				Lua::PushNil(call);
				return 1;
			}
			Lua::PushString(call, std::format("{:016x}", ComputeAnimationPoseDigest(pose->JointModel, pose->Morphs)));
			return 1;
		}

		// Test.GetParticleDigest(entity): the seeded particle hash of the entity's ParticleEmitter (FindParticleDigest, M26;
		// Docs/Decisions/0037-m26-contract.md decisions 5 and 26), what a suite compares across the Editor, Release and Dist
		// runs; nil before its first Particles phase or without a ParticleEmitter.
		static int GetParticleDigest(ScriptCall& call)
		{
			const auto identity = Lua::Check<ScriptEntityIdentity>(call, 1);
			IScriptHost& host = call.Engine->GetHost();
			Checked(call, ScriptProxy::ValidateEntityForRead(host, identity));
			const std::optional<uint64_t> digest = FindParticleDigest(host.GetScene().FindReadableEntityByID(identity.ID));
			if (!digest.has_value())
			{
				Lua::PushNil(call);
				return 1;
			}
			Lua::PushString(call, std::format("{:016x}", *digest));
			return 1;
		}

		static int GetScriptErrors(ScriptCall& call)
		{
			uint64_t since = 0;
			if (!Lua::IsNoneOrNil(call, 1))
			{
				const double value = Lua::Check<double>(call, 1);
				if (value < 0 || value > 9007199254740991.0 || std::floor(value) != value)
					return Lua::RaiseError(call, "error cursor must be an exactly representable nonnegative integer");
				since = static_cast<uint64_t>(value);
			}
			const auto errors = call.Engine->GetErrors().Read(since, std::numeric_limits<uint32_t>::max());
			lua_createtable(call.State, 0, 0);
			int index = 1;
			for (const auto& error : errors)
			{
				lua_createtable(call.State, 0, 14);
				const auto string = [&call](const char* name, std::string_view value)
				{
					Lua::PushString(call, value);
					lua_setfield(call.State, -2, name);
				};
				const auto number = [&call](const char* name, uint64_t value)
				{
					Lua::Push(call, static_cast<double>(value));
					lua_setfield(call.State, -2, name);
				};
				number("id", error.ID);
				string("kind", ScriptErrorKindToString(error.Kind));
				string("script", error.Script);
				number("line", error.Line);
				number("column", error.Column);
				string("message", error.Message);
				string("callback", error.Callback);
				string("entity", error.Entity.ToString());
				string("entityName", error.EntityName);
				number("tick", error.Tick);
				number("count", error.Count);
				string("jsonPointer", error.JsonPointer);
				lua_createtable(call.State, 0, 0);
				int frameIndex = 1;
				for (const auto& frame : error.Traceback)
				{
					lua_createtable(call.State, 0, 3);
					string("script", frame.Script);
					number("line", frame.Line);
					string("function", frame.Function);
					lua_rawseti(call.State, -2, frameIndex++);
				}
				lua_setfield(call.State, -2, "traceback");
				lua_rawseti(call.State, -2, index++);
			}
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterTest(ScriptApiRegistry& api)
		{
			ENGINE_TRY(api.RegisterAlias("TestSuiteOptions", "{CaseTimeoutTicks: number?}", "Default positive tick timeout for cases in a suite."));
			ENGINE_TRY(api.RegisterAlias("TestCaseOptions", "{TimeoutTicks: number?}", "Positive tick timeout overriding the suite default."));
			ENGINE_TRY(api.RegisterAlias("TestSuite", "{Name: string, Body: () -> (), Options: TestSuiteOptions}", "Authenticated suite constructor result."));
			ENGINE_TRY(api.RegisterAlias("TestAudioLevels", "{RmsLeft: number, RmsRight: number, Peak: number}", "Stereo RMS and absolute peak of the requested simulation sample interval."));
			ENGINE_TRY(api.RegisterAlias("TestExtraction", "{Alpha: number, Frame: number}", "The last completed render extraction: its interpolation Alpha, and Frame, the number of extractions completed so far."));
			ENGINE_TRY(api.RegisterAlias("TestMusic",
				"{Voice: string, Clip: AssetRef, Intro: AssetRef?, InIntro: boolean, CursorFrames: number, LengthFrames: number, FadeVolume: number, FadingOut: number}",
				"The session's music voice as of the last AudioUpdate phase: its engine handle as 16 hexadecimal digits, its clip, the intro "
				"it started with and whether it still plays it, its position and length in the frames of the clip it plays (the intro's "
				"while InIntro), its level and the music voices still fading out."));
			ENGINE_TRY(api.RegisterAlias("ScriptError", "{id: number, kind: string, script: string, line: number, column: number, message: string, callback: string, entity: string, entityName: string, tick: number, count: number, jsonPointer: string, traceback: {{script: string, line: number, [\"function\"]: string}}}", "Owned script diagnostic with exclusive occurrence cursor."));
			ScriptMemberOptions pure;
			pure.Environments = ScriptApiEnvironment::Test;
			pure.Mutates = false;
			ScriptMemberOptions mutation = pure;
			mutation.Mutates = true;
			ScriptMemberOptions all = pure;
			all.Environments = ScriptApiEnvironment::All;
			ScriptMemberOptions editor = mutation;
			editor.Modes = RunModes::EditorOnly;
			// String-enum parameters (§11.4): typed in Engine.d.luau, validated before the callback and counted per value.
			// Editor and Runtime registries also reflect the identical automation enum; Freeze merges identical definitions.
			EnumInfo states("InputEventState", "The state of a button-like input event (§13.6).");
			states.AddEntry({ std::string(Utils::InputEventStates[0]), 0, "Pressed on its tick." });
			states.AddEntry({ std::string(Utils::InputEventStates[1]), 1, "Released on its tick." });
			states.AddEntry({ std::string(Utils::InputEventStates[2]), 2, "Pressed on its tick and released on the next, so both edges are observed." });
			ENGINE_TRY(api.RegisterEnum(states));
			ScriptMemberOptions action = mutation;
			action.EnumParameters = { { .ArgumentIndex = 2, .EnumName = "InputEventState", .Optional = true } };
			ScriptMemberOptions key = mutation;
			key.EnumParameters = { { .ArgumentIndex = 1, .EnumName = "Key" }, { .ArgumentIndex = 2, .EnumName = "InputEventState" } };
			ScriptMemberOptions mouse = mutation;
			mouse.EnumParameters = { { .ArgumentIndex = 1, .EnumName = "MouseButton" }, { .ArgumentIndex = 2, .EnumName = "InputEventState" } };
			ScriptMemberOptions gamepad = mutation;
			gamepad.EnumParameters = {
				{ .ArgumentIndex = 2, .EnumName = "GamepadButton", .SelectorIndex = 3, .Selector = ScriptArgumentType::String },
				{ .ArgumentIndex = 3, .EnumName = "InputEventState", .SelectorIndex = 3, .Selector = ScriptArgumentType::String },
				{ .ArgumentIndex = 2, .EnumName = "GamepadAxis", .SelectorIndex = 3, .Selector = ScriptArgumentType::Number },
			};
			const std::string description = std::format("Feature test suites, assertions, waits and deterministic input. A reported message shows authored locations "
														"and keeps at most {} bytes, truncated and marked like a log entry. A failed Test.ExpectEqual or Test.ExpectNear "
														"adds its values after the message, which is cut first so they always fit: nil, booleans and numbers (shortest "
														"round-trip form) as Luau writes them, strings quoted (their first {} bytes, then their length), vectors, Colors and "
														"Quats by component, Entities and AssetRefs by UUID and other values by type name. A case's report keeps at most {} "
														"failures for expectations and unexpected script errors, the last then counting any further ones as [truncated: "
														"<n> more failures], and a suite at most {} such failures. Test.Inject* queues at most {} events per input tick.",
				MaxScriptLogMessageBytes, Utils::MaxExpectationStringBytes, MaxScriptTestFailuresPerCase, MaxScriptTestFailuresPerSuite, MaxScriptInjectedInputsPerTick);
			api.Module("Test", description)
				.Function("Suite", Detail::CreateTestSuite, "(name: string, body: () -> (), options: TestSuiteOptions?) -> TestSuite",
					std::format("Construct a suite without executing its body; the name is UTF-8 without NUL and at most {} bytes.", MaxScriptTestNameBytes), all)
				.Function("Case", Utils::Case, "(name: string, fn: () -> (), options: TestCaseOptions?) -> ()",
					std::format("Register a case while collecting the suite body; the name is nonempty, unique in the suite and at most {} bytes, and a suite has at most {} cases.", MaxScriptTestNameBytes, MaxScriptTestCasesPerSuite), pure)
				.Function("Expect", Utils::Expect, "(condition: boolean, message: string?) -> ()", "Record a failed condition and continue the case.", pure)
				.Function("ExpectEqual", Utils::ExpectEqual, "(actual: any, expected: any, message: string?) -> ()",
					"Record a failed Luau equality comparison of actual with expected. The failure shows the message (default \"expected values to be equal\") "
					"followed by (got <actual>, expected <expected>).",
					pure)
				.Function("ExpectNear", Utils::ExpectNear, "(actual: number | vector, expected: number | vector, epsilon: number?, message: string?) -> ()",
					"Compare finite numbers or vector components with an absolute epsilon, default 0.00001. The failure shows the message (default \"values "
					"differ by more than epsilon\") followed by (got <actual>, expected <expected>, epsilon <epsilon>).",
					pure)
				.Function("Fail", Utils::Fail, "(message: string) -> ()", "End this case as failed. A case thread, or a task a behaviour spawned (which is cancelled), never runs past the call, even inside pcall; a lifecycle callback cannot yield, so there the call raises a script error that unwinds to the nearest pcall. The outcome stays latched either way.", pure)
				.Function("Skip", Utils::Skip, "(reason: string) -> ()", "End this case as skipped without erasing earlier failures; it stops its caller the way Test.Fail does, and the outcome stays latched.", pure)
				.Function("WaitTicks", Utils::WaitTicks, "(ticks: number) -> ()", "Yield a test thread for simulation ticks.", pure)
				.Function("WaitUntil", Utils::WaitUntil, "(predicate: () -> boolean, timeoutTicks: number) -> ()",
					"Evaluate the predicate now and after every following tick until it returns true; it must return a boolean (anything else is a "
					"script error). Still false after timeoutTicks ticks, the case fails with \"Test.WaitUntil exceeded its tick limit\" at this call.",
					pure)
				.Function("ExpectScriptError", Utils::ExpectScriptError, "(pattern: string, withinTicks: number) -> ()", std::format("Claim one nonfatal error occurrence in the current case using a Luau message pattern of at most {} bytes; the case keeps at most {} unclaimed errors to claim.", MaxScriptErrorPatternBytes, MaxScriptTestUnclaimedErrors), pure)
				.Function("CaptureAudio", Utils::CaptureAudio, "(ticks: number) -> TestAudioLevels", "Capture exactly the requested current-and-following ticks; yield until grouped audio is ready.", mutation)
				.Function("InjectAction", Utils::InjectAction, "(name: string, state: InputEventState?, value: number?) -> ()",
					"Queue a named button state or analog value (with a nil or Down state) for the next unapplied input tick. A button stays down until an Up "
					"(Tap releases it on the next tick), and an analog value holds on every later tick until another value is injected for the action "
					"(0 releases it).",
					action)
				.Function("InjectKey", Utils::InjectKey, "(key: Key, state: InputEventState) -> ()", "Queue a key state for the next unapplied input tick.", key)
				.Function("InjectMouse", Utils::InjectMouse, "(button: MouseButton, state: InputEventState, position: vector?) -> ()", "Queue a mouse button and optional finite XY position.", mouse)
				.Function("InjectGamepad", Utils::InjectGamepad, "((index: number, button: GamepadButton, state: InputEventState) -> ()) & ((index: number, axis: GamepadAxis, value: number) -> ())",
					"Queue a gamepad button state, or an engine-convention axis value when the third argument is a number.", gamepad)
				.Function("InjectMouseMove", Utils::InjectMouseMove, "(position: vector) -> ()", "Queue a mouse move to a finite XY position in view coordinates for the next unapplied input tick.", mutation)
				.Function("InjectScroll", Utils::InjectScroll, "(delta: vector) -> ()", "Queue a scroll of a finite XY delta (wheel steps, y up-positive) for the next unapplied input tick.", mutation)
				.Function("InjectText", Utils::InjectText, "(text: string) -> ()", "Queue committed text (non-empty UTF-8) for the next unapplied input tick, as typed characters.", mutation)
				.Function("InjectSignal", Utils::InjectSignal, "(name: string) -> ()",
					"Queue the frame-time signal name (1 to 64 bytes of UTF-8) for the next unapplied input tick, where Input.WasSignalled reads it: "
					"how a test takes the decisions a live session posts with Input.PostSignal, which test runs ignore.",
					mutation)
				.Function("Screenshot", Utils::Screenshot, "(name: string) -> ()",
					std::format("Capture a screenshot through the owning host; a suite takes at most {} screenshots, and a later call raises an error without capturing.", MaxScriptTestScreenshotsPerSuite), mutation)
				.Function("ReloadScript", Utils::ReloadScript, "(asset: AssetRef) -> ()", "Synchronously reload a script in an editor test.", editor)
				.Function("GetStateHash", Utils::GetStateHash, "() -> string", "Read the canonical simulated state hash: the world (every entity with its serialized components, the tick, the session's Random state, the id draws and every body's physics state), never script tables, audio or input. It includes the tick, so two reads at different ticks always differ: compare two runs with input.replay's stateHash instead.", pure)
				.Function("GetLastExtraction", Utils::GetLastExtraction, "() -> TestExtraction", "Read the last completed render extraction.", pure)
				.Function("GetExtractedPosition", Utils::GetExtractedPosition, "(entity: Entity) -> vector?", "Read an entity's position in the last render snapshot.", pure)
				.Function("GetScriptErrors", Utils::GetScriptErrors, "(since: number?) -> {ScriptError}", std::format("Read the current scene's VM's error occurrences newer than an exclusive error cursor; claimed errors remain visible. The VM keeps the {} distinct errors with the latest occurrences. A scene transition (Scene.Load, Scene.Reload) replaces the VM, so the next scene's list starts empty: an error the previous scene raised, in the transition's frame too, is read only by a case that ran in that scene (the case that requested the transition reads those raised before it returned), and the suite's scriptErrors count in the run's report counts it.", MaxScriptErrorEntries), pure)
				.Function("GetAudioClipDigest", Utils::GetAudioClipDigest, "(clip: AssetRef) -> string",
					"Returns the XXH64 of an audio clip's samples decoded to 32-bit float at its own rate, as 16 hexadecimal digits; test suites only.", pure)
				.Function("GetPoseDigest", Utils::GetPoseDigest, "(entity: Entity) -> string?",
					"Returns the digest of an Animator entity's pose sampled in the last fixed step (the XXH64 of its joint model matrices and morph "
					"weights) as 16 hexadecimal digits, or nil without a pose; the same in every configuration, so a suite compares it across runs; "
					"test suites only.",
					pure)
				.Function("GetParticleDigest", Utils::GetParticleDigest, "(entity: Entity) -> string?",
					"Returns the seeded particle hash of a ParticleEmitter entity (the XXH64 of its live particles, random stream and emission "
					"state) as 16 hexadecimal digits, or nil before its first Particles phase or without a ParticleEmitter; the same in every "
					"configuration, so a suite compares it across runs; test suites only.",
					pure)
				.Function("SetSaveData", Utils::SetSaveData, "(values: {[string]: any}) -> ()",
					std::format("Replaces the session's save data with values (keys of 1 to {} bytes, values SaveData.Set takes), as a game "
								"finds it at its start: how a suite seeds the store a test run keeps in memory, empty until it does. Each case of an "
								"Isolation \"Case\" suite starts empty again; a case that seeds the store and calls Scene.Reload() returns in that "
								"tick, and a later case of an Isolation \"Suite\" suite sees the reloaded scene read it.",
						MaxSaveDataKeyBytes),
					mutation)
				.Function("GetMusic", Utils::GetMusic, "() -> TestMusic?",
					"Returns the session's music voice as of the last AudioUpdate phase (Audio.PlayMusic), or nil without one (no music, or "
					"a clip played once that ended): Voice, the engine's voice handle as 16 hexadecimal digits, the same while the voice is "
					"kept, so a suite tells a track that plays on through Scene.Load from a restart; Clip; Intro, the intro the voice "
					"started with (nil for none), and InIntro, whether it still plays it; CursorFrames and LengthFrames, in the frames of "
					"the clip it plays (the intro's while InIntro); FadeVolume, the voice's level, a multiplier of its Volume; and "
					"FadingOut, the music voices still fading out. Deterministic, since a test run owns audio time; test suites only. "
					"Raises \"audio is unavailable\" outside a play session.",
					pure);
			return {};
		}

	}

}
