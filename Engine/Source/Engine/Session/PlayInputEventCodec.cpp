#include "EnginePCH.h"
#include "Engine/Session/PlayInputEventCodec.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/FuzzySuggest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <format>

namespace Engine {

	namespace {

		constexpr std::array<std::string_view, 9> Types = { "Action", "Key", "MouseButton", "MouseMove", "MouseDelta", "Scroll", "GamepadButton", "GamepadAxis", "Text" };
		constexpr std::array<std::string_view, 3> States = { "Down", "Up", "Tap" };

		bool EqualName(std::string_view a, std::string_view b)
		{
			const auto lower = [](char c)
			{
				return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
			};
			return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [lower](char x, char y)
			{
				return lower(x) == lower(y);
			});
		}

		template<typename Code, typename Parse, typename Print>
		Result<Code> Control(const JsonReader& reader, Parse parse, Print print, size_t count)
		{
			ENGINE_TRY_ASSIGN(auto name, reader.ReadString());
			if (const auto code = parse(name))
				return *code;
			std::vector<std::string> names;
			for (size_t i = 0; i < count; ++i)
			{
				auto value = print(static_cast<Code>(i));
				if (!value.empty())
					names.emplace_back(value);
			}
			auto hint = MakeDidYouMeanHint(FuzzySuggest(name, names));
			if (hint.empty())
				hint = "use a named input control from the input reference";
			return std::unexpected(reader.MakeLocatedError(ErrorCode::InvalidArgument, std::format("unknown input control '{}'", name)).WithHint(std::move(hint)));
		}

		Result<StampedPlayInputEvent> Parse(const Json& value, const InputActionMap& actions, std::string_view pointer, bool allowTick)
		{
			JsonReader root(value, std::string(pointer));
			ENGINE_TRY(root.ExpectType(JsonType::Object));
			StampedPlayInputEvent stamped;
			auto& e = stamped.Event;
			ENGINE_TRY_ASSIGN(auto type, root.ReadMember<std::string>("type"));
			const auto found = std::find_if(Types.begin(), Types.end(), [&type](auto name)
			{
				return EqualName(type, name);
			});
			if (found == Types.end())
				return std::unexpected(root.MakeLocatedError(ErrorCode::InvalidArgument, "unknown input event type").WithHint("use action, key, mouseButton, mouseMove, mouseDelta, scroll, gamepadButton, gamepadAxis or text"));
			e.Type = static_cast<PlayInputEventType>(found - Types.begin());
			std::vector<std::string_view> allowed{ "type" };
			if (allowTick)
			{
				allowed.push_back("tick");
				if (const auto tick = root.FindMember("tick"))
				{
					ENGINE_TRY_ASSIGN(stamped.Tick, tick->ReadUInt64());
				}
			}
			const auto state = [&]() -> Status
			{
				allowed.push_back("state");
				if (auto member = root.FindMember("state"))
				{
					ENGINE_TRY_ASSIGN(auto name, member->ReadString());
					const auto match = std::find_if(States.begin(), States.end(), [&name](auto item)
					{
						return EqualName(name, item);
					});
					if (match == States.end())
						return std::unexpected(member->MakeLocatedError(ErrorCode::InvalidArgument, "expected down, up or tap").WithHint("use down, up or tap"));
					e.State = static_cast<PlayInputEventState>(match - States.begin());
				}
				return {};
			};
			const auto vector = [&](std::string_view key, glm::vec2& out) -> Status
			{
				allowed.push_back(key);
				ENGINE_TRY_ASSIGN(auto member, root.GetMember(key));
				ENGINE_TRY_ASSIGN(size_t count, member.GetArraySize());
				if (count != 2)
					return std::unexpected(member.MakeLocatedError(ErrorCode::InvalidArgument, "expected two finite coordinates"));
				ENGINE_TRY_ASSIGN(auto x, member.GetElement(0));
				ENGINE_TRY_ASSIGN(auto y, member.GetElement(1));
				ENGINE_TRY_ASSIGN(out.x, x.ReadFloat());
				ENGINE_TRY_ASSIGN(out.y, y.ReadFloat());
				return {};
			};
			if (e.Type == PlayInputEventType::Action)
			{
				allowed.push_back("name");
				ENGINE_TRY_ASSIGN(e.Name, root.ReadMember<std::string>("name"));
				if (auto member = root.FindMember("value"))
				{
					allowed.push_back("value");
					e.HasValue = true;
					ENGINE_TRY_ASSIGN(e.Value, member->ReadFloat());
				}
				else
					ENGINE_TRY(state());
			}
			else if (e.Type == PlayInputEventType::KeyInput)
			{
				allowed.push_back("key");
				ENGINE_TRY_ASSIGN(auto member, root.GetMember("key"));
				ENGINE_TRY_ASSIGN(e.KeyCode, Control<Key>(member, KeyFromString, KeyToString, KeyCodeCount));
				ENGINE_TRY(state());
			}
			else if (e.Type == PlayInputEventType::MouseButtonInput)
			{
				allowed.push_back("button");
				ENGINE_TRY_ASSIGN(auto member, root.GetMember("button"));
				ENGINE_TRY_ASSIGN(e.Button, Control<MouseButton>(member, MouseButtonFromString, MouseButtonToString, MouseButtonCount));
				ENGINE_TRY(state());
				if (root.HasMember("position"))
				{
					ENGINE_TRY(vector("position", e.Position));
					e.HasPosition = true;
				}
			}
			else if (e.Type == PlayInputEventType::MouseMove)
				ENGINE_TRY(vector("position", e.Position));
			else if (e.Type == PlayInputEventType::MouseDelta || e.Type == PlayInputEventType::Scroll)
				ENGINE_TRY(vector("delta", e.Delta));
			else if (e.Type == PlayInputEventType::Text)
			{
				allowed.push_back("text");
				ENGINE_TRY_ASSIGN(e.Text, root.ReadMember<std::string>("text"));
			}
			else
			{
				allowed.push_back("gamepad");
				if (auto member = root.FindMember("gamepad"))
				{
					ENGINE_TRY_ASSIGN(e.Gamepad, member->ReadUInt32());
				}
				if (e.Type == PlayInputEventType::GamepadButtonInput)
				{
					allowed.push_back("button");
					ENGINE_TRY_ASSIGN(auto member, root.GetMember("button"));
					ENGINE_TRY_ASSIGN(e.GamepadButtonCode, Control<GamepadButton>(member, GamepadButtonFromString, GamepadButtonToString, GamepadButtonCount));
					ENGINE_TRY(state());
				}
				else
				{
					allowed.push_back("axis");
					allowed.push_back("value");
					ENGINE_TRY_ASSIGN(auto member, root.GetMember("axis"));
					ENGINE_TRY_ASSIGN(e.Axis, Control<GamepadAxis>(member, GamepadAxisFromString, GamepadAxisToString, GamepadAxisCount));
					ENGINE_TRY_ASSIGN(e.Value, root.ReadMember<float>("value"));
				}
			}
			ENGINE_TRY_ASSIGN(auto extra, root.FindUnknownMembers(allowed));
			if (!extra.empty())
				return std::unexpected(Error(ErrorCode::InvalidArgument, std::format("a {} event has no member '{}'", type, extra.front())).WithLocation({ .File = {}, .JsonPointer = JsonReader::AppendPointer(pointer, extra.front()), .Entity = {} }).WithHint("remove the member; each event type accepts only its own fields"));
			const auto valid = ValidatePlayInputEvent(e, actions);
			if (!valid)
			{
				ErrorLocation location = valid.error().GetLocation();
				location.JsonPointer = std::string(pointer) + location.JsonPointer.value_or("");
				return std::unexpected(Error(valid.error()).WithLocation(std::move(location)));
			}
			return stamped;
		}

	}

	Result<StampedPlayInputEvent> ParsePlayInputEvent(const Json& value, const InputActionMap& actions, std::string_view pointer, bool allowTick)
	{
		auto result = Parse(value, actions, pointer, allowTick);
		if (!result && result.error().GetCode() != ErrorCode::InvalidArgument)
			return std::unexpected(Error(ErrorCode::InvalidArgument, result.error().GetMessageText()).WithLocation(result.error().GetLocation()).WithHint("use the input event schema and finite values"));
		return result;
	}

	Result<StampedPlayInputEvent> DecodeReplayEvent(const ReplayEvent& e, const InputActionMap& actions, std::string_view pointer)
	{
		Json value = { { "tick", e.Tick }, { "type", e.Type } };
		if (e.Type == "Action" || !e.Name.empty())
			value["name"] = e.Name;
		if (e.State)
			value["state"] = *e.State;
		if (e.Value)
			value["value"] = *e.Value;
		if (e.Type == "Key" || !e.KeyName.empty())
			value["key"] = e.KeyName;
		if (e.Type == "MouseButton" || e.Type == "GamepadButton" || !e.ButtonName.empty())
			value["button"] = e.ButtonName;
		if (e.Type == "GamepadButton" || e.Type == "GamepadAxis" || e.Gamepad != 0)
			value["gamepad"] = e.Gamepad;
		if (e.Type == "GamepadAxis" || !e.AxisName.empty())
			value["axis"] = e.AxisName;
		if (e.Position)
			value["position"] = { e.Position->x, e.Position->y };
		if (e.Delta)
			value["delta"] = { e.Delta->x, e.Delta->y };
		if (e.Type == "Text" || !e.Text.empty())
			value["text"] = e.Text;
		return ParsePlayInputEvent(value, actions, pointer);
	}

	Result<ReplayEvent> EncodeReplayEvent(uint64_t tick, const PlayInputEvent& e)
	{
		const auto kind = static_cast<size_t>(e.Type);
		const auto state = static_cast<size_t>(e.State);
		if (kind >= Types.size() || state >= States.size())
			return MakeError(ErrorCode::InvalidArgument, "invalid input event tag");
		ReplayEvent result;
		result.Tick = tick;
		result.Type = Types[kind];
		switch (e.Type)
		{
			case PlayInputEventType::Action:
				result.Name = e.Name;
				if (e.HasValue)
					result.Value = e.Value;
				else
					result.State = States[state];
				break;
			case PlayInputEventType::KeyInput:
				result.KeyName = KeyToString(e.KeyCode);
				result.State = States[state];
				break;
			case PlayInputEventType::MouseButtonInput:
				result.ButtonName = MouseButtonToString(e.Button);
				result.State = States[state];
				if (e.HasPosition)
					result.Position = e.Position;
				break;
			case PlayInputEventType::MouseMove:  result.Position = e.Position; break;
			case PlayInputEventType::MouseDelta:
			case PlayInputEventType::Scroll:     result.Delta = e.Delta; break;
			case PlayInputEventType::GamepadButtonInput:
				result.Gamepad = e.Gamepad;
				result.ButtonName = GamepadButtonToString(e.GamepadButtonCode);
				result.State = States[state];
				break;
			case PlayInputEventType::GamepadAxisInput:
				result.Gamepad = e.Gamepad;
				result.AxisName = GamepadAxisToString(e.Axis);
				result.Value = e.Value;
				break;
			case PlayInputEventType::Text: result.Text = e.Text; break;
		}
		// Reuse the Asset validator for fields that do not depend on a project's action map.
		ReplayDocument document;
		document.Header.Scene.Handle = UUID(1);
		document.Header.EngineVersion = "validation";
		document.Header.Config = "validation";
		document.FinalStateHash = "0000000000000000";
		document.FinalTick = 1;
		document.Events.push_back(result);
		document.Events.back().Tick = 0;
		ENGINE_TRY(ValidateReplayDocument(document));
		return result;
	}

}
