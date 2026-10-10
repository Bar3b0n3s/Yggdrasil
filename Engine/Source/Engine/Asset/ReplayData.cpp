#include "EnginePCH.h"
#include "Engine/Asset/ReplayData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/FixedStepScheduler.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Platform/Input/InputState.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <limits>

namespace Engine {

	namespace {

		constexpr std::array<std::string_view, 9> EventTypes = { "Action", "Key", "MouseButton", "MouseMove", "MouseDelta", "Scroll", "GamepadButton", "GamepadAxis", "Text" };
		constexpr std::array<std::string_view, 3> States = { "Down", "Up", "Tap" };

		Error Invalid(std::string pointer, std::string message)
		{
			return Error(ErrorCode::Validation, std::move(message)).WithLocation({ .File = {}, .JsonPointer = std::move(pointer), .Entity = {} });
		}

		Status Text(std::string_view value, std::string pointer, bool required = false)
		{
			if ((required && value.empty()) || value.size() > std::numeric_limits<uint32_t>::max() || !IsValidUtf8(value))
				return std::unexpected(Invalid(std::move(pointer), "expected valid UTF-8 text within the format length limit"));
			return {};
		}

		Status CheckTree(const JsonReader& root)
		{
			std::vector<std::pair<JsonReader, size_t>> pending{ { root, 0 } };
			while (!pending.empty())
			{
				auto [reader, depth] = std::move(pending.back());
				pending.pop_back();
				if (reader.IsArray() || reader.IsObject())
				{
					if (depth >= MaxJsonDepth)
						return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation, "JSON nesting limit exceeded"));
					if (reader.IsObject())
					{
						ENGINE_TRY_ASSIGN(auto names, reader.GetMemberNames());
						for (const auto& name : names)
						{
							ENGINE_TRY(Text(name, reader.GetPointer()));
							ENGINE_TRY_ASSIGN(auto child, reader.GetMember(name));
							pending.emplace_back(std::move(child), depth + 1);
						}
					}
					else
					{
						ENGINE_TRY_ASSIGN(size_t count, reader.GetArraySize());
						for (size_t i = 0; i < count; ++i)
						{
							ENGINE_TRY_ASSIGN(auto child, reader.GetElement(i));
							pending.emplace_back(std::move(child), depth + 1);
						}
					}
				}
				else if (reader.GetType() == JsonType::Float)
				{
					ENGINE_TRY(reader.ReadFloat());
				}
				else if (reader.GetType() == JsonType::String)
				{
					ENGINE_TRY_ASSIGN(auto value, reader.ReadString());
					ENGINE_TRY(Text(value, reader.GetPointer()));
				}
			}
			return {};
		}

		Status Unknown(const JsonReader& reader, std::span<const std::string_view> keys, ReplayLoadReport& report, bool strict)
		{
			ENGINE_TRY_ASSIGN(auto unknown, reader.FindUnknownMembers(keys));
			for (const auto& key : unknown)
			{
				const auto pointer = JsonReader::AppendPointer(reader.GetPointer(), key);
				const auto message = std::format("unknown replay member '{}'", key);
				report.Diagnostics.push_back({ strict ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning, {}, pointer, message, {}, {} });
				if (strict)
					return std::unexpected(Invalid(pointer, message));
			}
			return {};
		}

		Json EventJson(const ReplayEvent& e)
		{
			Json j = { { "Tick", e.Tick }, { "Type", e.Type } };
			if (e.Type == "Action" || !e.Name.empty())
				j["Name"] = e.Name;
			if (e.State)
				j["State"] = *e.State;
			if (e.Value)
				j["Value"] = *e.Value;
			if (e.Type == "Key" || !e.KeyName.empty())
				j["Key"] = e.KeyName;
			if (e.Type == "MouseButton" || e.Type == "GamepadButton" || !e.ButtonName.empty())
				j["Button"] = e.ButtonName;
			if (e.Type == "GamepadButton" || e.Type == "GamepadAxis" || e.Gamepad != 0)
				j["Gamepad"] = e.Gamepad;
			if (e.Type == "GamepadAxis" || !e.AxisName.empty())
				j["Axis"] = e.AxisName;
			if (e.Position)
				j["Position"] = { e.Position->x, e.Position->y };
			if (e.Delta)
				j["Delta"] = { e.Delta->x, e.Delta->y };
			if (e.Type == "Text" || !e.Text.empty())
				j["Text"] = e.Text;
			return j;
		}

		Result<ReplayEvent> ReadEvent(const JsonReader& r)
		{
			ReplayEvent e;
			ENGINE_TRY_ASSIGN(e.Tick, r.ReadMember<uint64_t>("Tick"));
			ENGINE_TRY_ASSIGN(e.Type, r.ReadMember<std::string>("Type"));
			if (std::find(EventTypes.begin(), EventTypes.end(), e.Type) == EventTypes.end())
				return std::unexpected(Invalid(r.GetPointer() + "/Type", "unknown or noncanonical replay event type"));
			std::vector<std::string_view> allowed{ "Tick", "Type" };
			const auto string = [&r, &allowed](std::string_view key, std::string& out) -> Status
			{
				allowed.push_back(key);
				ENGINE_TRY_ASSIGN(out, r.ReadMember<std::string>(key));
				return Text(out, JsonReader::AppendPointer(r.GetPointer(), key), true);
			};
			const auto vector = [&r, &allowed](std::string_view key, std::optional<glm::vec2>& out) -> Status
			{
				allowed.push_back(key);
				ENGINE_TRY_ASSIGN(auto v, r.GetMember(key));
				ENGINE_TRY_ASSIGN(size_t count, v.GetArraySize());
				if (count != 2)
					return std::unexpected(v.MakeLocatedError(ErrorCode::Validation, "expected two finite coordinates"));
				ENGINE_TRY_ASSIGN(auto x, v.GetElement(0));
				ENGINE_TRY_ASSIGN(auto y, v.GetElement(1));
				ENGINE_TRY_ASSIGN(float xv, x.ReadFloat());
				ENGINE_TRY_ASSIGN(float yv, y.ReadFloat());
				out = glm::vec2(xv, yv);
				return {};
			};
			if (e.Type == "Action")
				ENGINE_TRY(string("Name", e.Name));
			if (e.Type == "Key")
			{
				ENGINE_TRY(string("Key", e.KeyName));
				const auto code = KeyFromString(e.KeyName);
				if (!code || *code == Key::None || KeyToString(*code) != e.KeyName)
					return std::unexpected(Invalid(r.GetPointer() + "/Key", "unknown or noncanonical key"));
			}
			if (e.Type == "MouseButton" || e.Type == "GamepadButton")
			{
				ENGINE_TRY(string("Button", e.ButtonName));
				const auto mouse = MouseButtonFromString(e.ButtonName);
				const auto pad = GamepadButtonFromString(e.ButtonName);
				if ((e.Type == "MouseButton" && (!mouse || MouseButtonToString(*mouse) != e.ButtonName)) || (e.Type == "GamepadButton" && (!pad || GamepadButtonToString(*pad) != e.ButtonName)))
					return std::unexpected(Invalid(r.GetPointer() + "/Button", "unknown or noncanonical button"));
			}
			if (e.Type == "GamepadAxis")
			{
				ENGINE_TRY(string("Axis", e.AxisName));
				const auto axis = GamepadAxisFromString(e.AxisName);
				if (!axis || GamepadAxisToString(*axis) != e.AxisName)
					return std::unexpected(Invalid(r.GetPointer() + "/Axis", "unknown or noncanonical axis"));
			}
			if (e.Type == "GamepadAxis" || e.Type == "GamepadButton")
			{
				allowed.push_back("Gamepad");
				ENGINE_TRY_ASSIGN(e.Gamepad, r.ReadMember<uint32_t>("Gamepad"));
				if (e.Gamepad >= MaxGamepads)
					return std::unexpected(Invalid(r.GetPointer() + "/Gamepad", "gamepad index out of range"));
			}
			if (e.Type == "GamepadAxis" || (e.Type == "Action" && r.HasMember("Value")))
			{
				allowed.push_back("Value");
				ENGINE_TRY_ASSIGN(float value, r.ReadMember<float>("Value"));
				const float minimum = e.Type == "GamepadAxis" && (e.AxisName == "LeftTrigger" || e.AxisName == "RightTrigger") ? 0.0f : -1.0f;
				if (value < minimum || value > 1.0f)
					return std::unexpected(Invalid(r.GetPointer() + "/Value", "input value out of range"));
				e.Value = value;
			}
			else if (e.Type == "Action" || e.Type == "Key" || e.Type == "MouseButton" || e.Type == "GamepadButton")
			{
				allowed.push_back("State");
				ENGINE_TRY_ASSIGN(auto state, r.ReadMember<std::string>("State"));
				if (std::find(States.begin(), States.end(), state) == States.end())
					return std::unexpected(Invalid(r.GetPointer() + "/State", "expected Down, Up or Tap"));
				e.State = std::move(state);
			}
			if (e.Type == "MouseMove" || (e.Type == "MouseButton" && r.HasMember("Position")))
				ENGINE_TRY(vector("Position", e.Position));
			if (e.Type == "MouseDelta" || e.Type == "Scroll")
				ENGINE_TRY(vector("Delta", e.Delta));
			if (e.Type == "Text")
				ENGINE_TRY(string("Text", e.Text));
			ENGINE_TRY_ASSIGN(auto extra, r.FindUnknownMembers(allowed));
			if (!extra.empty())
				return std::unexpected(Invalid(JsonReader::AppendPointer(r.GetPointer(), extra.front()), "member is not valid for this event type"));
			return e;
		}

		void WriteEvent(BinaryWriter& w, const ReplayEvent& e)
		{
			w.WriteU64(e.Tick);
			w.WriteU8(static_cast<uint8_t>(std::find(EventTypes.begin(), EventTypes.end(), e.Type) - EventTypes.begin()));
			const auto state = [&w, &e]()
			{
				w.WriteU8(static_cast<uint8_t>(std::find(States.begin(), States.end(), *e.State) - States.begin()));
			};
			const auto vector = [&w](glm::vec2 v)
			{
				w.WriteF32(v.x);
				w.WriteF32(v.y);
			};
			if (e.Type == "Action")
			{
				w.WriteString(e.Name);
				w.WriteBool(e.Value.has_value());
				if (e.Value)
					w.WriteF32(*e.Value);
				else
					state();
			}
			else if (e.Type == "Key")
			{
				w.WriteString(e.KeyName);
				state();
			}
			else if (e.Type == "MouseButton")
			{
				w.WriteString(e.ButtonName);
				state();
				w.WriteBool(e.Position.has_value());
				if (e.Position)
					vector(*e.Position);
			}
			else if (e.Type == "MouseMove")
				vector(*e.Position);
			else if (e.Type == "MouseDelta" || e.Type == "Scroll")
				vector(*e.Delta);
			else if (e.Type == "GamepadButton")
			{
				w.WriteU32(e.Gamepad);
				w.WriteString(e.ButtonName);
				state();
			}
			else if (e.Type == "GamepadAxis")
			{
				w.WriteU32(e.Gamepad);
				w.WriteString(e.AxisName);
				w.WriteF32(*e.Value);
			}
			else
				w.WriteString(e.Text);
		}

		Result<ReplayEvent> ReadEvent(BinaryReader& r)
		{
			ReplayEvent e;
			ENGINE_TRY_ASSIGN(e.Tick, r.ReadU64());
			ENGINE_TRY_ASSIGN(uint8_t kind, r.ReadU8());
			if (kind >= EventTypes.size())
				return std::unexpected(Invalid("/Events", "invalid event tag"));
			e.Type = EventTypes[kind];
			const auto state = [&r, &e]() -> Status
			{
				ENGINE_TRY_ASSIGN(uint8_t tag, r.ReadU8());
				if (tag >= States.size())
					return std::unexpected(Invalid("/Events", "invalid state tag"));
				e.State = States[tag];
				return {};
			};
			const auto vector = [&r]() -> Result<glm::vec2>
			{
				ENGINE_TRY_ASSIGN(float x, r.ReadF32());
				ENGINE_TRY_ASSIGN(float y, r.ReadF32());
				return glm::vec2(x, y);
			};
			if (kind == 0)
			{
				ENGINE_TRY_ASSIGN(e.Name, r.ReadString());
				ENGINE_TRY_ASSIGN(bool hasValue, r.ReadBool());
				if (hasValue)
				{
					ENGINE_TRY_ASSIGN(float value, r.ReadF32());
					e.Value = value;
				}
				else
					ENGINE_TRY(state());
			}
			else if (kind == 1)
			{
				ENGINE_TRY_ASSIGN(e.KeyName, r.ReadString());
				ENGINE_TRY(state());
			}
			else if (kind == 2)
			{
				ENGINE_TRY_ASSIGN(e.ButtonName, r.ReadString());
				ENGINE_TRY(state());
				ENGINE_TRY_ASSIGN(bool hasPosition, r.ReadBool());
				if (hasPosition)
				{
					ENGINE_TRY_ASSIGN(auto value, vector());
					e.Position = value;
				}
			}
			else if (kind == 3)
			{
				ENGINE_TRY_ASSIGN(auto value, vector());
				e.Position = value;
			}
			else if (kind == 4 || kind == 5)
			{
				ENGINE_TRY_ASSIGN(auto value, vector());
				e.Delta = value;
			}
			else if (kind == 6)
			{
				ENGINE_TRY_ASSIGN(e.Gamepad, r.ReadU32());
				ENGINE_TRY_ASSIGN(e.ButtonName, r.ReadString());
				ENGINE_TRY(state());
			}
			else if (kind == 7)
			{
				ENGINE_TRY_ASSIGN(e.Gamepad, r.ReadU32());
				ENGINE_TRY_ASSIGN(e.AxisName, r.ReadString());
				ENGINE_TRY_ASSIGN(float value, r.ReadF32());
				e.Value = value;
			}
			else
			{
				ENGINE_TRY_ASSIGN(e.Text, r.ReadString());
			}
			return e;
		}

	}

	ReplayData::ReplayData()
		: Asset(StaticType)
	{
	}

	Status ValidateReplayDocument(const ReplayDocument& d)
	{
		if (d.Version > ReplayFormatVersion)
			return std::unexpected(Error(ErrorCode::UnsupportedVersion, "unsupported replay version").WithLocation({ .File = {}, .JsonPointer = "/Version", .Entity = {} }));
		if (d.Version != ReplayFormatVersion)
			return std::unexpected(Invalid("/Version", "invalid replay version"));
		if (!d.Header.Scene.Handle.IsValid())
			return std::unexpected(Invalid("/Scene/Handle", "replay needs a valid starting scene asset handle"));
		ENGINE_TRY(Text(d.Header.Scene.Path, "/Scene/Path"));
		ENGINE_TRY(Text(d.Header.EngineVersion, "/EngineVersion", true));
		ENGINE_TRY(Text(d.Header.Config, "/Config", true));
		if (d.Header.FixedHz == 0 || d.Header.FixedHz > FrameLoopConfig::MaxFixedHz)
			return std::unexpected(Invalid("/FixedHz", "fixed rate out of range"));
		if (!d.Header.Parameters.IsNull())
		{
			JsonReader parameters(d.Header.Parameters.Get(), "/Parameters");
			ENGINE_TRY(parameters.ExpectType(JsonType::Object));
			ENGINE_TRY(CheckTree(parameters));
		}
		const auto hash = UUID::FromString(d.FinalStateHash);
		if (!hash || hash->ToString() != d.FinalStateHash)
			return std::unexpected(Invalid("/FinalStateHash", "expected 16 lowercase hexadecimal digits"));
		if (d.Events.size() > std::numeric_limits<uint32_t>::max() || d.Expect.size() > std::numeric_limits<uint32_t>::max())
			return std::unexpected(Invalid("", "replay item count exceeds format limit"));
		uint64_t previous = 0;
		for (size_t i = 0; i < d.Events.size(); ++i)
		{
			const auto& e = d.Events[i];
			const auto pointer = std::format("/Events/{}", i);
			if (e.Tick < previous || e.Tick >= d.FinalTick)
				return std::unexpected(Invalid(pointer + "/Tick", "events must be ordered and precede FinalTick"));
			previous = e.Tick;
			const Json value = EventJson(e);
			ENGINE_TRY(ReadEvent(JsonReader(value, pointer)));
		}
		previous = 0;
		for (size_t i = 0; i < d.Expect.size(); ++i)
		{
			const auto& e = d.Expect[i];
			const auto pointer = std::format("/Expect/{}", i);
			if (e.Tick < previous || e.Tick > d.FinalTick)
				return std::unexpected(Invalid(pointer + "/Tick", "expectations must be ordered and at or before FinalTick"));
			previous = e.Tick;
			ENGINE_TRY(Text(e.Luau, pointer + "/Luau", true));
		}
		return {};
	}

	Result<ReplayDocument> ReplayFromJson(const Json& document, ReplayLoadReport& report, bool strictUnknowns)
	{
		report = {};
		const JsonReader root(document);
		ReplayDocument d;
		ENGINE_TRY_ASSIGN(d.Version, root.ReadFormatHeader(ReplayFormatName, 1, ReplayFormatVersion));
		report.FileVersion = d.Version;
		constexpr std::array<std::string_view, 12> Keys = { "Format", "Version", "Scene", "Parameters", "Seed", "FixedHz", "EngineVersion", "Config", "Events", "Expect", "FinalTick", "FinalStateHash" };
		ENGINE_TRY(Unknown(root, Keys, report, strictUnknowns));
		ENGINE_TRY_ASSIGN(auto scene, root.GetMember("Scene"));
		constexpr std::array<std::string_view, 2> SceneKeys = { "Handle", "Path" };
		ENGINE_TRY(Unknown(scene, SceneKeys, report, strictUnknowns));
		ENGINE_TRY_ASSIGN(d.Header.Scene.Handle, scene.ReadMember<UUID>("Handle"));
		ENGINE_TRY_ASSIGN(d.Header.Scene.Path, scene.ReadMember<std::string>("Path"));
		ENGINE_TRY_ASSIGN(auto parameters, root.GetMember("Parameters"));
		ENGINE_TRY(parameters.ExpectType(JsonType::Object));
		ENGINE_TRY(CheckTree(parameters));
		d.Header.Parameters.Set(parameters.GetValue());
		ENGINE_TRY_ASSIGN(d.Header.Seed, root.ReadMember<uint64_t>("Seed"));
		ENGINE_TRY_ASSIGN(d.Header.FixedHz, root.ReadMember<uint32_t>("FixedHz"));
		ENGINE_TRY_ASSIGN(d.Header.EngineVersion, root.ReadMember<std::string>("EngineVersion"));
		ENGINE_TRY_ASSIGN(d.Header.Config, root.ReadMember<std::string>("Config"));
		ENGINE_TRY_ASSIGN(d.FinalTick, root.ReadMember<uint64_t>("FinalTick"));
		ENGINE_TRY_ASSIGN(d.FinalStateHash, root.ReadMember<std::string>("FinalStateHash"));
		ENGINE_TRY_ASSIGN(auto events, root.GetMember("Events"));
		ENGINE_TRY_ASSIGN(size_t count, events.GetArraySize());
		for (size_t i = 0; i < count; ++i)
		{
			ENGINE_TRY_ASSIGN(auto item, events.GetElement(i));
			ENGINE_TRY_ASSIGN(auto event, ReadEvent(item));
			d.Events.push_back(std::move(event));
		}
		ENGINE_TRY_ASSIGN(auto expectations, root.GetMember("Expect"));
		ENGINE_TRY_ASSIGN(size_t expectationCount, expectations.GetArraySize());
		constexpr std::array<std::string_view, 2> ExpectKeys = { "Tick", "Luau" };
		for (size_t i = 0; i < expectationCount; ++i)
		{
			ENGINE_TRY_ASSIGN(auto item, expectations.GetElement(i));
			ENGINE_TRY(Unknown(item, ExpectKeys, report, strictUnknowns));
			ReplayExpectation e;
			ENGINE_TRY_ASSIGN(e.Tick, item.ReadMember<uint64_t>("Tick"));
			ENGINE_TRY_ASSIGN(e.Luau, item.ReadMember<std::string>("Luau"));
			d.Expect.push_back(std::move(e));
		}
		ENGINE_TRY(ValidateReplayDocument(d));
		return d;
	}

	Result<ReplayDocument> ReplayFromText(std::string_view text, ReplayLoadReport& report, bool strictUnknowns)
	{
		report = {};
		ENGINE_TRY_ASSIGN(auto document, JsonReader::Parse(text));
		return ReplayFromJson(document, report, strictUnknowns);
	}

	Result<Json> ReplayToJson(const ReplayDocument& d)
	{
		ENGINE_TRY(ValidateReplayDocument(d));
		Json j = { { "Format", ReplayFormatName }, { "Version", d.Version }, { "Scene", { { "Handle", d.Header.Scene.Handle.ToString() }, { "Path", d.Header.Scene.Path } } }, { "Parameters", d.Header.Parameters.IsNull() ? Json::object() : d.Header.Parameters.Get() }, { "Seed", d.Header.Seed }, { "FixedHz", d.Header.FixedHz }, { "EngineVersion", d.Header.EngineVersion }, { "Config", d.Header.Config }, { "Events", Json::array() }, { "Expect", Json::array() }, { "FinalTick", d.FinalTick }, { "FinalStateHash", d.FinalStateHash } };
		for (const auto& e : d.Events)
			j["Events"].push_back(EventJson(e));
		for (const auto& e : d.Expect)
			j["Expect"].push_back({ { "Tick", e.Tick }, { "Luau", e.Luau } });
		return j;
	}

	Result<std::string> ReplayToText(const ReplayDocument& document)
	{
		ENGINE_TRY_ASSIGN(auto value, ReplayToJson(document));
		return JsonWriter::Write(value);
	}

	Result<Buffer> CookReplay(const ReplayDocument& d, std::span<const ReplayBytecodeExpectation> expectations, uint32_t importerVersion)
	{
		ENGINE_TRY(ValidateReplayDocument(d));
		if (expectations.size() != d.Expect.size())
			return std::unexpected(Invalid("/Expect", "bytecode expectation count differs from source"));
		BinaryWriter w;
		w.WriteU32(d.Version);
		w.WriteU64(d.Header.Scene.Handle.GetValue());
		w.WriteString(d.Header.Scene.Path);
		ENGINE_TRY_ASSIGN(auto parameters, JsonWriter::Write(d.Header.Parameters.IsNull() ? Json::object() : d.Header.Parameters.Get(), JsonStyle::Minified));
		ENGINE_TRY(Text(parameters, "/Parameters"));
		w.WriteString(parameters);
		w.WriteU64(d.Header.Seed);
		w.WriteU32(d.Header.FixedHz);
		w.WriteString(d.Header.EngineVersion);
		w.WriteString(d.Header.Config);
		w.WriteU32(static_cast<uint32_t>(d.Events.size()));
		for (const auto& e : d.Events)
			WriteEvent(w, e);
		w.WriteU32(static_cast<uint32_t>(expectations.size()));
		for (size_t i = 0; i < expectations.size(); ++i)
		{
			const auto& e = expectations[i];
			if (e.Tick != d.Expect[i].Tick || e.Script.Kind != ScriptKind::Module)
				return std::unexpected(Invalid(std::format("/Expect/{}", i), "expectation must be a Module at its source tick"));
			ENGINE_TRY_ASSIGN(auto script, CookScript(e.Script, importerVersion));
			if (script.size() > std::numeric_limits<uint32_t>::max())
				return std::unexpected(Invalid(std::format("/Expect/{}", i), "compiled expectation exceeds format size limit"));
			w.WriteU64(e.Tick);
			w.WriteU32(static_cast<uint32_t>(script.size()));
			w.WriteBytes(script);
		}
		w.WriteU64(d.FinalTick);
		w.WriteU64(UUID::FromString(d.FinalStateHash)->GetValue());
		return WriteCookedArtifact(ReplayData::StaticType, ReplayData::FormatVersion, importerVersion, w.GetData());
	}

	Result<AssetRef<ReplayData>> LoadCookedReplay(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(auto artifact, ReadCookedArtifact(cooked, ReplayData::StaticType, ReplayData::FormatVersion));
		BinaryReader r(artifact.Payload);
		ReplayDocument d;
		ENGINE_TRY_ASSIGN(d.Version, r.ReadU32());
		ENGINE_TRY_ASSIGN(uint64_t handle, r.ReadU64());
		d.Header.Scene.Handle = UUID(handle);
		ENGINE_TRY_ASSIGN(d.Header.Scene.Path, r.ReadString());
		ENGINE_TRY_ASSIGN(auto parametersText, r.ReadString());
		ENGINE_TRY_ASSIGN(auto parameters, JsonReader::Parse(parametersText));
		ENGINE_TRY(JsonReader(parameters, "/Parameters").ExpectType(JsonType::Object));
		d.Header.Parameters.Set(std::move(parameters));
		ENGINE_TRY_ASSIGN(d.Header.Seed, r.ReadU64());
		ENGINE_TRY_ASSIGN(d.Header.FixedHz, r.ReadU32());
		ENGINE_TRY_ASSIGN(d.Header.EngineVersion, r.ReadString());
		ENGINE_TRY_ASSIGN(d.Header.Config, r.ReadString());
		ENGINE_TRY_ASSIGN(uint32_t count, r.ReadU32());
		if (count > r.GetRemaining() / 9)
			return std::unexpected(Invalid("/Events", "event count exceeds remaining payload"));
		for (uint32_t i = 0; i < count; ++i)
		{
			ENGINE_TRY_ASSIGN(auto e, ReadEvent(r));
			d.Events.push_back(std::move(e));
		}
		ENGINE_TRY_ASSIGN(uint32_t expectationCount, r.ReadU32());
		if (expectationCount > r.GetRemaining() / 12)
			return std::unexpected(Invalid("/Expect", "expectation count exceeds remaining payload"));
		auto result = CreateRef<ReplayData>();
		for (uint32_t i = 0; i < expectationCount; ++i)
		{
			ENGINE_TRY_ASSIGN(uint64_t tick, r.ReadU64());
			ENGINE_TRY_ASSIGN(uint32_t length, r.ReadU32());
			ENGINE_TRY_ASSIGN(auto bytes, r.ReadBytes(length));
			ENGINE_TRY_ASSIGN(auto script, LoadCookedScript(bytes));
			if (script->Kind != ScriptKind::Module)
				return std::unexpected(Invalid(std::format("/Expect/{}", i), "expectation bytecode is not a Module"));
			result->Expect.push_back({ tick, *script });
			d.Expect.push_back({ tick, "compiled" });
		}
		ENGINE_TRY_ASSIGN(d.FinalTick, r.ReadU64());
		ENGINE_TRY_ASSIGN(uint64_t hash, r.ReadU64());
		d.FinalStateHash = UUID(hash).ToString();
		if (!r.IsAtEnd())
			return std::unexpected(Invalid("", "trailing replay payload bytes"));
		ENGINE_TRY(ValidateReplayDocument(d));
		result->Header = std::move(d.Header);
		result->Events = std::move(d.Events);
		result->FinalTick = d.FinalTick;
		result->FinalStateHash = std::move(d.FinalStateHash);
		return AssetRef<ReplayData>(std::move(result));
	}

}
