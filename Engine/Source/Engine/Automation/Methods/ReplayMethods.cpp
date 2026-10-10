#include "EnginePCH.h"
#include "Engine/Automation/Methods/ReplayMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/PlayMethods.h"
#include "Engine/Automation/Methods/Private/PlayMethodSupport.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Platform/Input/InputActionMap.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/ScriptCompiler.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Session/PlayInputEventCodec.h"
#include "Engine/Session/ReplayPlayer.h"
#include "Engine/Session/ReplayRecorder.h"

#include <nlohmann/json.hpp>

#include <array>
#include <format>
#include <optional>

namespace Engine {

	namespace {

		Status CheckReplayAdmission(AutomationMethodContext& context)
		{
			if (const PlaySession* session = context.GetPlaySession())
			{
				ENGINE_TRY(Utils::CheckLockstepOwner(context, *session));
				if (session->IsStepping())
					return MakeError(ErrorCode::InvalidState, "another operation is advancing the play session");
			}
			return {};
		}

		class ReplayMethodOperation final : public PendingOperation, private IReplayEvaluator
		{
		public:
			explicit ReplayMethodOperation(AssetRef<ReplayData> replay)
				: m_Replay(std::move(replay))
			{
			}

			[[nodiscard]] Status Begin(PlaySession& session, const ReplayPlaybackOptions& options)
			{
				m_Serial = session.GetSerial();
				ENGINE_TRY(m_Player.Begin(m_Replay, session, options));
				session.SetStepping(true);
				return {};
			}

			[[nodiscard]] std::optional<Result<Json>> Poll(MethodContext& context) override
			{
				auto& host = static_cast<AutomationMethodContext&>(context);
				const auto start = host.GetWallClockTime();
				do
				{
					PlaySession* session = host.GetPlaySession();
					if (session == nullptr || session->GetSerial() != m_Serial || !session->IsStepping())
					{
						Cancel(context);
						return MakeError(ErrorCode::Cancelled, "the replay session ended or was replaced");
					}
					const auto advanced = m_Player.Advance(*session, *this);
					m_Tick = session->GetTick();
					if (!advanced)
					{
						const Error error = advanced.error();
						Cancel(context);
						return std::unexpected(error);
					}
					if (*advanced)
					{
						const auto playback = m_Player.GetResult();
						if (!playback)
						{
							const Error error = playback.error();
							Cancel(context);
							return std::unexpected(error);
						}
						InputReplayResult result{ .Passed = playback->Passed, .Verified = playback->Verified, .HashChecked = playback->HashChecked, .HashMatched = playback->HashMatched, .FinalTick = ToAutomationCounter(playback->FinalTick), .StateHash = playback->StateHash, .ExpectedStateHash = playback->ExpectedStateHash };
						for (size_t index = 0; index < playback->Expect.size(); ++index)
						{
							const ReplayExpectationOutcome& outcome = playback->Expect[index];
							InputReplayResult::Expectation row{ .Tick = ToAutomationCounter(outcome.Tick), .Evaluated = outcome.Evaluated, .Satisfied = outcome.Satisfied, .Value = outcome.Value, .Message = outcome.Failure ? outcome.Failure->GetMessageText() : std::string{}, .File = outcome.File, .Line = outcome.Line };
							if (index < m_Replay->Expect.size())
								row.JsonPointer = m_Replay->Expect[index].Script.SourceMap.JsonPointer;
							if (outcome.Failure && outcome.Failure->GetLocation().JsonPointer.has_value())
								row.JsonPointer = *outcome.Failure->GetLocation().JsonPointer;
							result.Expect.push_back(std::move(row));
						}
						Cancel(context);
						const auto serialized = context.SerializeResult(result);
						if (!serialized)
							return std::unexpected(serialized.error());
						if (!result.Passed)
						{
							context.SetErrorData("replay", *serialized);
							return MakeError(ErrorCode::Validation, "replay verification failed at tick {}", result.FinalTick);
						}
						return *serialized;
					}
				} while (host.GetWallClockTime() - start < PlayStepFrameBudget);
				return std::nullopt;
			}

			void Cancel(MethodContext& context) override
			{
				if (m_Released)
					return;
				m_Released = true;
				m_Player.Cancel();
				auto& host = static_cast<AutomationMethodContext&>(context);
				host.ReleaseReplayInput(m_Serial);
				PlaySession* session = host.GetPlaySession();
				if (session != nullptr && session->GetSerial() == m_Serial)
				{
					session->SetPaused(true);
					session->SetStepping(false);
					session->SetExtractionEnabled(true);
					session->SetLockstep(false);
				}
			}

			[[nodiscard]] std::string GetPhase() const override { return std::format("Play:replay {}/{}", m_Tick, m_Replay->FinalTick); }
		private:
			[[nodiscard]] Result<Json> Evaluate(PlaySession& session, const ReplayBytecodeExpectation& expectation) override
			{
				ScriptEngine* scripts = session.GetScripts();
				if (scripts == nullptr || scripts->IsStopped())
					return MakeError(ErrorCode::InvalidState, "replay expectation requires a live Play VM");
				ENGINE_TRY_ASSIGN(ScriptEvaluation result, scripts->ExecuteBytecode(expectation.Script));
				return result.Value.Get();
			}
		private:
			AssetRef<ReplayData> m_Replay{};
			ReplayPlayer m_Player{};
			uint64_t m_Serial = 0;
			uint64_t m_Tick = 0;
			bool m_Released = false;
		};

	}

	namespace Automation {

		Result<InputRecordResult> InputRecord(AutomationMethodContext& context, const InputRecordParams& params)
		{
			ENGINE_TRY(CheckReplayAdmission(context));
			if (params.Action == InputRecordAction::Start)
			{
				for (const std::string_view field : std::array{ "path", "expect" })
					if (context.HasParam(field))
						return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("/{}", field), "member is only valid for Stop", {}));
				if (context.HasParam("parameters") && !params.Parameters.Get().is_object())
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/parameters", "parameters must be an object", {}));
				if (context.GetPlaySession() != nullptr && !params.Restart)
					return MakeError(ErrorCode::InvalidState, "recording requires a fresh tick-zero session; pass restart:true to replace the current session");
				PlayStartOptions options;
				options.Lockstep = true;
				options.LockstepOwner = context.GetRequest().Client;
				options.ScenePath = params.Scene;
				options.Parameters = params.Parameters;
				options.Seed = context.HasParam("seed") ? std::optional<uint64_t>(params.Seed) : std::nullopt;
				ENGINE_TRY(context.StartRecordingSession(options, params.Restart));
				ENGINE_TRY_ASSIGN(PlaySession * session, Utils::RequirePlaySession(context));
				ENGINE_TRY_ASSIGN(const ReplayHeader header, context.DescribeReplayHeader());
				ReplayRecorder* recorder = session->GetRecorder();
				if (recorder == nullptr)
					return MakeError(ErrorCode::InvalidState, "the session has no input recorder");
				ENGINE_TRY(recorder->Begin(header, *session));
				return InputRecordResult{ .Recording = true, .Tick = 0 };
			}
			if (params.Action != InputRecordAction::Stop)
				return MakeError(ErrorCode::InvalidArgument, "unknown recording action");
			for (const std::string_view field : std::array{ "restart", "scene", "parameters", "seed" })
				if (context.HasParam(field))
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("/{}", field), "member is only valid for Start", {}));
			if (!context.HasParam("path") || params.Path.empty())
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/path", "Stop requires an output path", {}));
			ENGINE_TRY_ASSIGN(PlaySession * session, Utils::RequirePlaySession(context));
			ReplayRecorder* recorder = session->GetRecorder();
			if (recorder == nullptr || !recorder->IsRecording())
				return MakeError(ErrorCode::InvalidState, "no input recording is active");
			ENGINE_TRY(context.ValidateReplayOutput(params.Path));
			std::vector<ReplayExpectation> expectations;
			for (size_t index = 0; index < params.Expect.size(); ++index)
			{
				const InputRecordExpectation& expectation = params.Expect[index];
				if (expectation.Tick > session->GetTick() || (index != 0 && expectation.Tick < params.Expect[index - 1].Tick))
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("/expect/{}/tick", index), "expectation ticks must be ordered and within the recording", {}));
				const std::string pointer = std::format("/expect/{}/luau", index);
				ENGINE_TRY_ASSIGN(const ScriptCompilation compiled, ScriptCompiler::Compile({ .Source = expectation.Luau, .Mode = ScriptCompileMode::ExpressionOrChunk, .ChunkName = "=input.record", .JsonPointer = pointer }));
				static_cast<void>(compiled);
				expectations.push_back({ .Tick = expectation.Tick, .Luau = expectation.Luau });
			}
			ENGINE_TRY_ASSIGN(const ReplayDocument document, recorder->Finish(*session, expectations));
			ENGINE_TRY_ASSIGN(std::string written, context.WriteReplay(params.Path, document));
			return InputRecordResult{ .Recording = false, .Tick = ToAutomationCounter(document.FinalTick), .Events = ToAutomationCounter(document.Events.size()), .Path = std::move(written), .StateHash = document.FinalStateHash };
		}

		Result<Scope<PendingOperation>> InputReplay(AutomationMethodContext& context, const InputReplayParams& params)
		{
			ENGINE_TRY(CheckReplayAdmission(context));
			if (params.StrictHash && !params.Verify)
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/strictHash", "strictHash requires verify:true", {}));
			ENGINE_TRY_ASSIGN(AssetRef<ReplayData> replay, context.LoadReplay(params.Path));
			const ProjectSettings* settings = context.GetProjectSettings();
			if (settings == nullptr)
				return MakeError(ErrorCode::InvalidState, "no project settings are available");
			ENGINE_TRY_ASSIGN(const PlayInput input, PlayInput::Create(settings->Input));
			for (size_t index = 0; index < replay->Events.size(); ++index)
			{
				ENGINE_TRY_ASSIGN(const StampedPlayInputEvent event, DecodeReplayEvent(replay->Events[index], input.GetActions(), std::format("/Events/{}", index)));
				static_cast<void>(event);
			}
			auto operation = CreateScope<ReplayMethodOperation>(replay);
			ENGINE_TRY(context.RestartForReplay(replay->Header));
			ENGINE_TRY_ASSIGN(PlaySession * session, Utils::RequirePlaySession(context));
			session->SetLockstep(true, context.GetRequest().Client);
			const Status begun = operation->Begin(*session, { .Verify = params.Verify, .StrictHash = params.StrictHash });
			if (!begun)
			{
				operation->Cancel(context);
				return std::unexpected(begun.error());
			}
			return Scope<PendingOperation>(std::move(operation));
		}

	}

	void RegisterReplayMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<InputRecordAction>("InputRecordAction", "Start a fresh recording or stop and save it.")
			.Entry(InputRecordAction::Start, "Start", "Start at tick zero.")
			.Entry(InputRecordAction::Stop, "Stop", "Validate and save the completed recording.");
		registry.Struct<InputRecordExpectation>("InputRecordExpectation", "An authored replay assertion at a completed-tick boundary.")
			.Field("tick", &InputRecordExpectation::Tick, "Completed ticks, including zero.")
			.Field("luau", &InputRecordExpectation::Luau, "Luau expression or chunk returning a truthy value.");
		registry.Struct<InputRecordParams>("InputRecordParams", "Action-dependent recording parameters; inappropriate members are refused.")
			.Field("action", &InputRecordParams::Action, "Start or Stop.")
			.Field("restart", &InputRecordParams::Restart, "Start only: explicitly replace an existing play session.")
			.Field("scene", &InputRecordParams::Scene, "Start only: saved scene path; absent uses the host's current scene.")
			.Field("parameters", &InputRecordParams::Parameters, "Start only: initial scene load parameters object.")
			.Field("seed", &InputRecordParams::Seed, "Start only: initial seed; absent inherits session/project policy.")
			.Field("path", &InputRecordParams::Path, "Stop only, required: confined replay destination (Runtime relative paths are below user://Replays).")
			.Field("expect", &InputRecordParams::Expect, "Stop only: ordered expectations, compiled before consuming the recording.");
		registry.Struct<InputRecordResult>("InputRecordResult", "Recording state or saved replay.")
			.Field("recording", &InputRecordResult::Recording, "Recording remains active.")
			.Field("tick", &InputRecordResult::Tick, "Completed ticks.")
			.Field("events", &InputRecordResult::Events, "Applied input events written.")
			.Field("path", &InputRecordResult::Path, "Canonical saved path; empty on Start.")
			.Field("stateHash", &InputRecordResult::StateHash, "Observed terminal state hash; empty on Start.");
		registry.Struct<InputReplayParams>("InputReplayParams", "Replay an asset in a fresh deterministic session.")
			.Field("path", &InputReplayParams::Path, "Replay asset path, or a Runtime recording's returned user://Replays identity.")
			.Field("verify", &InputReplayParams::Verify, "Evaluate cooked expectations.")
			.Field("strictHash", &InputReplayParams::StrictHash, "Compare the recorded final hash; requires verify.");
		registry.Struct<InputReplayResult::Expectation>("InputReplayExpectationResult", "One replay expectation and its authored location.")
			.Field("tick", &InputReplayResult::Expectation::Tick, "Completed-tick boundary.")
			.Field("evaluated", &InputReplayResult::Expectation::Evaluated, "The expectation executed.")
			.Field("satisfied", &InputReplayResult::Expectation::Satisfied, "The result was Lua-truthy.")
			.Field("value", &InputReplayResult::Expectation::Value, "Detached result value.")
			.Field("message", &InputReplayResult::Expectation::Message, "Failure explanation, when present.")
			.Field("file", &InputReplayResult::Expectation::File, "Authored replay path.")
			.Field("line", &InputReplayResult::Expectation::Line, "Line within the authored predicate.")
			.Field("jsonPointer", &InputReplayResult::Expectation::JsonPointer, "Embedded Expect source pointer.");
		registry.Struct<InputReplayResult>("InputReplayResult", "Complete replay verification outcome, also returned as structured error data on failure.")
			.Field("passed", &InputReplayResult::Passed, "Playback and all requested verification passed.")
			.Field("verified", &InputReplayResult::Verified, "Expectation verification was requested.")
			.Field("hashChecked", &InputReplayResult::HashChecked, "The final state hash was compared.")
			.Field("hashMatched", &InputReplayResult::HashMatched, "Compared hashes matched.")
			.Field("finalTick", &InputReplayResult::FinalTick, "Completed simulation ticks.")
			.Field("stateHash", &InputReplayResult::StateHash, "Observed final hash.")
			.Field("expectedStateHash", &InputReplayResult::ExpectedStateHash, "Recorded final hash.")
			.Field("expect", &InputReplayResult::Expect, "Every expectation outcome in authored order.");
	}

	void RegisterReplayMethods(MethodRegistry& methods)
	{
		methods.Add({ .Name = "input.record", .Description = "Starts a tick-zero recording or saves its validated applied inputs and expectations. Stop checks output write permission.", .RequiredParams = { "action" }, .ExposeAsTool = true, .AvailableInRuntime = true, .Examples = { { .Description = "Start a fresh recording.", .Params = Json{ { "action", "Start" } } }, { .Description = "Save the recording.", .Params = Json{ { "action", "Stop" }, { "path", "Assets/Tests/Recorded.replay" } } } } }, &Automation::InputRecord);
		methods.AddPending<AutomationMethodContext, InputReplayParams, InputReplayResult>(
			{ .Name = "input.replay", .Description = "Replays recorded inputs in a fresh session; verification failures retain every expectation and hash outcome in error data.", .RequiredParams = { "path" }, .ExposeAsTool = true, .AvailableInRuntime = true, .TimeoutSeconds = 600, .Examples = { { .Description = "Verify a replay.", .Params = Json{ { "path", "Assets/Tests/Recorded.replay" }, { "verify", true }, { "strictHash", true } } } } }, &Automation::InputReplay);
	}

}
