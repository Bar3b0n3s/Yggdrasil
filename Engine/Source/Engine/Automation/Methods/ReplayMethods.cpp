#include "EnginePCH.h"
#include "Engine/Automation/Methods/ReplayMethods.h"

#include "Engine/Asset/ReplayData.h"
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

		// input.record stop's recording guard (ReplayMethods.h): the candidate the input check decides about, and where it
		// goes when the check lets it through.
		struct RecordingCandidate
		{
			ReplayDocument Document{};
			std::string Path{};
		};

		// input.record stop's result for the written `document`; `checkTick` is where its input check ended (0: none ran).
		InputRecordResult MakeRecordResult(const ReplayDocument& document, std::string written, uint64_t checkTick)
		{
			return InputRecordResult{ .Recording = false, .Tick = ToAutomationCounter(document.FinalTick), .Events = ToAutomationCounter(document.Events.size()), .Path = std::move(written), .StateHash = document.FinalStateHash, .Checkpoints = ToAutomationCounter(document.Checkpoints.size()), .InputCheckTick = ToAutomationCounter(checkTick) };
		}

		// input.replay, and input.record stop's input check: plays `replay` in the host's play session, which the handler
		// restarted for it, at least one tick per Poll within PlayStepFrameBudget.
		class ReplayMethodOperation final : public PendingOperation, private IReplayEvaluator
		{
		public:
			// With `guard`, the operation is input.record stop's input check: `replay` is the candidate, played without its input,
			// and guard->Document is written to guard->Path only when that playback does not reproduce it.
			explicit ReplayMethodOperation(AssetRef<ReplayData> replay, std::optional<RecordingCandidate> guard = std::nullopt)
				: m_Replay(std::move(replay)), m_Guard(std::move(guard))
			{
			}

			[[nodiscard]] Status Begin(PlaySession& session, const ReplayPlaybackOptions& options)
			{
				m_Serial = session.GetSerial();
				m_WithoutInput = options.WithoutInput;
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
						return Failed(Error(ErrorCode::Cancelled, "the replay session ended or was replaced"));
					}
					const auto advanced = m_Player.Advance(*session, *this);
					m_Tick = session->GetTick();
					if (!advanced)
					{
						const Error error = advanced.error();
						Cancel(context);
						return Failed(error);
					}
					if (*advanced)
					{
						const auto playback = m_Player.GetResult();
						Cancel(context);
						if (!playback)
							return Failed(playback.error());
						const Result<Json> serialized = context.SerializeResult(MakeReplayResult(*playback));
						if (!serialized)
							return std::unexpected(serialized.error());
						if (m_Guard)
							return FinishInputCheck(host, *playback, *serialized);
						if (!playback->Passed)
						{
							context.SetErrorData("replay", *serialized);
							if (playback->CheckpointFailed)
								return MakeError(ErrorCode::Validation, "replay verification failed at checkpoint tick {}: expected state hash {}, observed {}", playback->FinalTick, playback->ExpectedStateHash, playback->StateHash);
							if (!playback->StopReason.empty())
								return MakeError(ErrorCode::Validation, "replay failed: {}", playback->StopReason);
							return MakeError(ErrorCode::Validation, "replay verification failed at tick {}", playback->FinalTick);
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

			[[nodiscard]] std::string GetPhase() const override { return std::format("{} {}/{}", m_Guard ? "Record:input check" : "Play:replay", m_Tick, m_Replay->FinalTick); }
		private:
			[[nodiscard]] InputReplayResult MakeReplayResult(const ReplayPlaybackResult& playback) const
			{
				InputReplayResult result{ .Passed = playback.Passed, .Verified = playback.Verified, .HashChecked = playback.HashChecked, .HashMatched = playback.HashMatched, .FinalTick = ToAutomationCounter(playback.FinalTick), .StateHash = playback.StateHash, .ExpectedStateHash = playback.ExpectedStateHash, .StopReason = playback.StopReason, .CheckpointsChecked = ToAutomationCounter(playback.CheckpointsChecked), .CheckpointFailed = playback.CheckpointFailed, .WithoutInput = m_WithoutInput, .Expect = {} };
				for (size_t index = 0; index < playback.Expect.size(); ++index)
				{
					const ReplayExpectationOutcome& outcome = playback.Expect[index];
					InputReplayResult::Expectation row{ .Tick = ToAutomationCounter(outcome.Tick), .Evaluated = outcome.Evaluated, .Satisfied = outcome.Satisfied, .Value = outcome.Value, .Message = outcome.Failure ? outcome.Failure->GetMessageText() : std::string{}, .File = outcome.File, .Line = outcome.Line };
					if (index < m_Replay->Expect.size())
						row.JsonPointer = m_Replay->Expect[index].Script.SourceMap.JsonPointer;
					if (outcome.Failure && outcome.Failure->GetLocation().JsonPointer.has_value())
						row.JsonPointer = *outcome.Failure->GetLocation().JsonPointer;
					result.Expect.push_back(std::move(row));
				}
				return result;
			}

			// An error of the operation; the input check adds that nothing was written.
			[[nodiscard]] Result<Json> Failed(Error error) const
			{
				if (!m_Guard)
					return std::unexpected(std::move(error));
				return std::unexpected(std::move(error).WithContext(std::format("while checking that '{}' depends on its input; nothing was written", m_Guard->Path)));
			}

			// The input check's verdict (ReplayMethods.h): a playback without input that passes the candidate's verification
			// (every checkpoint, the final hash, every expectation), whatever script errors it raised, shows its outcome does
			// not depend on its input, so nothing is written; otherwise the candidate is.
			[[nodiscard]] Result<Json> FinishInputCheck(AutomationMethodContext& context, const ReplayPlaybackResult& playback, const Json& outcome)
			{
				const ReplayDocument& candidate = m_Guard->Document;
				if (playback.Passed)
				{
					context.SetErrorData("inputCheck", outcome);
					return std::unexpected(Error(ErrorCode::Validation, std::format("refusing to write '{}': its outcome does not depend on its input (the same scene, seed and parameters run "
																					"without its {} input event(s) reproduce its final state hash at tick {}, its {} checkpoint(s) and its {} expectation(s))",
																			m_Guard->Path, candidate.Events.size(), candidate.FinalTick, candidate.Checkpoints.size(), candidate.Expect.size()))
							.WithLocation({ .File = {}, .JsonPointer = "/path", .Entity = {} })
							.WithHint(std::format("end the recording while the played state is still in the scene (a recording is refused only when its input never shows in the "
												  "world at a checkpoint, one every {0} ticks, at its end or in an expectation, as in one shorter than {0} ticks that ends "
												  "idle, input that changes only script state, or play after the last checkpoint that a restart undoes), or add an "
												  "expectation that only the input satisfies; a replay without input events records a pure determinism run",
								ReplayCheckpointInterval)));
				}
				ENGINE_TRY_ASSIGN(std::string written, context.WriteReplay(m_Guard->Path, candidate));
				return context.SerializeResult(MakeRecordResult(candidate, std::move(written), playback.FinalTick));
			}

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
			std::optional<RecordingCandidate> m_Guard{};
			ReplayPlayer m_Player{};
			uint64_t m_Serial = 0;
			uint64_t m_Tick = 0;
			bool m_WithoutInput = false;
			bool m_Released = false;
		};

		// input.record's Start and an event-free Stop: the outcome is known when the handler returns, and the first Poll (in
		// the same dispatch) resolves it.
		class ResolvedRecordOperation final : public PendingOperation
		{
		public:
			explicit ResolvedRecordOperation(Json result)
				: m_Result(std::move(result))
			{
			}

			[[nodiscard]] std::optional<Result<Json>> Poll(MethodContext& /*context*/) override { return Result<Json>(std::move(m_Result)); }
		private:
			Json m_Result{};
		};

		[[nodiscard]] Result<Scope<PendingOperation>> Resolve(AutomationMethodContext& context, const InputRecordResult& result)
		{
			ENGINE_TRY_ASSIGN(Json serialized, context.SerializeResult(result));
			return Scope<PendingOperation>(CreateScope<ResolvedRecordOperation>(std::move(serialized)));
		}

	}

	namespace Automation {

		Result<Scope<PendingOperation>> InputRecord(AutomationMethodContext& context, const InputRecordParams& params)
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
				// The recorded session starts from the parameters exactly as the replay header stores them (floats as
				// float, §6), so every replay of it reads the same Scene.GetLoadParameters values (§6.7).
				const Result<Json> parameters = CanonicalizeReplayParameters(context.HasParam("parameters") ? params.Parameters.Get() : Json::object(), "/parameters");
				if (!parameters)
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, parameters.error().GetLocation().JsonPointer.value_or("/parameters"), parameters.error().GetMessageText(), "replay headers store numbers as float (§6.7); give values within the float range"));
				PlayStartOptions options;
				options.Lockstep = true;
				options.LockstepOwner = context.GetRequest().Client;
				options.ScenePath = params.Scene;
				options.Parameters = VariantValue(*parameters);
				options.Seed = context.HasParam("seed") ? std::optional<uint64_t>(params.Seed) : std::nullopt;
				ENGINE_TRY(context.StartRecordingSession(options, params.Restart));
				ENGINE_TRY_ASSIGN(PlaySession * session, Utils::RequirePlaySession(context));
				ENGINE_TRY_ASSIGN(const ReplayHeader header, context.DescribeReplayHeader());
				ReplayRecorder* recorder = session->GetRecorder();
				if (recorder == nullptr)
					return MakeError(ErrorCode::InvalidState, "the session has no input recorder");
				ENGINE_TRY(recorder->Begin(header, *session));
				return Resolve(context, InputRecordResult{ .Recording = true, .Tick = 0 });
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
			std::vector<ReplayBytecodeExpectation> compiledExpectations;
			for (size_t index = 0; index < params.Expect.size(); ++index)
			{
				const InputRecordExpectation& expectation = params.Expect[index];
				if (expectation.Tick > session->GetTick() || (index != 0 && expectation.Tick < params.Expect[index - 1].Tick))
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("/expect/{}/tick", index), "expectation ticks must be ordered and within the recording", {}));
				const std::string pointer = std::format("/expect/{}/luau", index);
				ENGINE_TRY_ASSIGN(ScriptCompilation compiled, ScriptCompiler::Compile({ .Source = expectation.Luau, .Mode = ScriptCompileMode::ExpressionOrChunk, .ChunkName = "=input.record", .JsonPointer = pointer }));
				expectations.push_back({ .Tick = expectation.Tick, .Luau = expectation.Luau });
				ReplayBytecodeExpectation bytecode;
				bytecode.Tick = expectation.Tick;
				bytecode.Script.Bytecode = std::move(compiled.Bytecode);
				bytecode.Script.SourceMap = std::move(compiled.SourceMap);
				compiledExpectations.push_back(std::move(bytecode));
			}
			ENGINE_TRY_ASSIGN(ReplayDocument document, recorder->Finish(*session, expectations));
			if (document.Events.empty())
			{
				// A pure determinism replay: nothing to depend on, so no input check (ReplayMethods.h).
				ENGINE_TRY_ASSIGN(std::string written, context.WriteReplay(params.Path, document));
				return Resolve(context, MakeRecordResult(document, std::move(written), 0));
			}
			// The recording guard: the play session restarts for the candidate, played without its input, and the candidate is
			// written only when that playback does not reproduce it. The recording has ended; a failure here writes nothing.
			auto replay = CreateRef<ReplayData>();
			replay->Header = document.Header;
			replay->Events = document.Events;
			replay->Expect = std::move(compiledExpectations);
			replay->Checkpoints = document.Checkpoints;
			replay->FinalTick = document.FinalTick;
			replay->FinalStateHash = document.FinalStateHash;
			const std::string failureContext = std::format("while starting the input check of '{}'; nothing was written", params.Path);
			const ReplayHeader header = document.Header;
			auto operation = CreateScope<ReplayMethodOperation>(AssetRef<ReplayData>(std::move(replay)), RecordingCandidate{ .Document = std::move(document), .Path = params.Path });
			if (Status restarted = context.RestartForReplay(header); !restarted)
				return std::unexpected(std::move(restarted).error().WithContext(failureContext));
			ENGINE_TRY_ASSIGN(PlaySession * check, Utils::RequirePlaySession(context));
			check->SetLockstep(true, context.GetRequest().Client);
			if (Status begun = operation->Begin(*check, { .Verify = true, .StrictHash = true, .WithoutInput = true }); !begun)
			{
				operation->Cancel(context);
				return std::unexpected(std::move(begun).error().WithContext(failureContext));
			}
			return Scope<PendingOperation>(std::move(operation));
		}

		Result<Scope<PendingOperation>> InputReplay(AutomationMethodContext& context, const InputReplayParams& params)
		{
			ENGINE_TRY(CheckReplayAdmission(context));
			if (params.StrictHash && !params.Verify)
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/strictHash", "strictHash requires verify:true", {}));
			// Without verification nothing is checked, so the input check would always pass and prove nothing.
			if (params.WithoutInput && !params.Verify)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/withoutInput", "withoutInput requires verify:true",
					"the input check is {\"verify\": true, \"strictHash\": true, \"withoutInput\": true}, and it must fail"));
			}
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
			const Status begun = operation->Begin(*session, { .Verify = params.Verify, .StrictHash = params.StrictHash, .WithoutInput = params.WithoutInput });
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
			.Field("stateHash", &InputRecordResult::StateHash, "Observed terminal state hash, written as FinalStateHash; empty on Start. It covers the simulated world only (every entity with its serialized components, the tick, the session's Random state, the id draws and every body's physics state), never script tables, audio or input.")
			.Field("checkpoints", &InputRecordResult::Checkpoints, "State-hash checkpoints written, one every 60 ticks before the final tick; 0 on Start.")
			.Field("inputCheckTick", &InputRecordResult::InputCheckTick, "Stop with input events: the tick where the input check (the recording replayed without its input) ended, at the first checkpoint that differed or at the final tick; the play session waits paused there. 0 when no check ran.");
		registry.Struct<InputReplayParams>("InputReplayParams", "Replay an asset in a fresh deterministic session.")
			.Field("path", &InputReplayParams::Path, "Replay asset path, or a Runtime recording's returned user://Replays identity.")
			.Field("verify", &InputReplayParams::Verify, "Evaluate cooked expectations.")
			.Field("strictHash", &InputReplayParams::StrictHash, "Compare the recorded state hash at every checkpoint and at the final tick; requires verify. The first checkpoint that differs ends the playback there.")
			.Field("withoutInput", &InputReplayParams::WithoutInput, "Apply none of the replay's input events: the check that its outcome depends on its input; requires verify. A replay whose verification still passes this way proves nothing about its input.");
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
			.Field("hashChecked", &InputReplayResult::HashChecked, "The state hashes (every checkpoint and the final one) were compared.")
			.Field("hashMatched", &InputReplayResult::HashMatched, "Compared hashes matched.")
			.Field("finalTick", &InputReplayResult::FinalTick, "Completed simulation ticks: the replay's final tick, or where playback stopped.")
			.Field("stateHash", &InputReplayResult::StateHash, "Observed state hash at finalTick. It covers the simulated world only (every entity with its serialized components, the tick, the session's Random state, the id draws and every body's physics state), never script tables, audio or input.")
			.Field("expectedStateHash", &InputReplayResult::ExpectedStateHash, "Recorded state hash at finalTick: the final hash, or the failed checkpoint's.")
			.Field("stopReason", &InputReplayResult::StopReason, "Why playback ended before the replay's final tick (a quit, a fatal script error or a checkpoint whose hash differed); empty when it reached it.")
			.Field("checkpointsChecked", &InputReplayResult::CheckpointsChecked, "State-hash checkpoints compared (strictHash), the failed one included.")
			.Field("checkpointFailed", &InputReplayResult::CheckpointFailed, "A checkpoint's hash differed: playback stopped at its tick (finalTick), and stateHash and expectedStateHash are the observed and recorded hashes there.")
			.Field("withoutInput", &InputReplayResult::WithoutInput, "None of the replay's input events were applied.")
			.Field("expect", &InputReplayResult::Expect, "Every expectation outcome in authored order.");
	}

	void RegisterReplayMethods(MethodRegistry& methods)
	{
		methods.AddPending<AutomationMethodContext, InputRecordParams, InputRecordResult>(
			{ .Name = "input.record", .Description = "Starts a tick-zero recording or saves its validated applied inputs, expectations and state-hash checkpoints (one every 60 ticks). Stop checks output write permission. A recording with input events is first replayed without them (the input check, which replaces the play session as input.replay does and leaves it paused where it ended); when that run reproduces every checkpoint, the final hash and every expectation, whatever script errors it raises, the recording's outcome does not depend on its input and Stop refuses to write it (Validation, the check's outcome in error data inputCheck). End such a recording while the played state is still in the scene.", .RequiredParams = { "action" }, .ExposeAsTool = true, .AvailableInRuntime = true, .TimeoutSeconds = 600, .Examples = { { .Description = "Start a fresh recording.", .Params = Json{ { "action", "Start" } } }, { .Description = "Save the recording.", .Params = Json{ { "action", "Stop" }, { "path", "Assets/Tests/Recorded.replay" } } } } }, &Automation::InputRecord);
		methods.AddPending<AutomationMethodContext, InputReplayParams, InputReplayResult>(
			{ .Name = "input.replay", .Description = "Replays recorded inputs in a fresh session; verification failures retain every expectation and hash outcome in error data. strictHash compares the state hash at each checkpoint and at the final tick and stops at the first checkpoint that differs. The state hash covers the simulated world (entities and their serialized components, the tick, Random, id draws, physics), never script tables, so expectations observe script state. withoutInput replays without the input events, the check that the replay depends on its input. Afterwards the session stays paused at the replay's last tick (or where playback stopped), so script.eval, scene.query and entity.get read its final state; play.stop ends it.", .RequiredParams = { "path" }, .ExposeAsTool = true, .AvailableInRuntime = true, .TimeoutSeconds = 600, .Examples = { { .Description = "Verify a replay.", .Params = Json{ { "path", "Assets/Tests/Recorded.replay" }, { "verify", true }, { "strictHash", true } } }, { .Description = "Check that a replay's verification fails without its input.", .Params = Json{ { "path", "Assets/Tests/Recorded.replay" }, { "verify", true }, { "strictHash", true }, { "withoutInput", true } } } } }, &Automation::InputReplay);
	}

}
