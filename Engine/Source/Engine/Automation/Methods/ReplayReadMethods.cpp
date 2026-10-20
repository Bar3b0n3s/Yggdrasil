#include "EnginePCH.h"
#include "Engine/Automation/Methods/ReplayMethods.h"

#include "Engine/Asset/ReplayData.h"
#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <string_view>

// input.readReplay (Engine/Automation/Methods/ReplayMethods.h; Docs/Decisions/0030-m19-contract.md decision 14, A-RREAD):
// its own file, so the recording verification's ReplayMethods.cpp and the replay reader never share a file.

namespace Engine {

	namespace {

		constexpr uint32_t MaxReadReplayLimit = 1000;
		// The header members of a replay file, in its order (ReplayToJson).
		constexpr std::array<std::string_view, 6> HeaderMembers = { "Scene", "Parameters", "Seed", "FixedHz", "EngineVersion", "Config" };

		// One replay event in play.step's input form (InputEventParams, InputMethods.h): camelCase keys, only the members its
		// type reads, its type and state in their PascalCase names (which are the replay's own spellings), its tick the
		// replay's absolute tick.
		Json ToInputEvent(const ReplayEvent& event)
		{
			Json input = { { "tick", event.Tick }, { "type", event.Type } };
			const auto vector = [](glm::vec2 value)
			{
				return Json::array({ value.x, value.y });
			};
			if (event.Type == "Action")
			{
				input["name"] = event.Name;
				if (event.Value.has_value())
					input["value"] = *event.Value;
				else
					input["state"] = event.State.value_or("Down");
			}
			else if (event.Type == "Key")
			{
				input["key"] = event.KeyName;
				input["state"] = event.State.value_or("Down");
			}
			else if (event.Type == "MouseButton")
			{
				input["button"] = event.ButtonName;
				input["state"] = event.State.value_or("Down");
				if (event.Position.has_value())
					input["position"] = vector(*event.Position);
			}
			else if (event.Type == "MouseMove")
				input["position"] = vector(event.Position.value_or(glm::vec2(0.0f)));
			else if (event.Type == "MouseDelta" || event.Type == "Scroll")
				input["delta"] = vector(event.Delta.value_or(glm::vec2(0.0f)));
			else if (event.Type == "GamepadButton")
			{
				input["gamepad"] = event.Gamepad;
				input["button"] = event.ButtonName;
				input["state"] = event.State.value_or("Down");
			}
			else if (event.Type == "GamepadAxis")
			{
				input["gamepad"] = event.Gamepad;
				input["axis"] = event.AxisName;
				input["value"] = event.Value.value_or(0.0f);
			}
			else
				input["text"] = event.Text;
			return input;
		}

		// The first event of the page `cursor` names: "" is the first event, otherwise the decimal index nextCursor gave.
		// Errors: InvalidArgument at /cursor for anything else, or an index past the replay's events.
		Result<size_t> ParseEventCursor(std::string_view cursor, size_t count)
		{
			if (cursor.empty())
				return size_t{ 0 };
			size_t offset = 0;
			const auto [end, error] = std::from_chars(cursor.data(), cursor.data() + cursor.size(), offset);
			if (error != std::errc() || end != cursor.data() + cursor.size() || offset >= count)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/cursor", std::format("'{}' is not a cursor of this replay's events", cursor),
					"pass \"\" for the first page, or the nextCursor of the previous read of the same replay"));
			}
			return offset;
		}

	}

	namespace Automation {

		Result<InputReadReplayResult> InputReadReplay(AutomationMethodContext& context, const InputReadReplayParams& params)
		{
			if (params.Limit < 1 || params.Limit > MaxReadReplayLimit)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/limit",
					std::format("limit {} is outside 1 to {}", params.Limit, MaxReadReplayLimit)));
			}
			ReplayLoadReport report;
			std::string canonical;
			ENGINE_TRY_ASSIGN(const ReplayDocument document, context.ReadReplayDocument(params.Path, report, canonical));
			ENGINE_TRY_ASSIGN(const size_t first, ParseEventCursor(params.Cursor, document.Events.size()));
			ENGINE_TRY_ASSIGN(const Json file, ReplayToJson(document));

			InputReadReplayResult result;
			result.Path = std::move(canonical);
			result.FormatVersion = report.FileVersion;
			Json header = Json::object();
			for (const std::string_view member : HeaderMembers)
			{
				if (const auto value = file.find(std::string(member)); value != file.end())
					header[std::string(member)] = *value;
			}
			result.Header = VariantValue(std::move(header));
			result.FinalTick = ToAutomationCounter(document.FinalTick);
			result.FinalStateHash = document.FinalStateHash;
			result.EventCount = ToAutomationCounter(document.Events.size());
			const size_t end = std::min(document.Events.size(), first + params.Limit);
			for (size_t index = first; index < end; ++index)
				result.Events.emplace_back(ToInputEvent(document.Events[index]));
			if (end < document.Events.size())
				result.NextCursor = std::to_string(end);
			for (const ReplayExpectation& expectation : document.Expect)
				result.Expect.push_back({ .Tick = ToAutomationCounter(expectation.Tick), .Luau = expectation.Luau });
			for (const ReplayCheckpoint& checkpoint : document.Checkpoints)
				result.Checkpoints.push_back({ .Tick = ToAutomationCounter(checkpoint.Tick), .StateHash = checkpoint.StateHash });
			result.HasFingerprint = document.Fingerprint.has_value();
			return result;
		}

	}

}
