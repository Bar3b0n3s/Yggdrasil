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
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Session/PlayInputEventCodec.h"
#include "Engine/Session/ReplayPlayer.h"
#include "Engine/Session/ReplayRecorder.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

		// input.record stop's candidate (ReplayMethods.h): verified with its input, then, when it has input events, checked
		// without it, and written to Path only when both let it through.
		struct RecordingCandidate
		{
			ReplayDocument Document{};
			std::string Path{};
		};

		// Docs/Decisions/0027-m17-contract.md "Frozen interfaces / A1": the hint of a recording that its own input does not
		// reproduce.
		constexpr std::string_view RecordingNotReproducedHint = "a script.eval or play.waitFor that writes script state (a self field, a module table) during a "
																"recording is neither recorded nor detected, an expectation must hold when the recording's own "
																"input is replayed, and a script that reads the host (Application queries, pairs over entity "
																"keys) diverges; record again without them";

		// What input.replay {withoutInput: true}'s failures say instead of GetReplayDivergenceHint: that playback is the input
		// check, and its failure the expected outcome for a replay that depends on its input.
		constexpr std::string_view ReplayWithoutInputNote = "the replay ran without its input events (withoutInput), which a replay that depends on its input is expected to fail";

		// input.record stop's result for the written `document`; `checkTick` is where its input check ended (0: none ran), and
		// `warnings` are the notes on the written replay (InputRecordResult::Warnings).
		InputRecordResult MakeRecordResult(const ReplayDocument& document, std::string written, uint64_t checkTick, std::vector<std::string> warnings = {})
		{
			return InputRecordResult{ .Recording = false, .Tick = ToAutomationCounter(document.FinalTick), .Events = ToAutomationCounter(document.Events.size()), .Path = std::move(written), .StateHash = document.FinalStateHash, .Checkpoints = ToAutomationCounter(document.Checkpoints.size()), .InputCheckTick = ToAutomationCounter(checkTick), .Warnings = std::move(warnings) };
		}

		// Where an input check ended (InputRecordResult::InputCheckTick): the tick of the first checkpoint whose state hash
		// differed, which a playback that continues past it keeps in FirstCheckpointFailure, else where the playback ended.
		[[nodiscard]] uint64_t GetInputCheckTick(const ReplayPlaybackResult& playback)
		{
			return playback.FirstCheckpointFailure.has_value() ? playback.FirstCheckpointFailure->Tick : playback.FinalTick;
		}

		// input.replay, and input.record stop's playbacks of a candidate: plays `replay` in the host's play session, restarted
		// for it (Restart), at least one tick per Poll within PlayStepFrameBudget.
		class ReplayMethodOperation final : public PendingOperation, private IReplayEvaluator
		{
		public:
			// With `candidate`, the operation is input.record stop's (ReplayMethods.h): `replay` is the candidate, first played
			// with its input (the verification, Docs/Decisions/0027-m17-contract.md A1; Docs/Decisions/0030-m19-contract.md
			// decisions 4 and 5), then, when it has input events, without it (the input check), and candidate->Document is
			// written to candidate->Path only when the verification reproduces it and the input check does not.
			explicit ReplayMethodOperation(AssetRef<ReplayData> replay, std::optional<RecordingCandidate> candidate = std::nullopt)
				: m_Replay(std::move(replay)), m_Guard(std::move(candidate)), m_Stage(m_Guard ? Stage::Verification : Stage::Replay)
			{
			}

			// Restarts the host's play session for the replay (RestartForReplay), takes its lockstep for the requesting client
			// and begins the current stage's playback there. A failure to begin cancels the operation, which pauses the new
			// session and releases its lockstep.
			[[nodiscard]] Status Restart(AutomationMethodContext& context, const ReplayPlaybackOptions& options)
			{
				ENGINE_TRY(context.RestartForReplay(m_Replay->Header));
				ENGINE_TRY_ASSIGN(PlaySession * session, Utils::RequirePlaySession(context));
				session->SetLockstep(true, context.GetRequest().Client);
				if (Status begun = Begin(*session, options); !begun)
				{
					Cancel(context);
					return begun;
				}
				return {};
			}

			// input.record stop's first playback: the candidate with its input (A1). Errors carry the verification's context.
			// The candidate was recorded moments before, so a checkpoint that differs does not blame changes since then: its
			// stop reason keeps the plain form (NameDivergenceCauses false), and the refusal's hint names the usual causes.
			[[nodiscard]] Status StartVerification(AutomationMethodContext& context)
			{
				if (Status started = Restart(context, { .Verify = true, .StrictHash = true, .WithoutInput = false, .NameDivergenceCauses = false }); !started)
					return std::unexpected(std::move(started).error().WithContext(std::format("while starting the verification of '{}' with its input; nothing was written", m_Guard->Path)));
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
					if (m_Stage == Stage::Verification)
						NoteScriptErrors(*session);
					if (*advanced)
					{
						const auto playback = m_Player.GetResult();
						Cancel(context);
						if (!playback)
							return Failed(playback.error());
						const Result<Json> serialized = context.SerializeResult(MakeReplayResult(*playback));
						if (!serialized)
							return std::unexpected(serialized.error());
						if (m_Stage == Stage::Verification)
						{
							if (std::optional<std::string> reason = DescribeVerificationFailure(*playback); reason.has_value())
								return RefuseUnreproduced(host, *reason, *serialized);
							const Result<std::optional<std::string>> teardown = StopVerificationScripts(host);
							if (!teardown)
								return Failed(teardown.error());
							if (teardown->has_value())
								return RefuseUnreproduced(host, **teardown, *serialized);
							// A recording without input events has no input to depend on: it is written after its verification
							// (Docs/Decisions/0030-m19-contract.md decision 4), and the play session stays the verification's.
							if (m_Guard->Document.Events.empty())
								return WriteCandidate(host, 0, nullptr);
							if (Status started = StartInputCheck(host); !started)
								return std::unexpected(started.error());
							continue;
						}
						if (m_Stage == Stage::InputCheck)
							return FinishInputCheck(host, *playback, *serialized);
						if (!playback->Passed)
						{
							context.SetErrorData("replay", *serialized);
							return std::unexpected(DescribeReplayFailure(*playback));
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

			[[nodiscard]] std::string GetPhase() const override
			{
				std::string_view stage = "Play:replay";
				if (m_Stage == Stage::Verification)
					stage = "Record:verification";
				else if (m_Stage == Stage::InputCheck)
					stage = "Record:input check";
				return std::format("{} {}/{}", stage, m_Tick, m_Replay->FinalTick);
			}
		private:
			// What the playback is: input.replay's, or one of input.record stop's two, in their order.
			enum class Stage : uint8_t
			{
				Replay,
				Verification,
				InputCheck
			};

			[[nodiscard]] Status Begin(PlaySession& session, const ReplayPlaybackOptions& options)
			{
				m_Serial = session.GetSerial();
				m_Tick = session.GetTick();
				m_WithoutInput = options.WithoutInput;
				m_Released = false;
				ENGINE_TRY(m_Player.Begin(m_Replay, session, options));
				session.SetStepping(true);
				if (m_Stage == Stage::Verification)
					NoteScriptErrors(session);
				return {};
			}

			// input.replay's failed playback. M17 friction F03: with the replay's input, a checkpoint or final hash that differs
			// says why it can and how to fix it (GetReplayDivergenceHint); an expectation that fails while the hash matches gets
			// no hint. withoutInput is the input check, whose failure is the expected outcome for a replay that depends on its
			// input: no hint, and the message says the replay ran without its input events. A playback that continued past its
			// failed checkpoints (continuePastCheckpoint) names the first of them, and with the replay's input the message ends
			// with the first change its start fingerprint names (Docs/Decisions/0030-m19-contract.md decisions 2 and 3).
			[[nodiscard]] Error DescribeReplayFailure(const ReplayPlaybackResult& playback) const
			{
				std::string message;
				bool diverged = false;
				if (playback.CheckpointFailed && playback.FirstCheckpointFailure.has_value())
				{
					const ReplayCheckpointFailure& first = *playback.FirstCheckpointFailure;
					std::string end;
					if (!playback.StopReason.empty())
						end = std::format("then the replay stopped at tick {}: {}", playback.FinalTick, playback.StopReason);
					else if (playback.HashMatched)
						end = std::format("the final state hash at tick {} matched", playback.FinalTick);
					else
						end = std::format("the final state hash at tick {} is {}, recorded {}", playback.FinalTick, playback.StateHash, playback.ExpectedStateHash);
					message = std::format("replay verification failed at checkpoint tick {}: expected state hash {}, observed {}; {} checkpoint(s) differed, and {}", first.Tick,
						first.ExpectedStateHash, first.StateHash, playback.CheckpointsFailed, end);
					diverged = true;
				}
				else if (playback.CheckpointFailed)
				{
					message = std::format("replay verification failed at checkpoint tick {}: expected state hash {}, observed {}", playback.FinalTick, playback.ExpectedStateHash, playback.StateHash);
					diverged = true;
				}
				else if (!playback.StopReason.empty())
				{
					message = std::format("replay failed: {}", playback.StopReason);
				}
				else
				{
					// Otherwise an expectation or the final hash failed.
					message = std::format("replay verification failed at tick {}", playback.FinalTick);
					diverged = playback.HashChecked && !playback.HashMatched;
				}
				if (m_WithoutInput)
					return Error(ErrorCode::Validation, std::format("{}; {}", message, ReplayWithoutInputNote));
				if (!playback.Changes.empty())
					message = std::format("{}; changed since the recording: {}", message, playback.Changes.front());
				// An empty hint is none.
				return Error(ErrorCode::Validation, std::move(message)).WithHint(diverged ? std::string(GetReplayDivergenceHint()) : std::string());
			}

			// The verification's script errors (A1). The session RestartForReplay created for it is fresh, so its own stream
			// holds every error of the playback, OnStart's included, as test.run's ordinary-play verification counts them
			// (FeatureTestRunner.cpp); the first is the session's first occurrence (GetFirstScriptError), since the stream moves
			// a repeated error behind the others, even within one tick. Teardown's (OnDestroy) are counted apart, once the
			// playback passed (StopVerificationScripts).
			void NoteScriptErrors(const PlaySession& session)
			{
				m_ScriptErrors = session.GetScriptErrors().GetCursor();
				if (!m_FirstScriptError.has_value())
					m_FirstScriptError = session.GetFirstScriptError();
			}

			// Why the verification refuses the candidate (Docs/Decisions/0027-m17-contract.md "Frozen interfaces / A1"): the first
			// failure in tick order, an expectation at a tick before that tick's checkpoint, and script errors only when the
			// playback otherwise passed; nullopt when it passed without any.
			[[nodiscard]] std::optional<std::string> DescribeVerificationFailure(const ReplayPlaybackResult& playback) const
			{
				// Every evaluated expectation is at or before the tick where the playback ended, so the first one that failed
				// comes before any failure at that tick; the ones a stop left unevaluated failed because of the stop.
				for (size_t index = 0; index < playback.Expect.size(); ++index)
				{
					const ReplayExpectationOutcome& outcome = playback.Expect[index];
					if (!outcome.Evaluated || outcome.Satisfied)
						continue;
					const std::string expectation = std::format("expectation {0} at tick {1} (/expect/{0}/luau)", index, outcome.Tick);
					// A false or nil result is a Validation failure; an expectation that raised keeps its error's code.
					if (outcome.Failure.has_value() && outcome.Failure->GetCode() != ErrorCode::Validation)
						return std::format("{} failed: {}", expectation, outcome.Failure->GetMessageText());
					return std::format("{} is false", expectation);
				}
				if (playback.CheckpointFailed)
					return std::format("the state hash at checkpoint tick {} is {}, recorded {}", playback.FinalTick, playback.StateHash, playback.ExpectedStateHash);
				if (!playback.StopReason.empty())
					return std::format("playback stopped at tick {}: {}", playback.FinalTick, playback.StopReason);
				// With every expectation satisfied and no stop, only the final hash can fail the playback (StrictHash).
				if (!playback.Passed)
					return std::format("the final state hash at tick {} is {}, recorded {}", playback.FinalTick, playback.StateHash, playback.ExpectedStateHash);
				if (m_ScriptErrors != 0)
				{
					const ScriptError& first = m_FirstScriptError.value_or(ScriptError{});
					return std::format("the playback raised {} script error(s); the first: {}:{}: {} (tick {})", m_ScriptErrors, first.Script, first.Line, first.Message, first.Tick);
				}
				return std::nullopt;
			}

			// Docs/Decisions/0030-m19-contract.md decision 5 (AL-TEARDOWN): once the verification reached its final tick with
			// nothing else failed (DescribeVerificationFailure), its session's scripts are stopped (ScriptEngine::Stop, as test.run
			// does before it reads a replay's errors), so the errors their OnDestroy raises count like any other: the refusal's
			// reason, or nullopt when they raised none. The session then stays the verification's, paused at its final tick with
			// its scripts stopped.
			[[nodiscard]] Result<std::optional<std::string>> StopVerificationScripts(AutomationMethodContext& context) const
			{
				PlaySession* session = context.GetPlaySession();
				if (session == nullptr || session->GetSerial() != m_Serial)
					return MakeError(ErrorCode::Cancelled, "the replay session ended or was replaced");
				ScriptEngine* scripts = session->GetScripts();
				if (scripts == nullptr)
					return std::optional<std::string>();
				scripts->Stop();
				const uint64_t raised = session->GetScriptErrors().GetCursor();
				if (raised == m_ScriptErrors)
					return std::optional<std::string>();
				// None was raised before (DescribeVerificationFailure), so the session's first is the first of teardown.
				const ScriptError first = session->GetFirstScriptError().value_or(ScriptError{});
				return std::optional<std::string>(std::format("the playback raised {} script error(s); the first: {}:{}: {} (at teardown)", raised - m_ScriptErrors, first.Script, first.Line, first.Message));
			}

			// Writes the candidate that the verification reproduced and, with input events, the input check let through; the
			// result reports where that check ended (`checkTick`, 0 when none ran) and the replay's `warnings`.
			[[nodiscard]] Result<Json> WriteCandidate(AutomationMethodContext& context, uint64_t checkTick, const ReplayPlaybackResult* inputCheck) const
			{
				const ReplayDocument& candidate = m_Guard->Document;
				ENGINE_TRY_ASSIGN(std::string written, context.WriteReplay(m_Guard->Path, candidate));
				std::vector<std::string> warnings;
				if (inputCheck != nullptr && IsFinalHashIndependent(*inputCheck))
					warnings.push_back(MakeFinalHashIndependentWarning(written, inputCheck->FirstCheckpointFailure->Tick, candidate.FinalStateHash));
				return context.SerializeResult(MakeRecordResult(candidate, std::move(written), checkTick, std::move(warnings)));
			}

			// A1's refusal: nothing is written, and the play session stays the verification's, paused where it ended.
			[[nodiscard]] Result<Json> RefuseUnreproduced(AutomationMethodContext& context, std::string_view reason, const Json& outcome) const
			{
				context.SetErrorData("verification", outcome);
				return std::unexpected(Error(ErrorCode::Validation, std::format("refusing to write '{}': replayed with its input, the recording does not reproduce itself: {}; nothing was written", m_Guard->Path, reason))
						.WithLocation({ .File = {}, .JsonPointer = "/path", .Entity = {} })
						.WithHint(std::string(RecordingNotReproducedHint)));
			}

			// input.record stop's second playback, after a verification that passed: the recording guard (the candidate without
			// its input). Errors carry the input check's context.
			[[nodiscard]] Status StartInputCheck(AutomationMethodContext& context)
			{
				m_Stage = Stage::InputCheck;
				if (Status started = Restart(context, { .Verify = true, .StrictHash = true, .WithoutInput = true, .ContinuePastCheckpoint = true }); !started)
					return std::unexpected(std::move(started).error().WithContext(std::format("while starting the input check of '{}'; nothing was written", m_Guard->Path)));
				return {};
			}

			[[nodiscard]] InputReplayResult MakeReplayResult(const ReplayPlaybackResult& playback) const
			{
				InputReplayResult result{ .Passed = playback.Passed, .Verified = playback.Verified, .HashChecked = playback.HashChecked, .HashMatched = playback.HashMatched, .FinalTick = ToAutomationCounter(playback.FinalTick), .StateHash = playback.StateHash, .ExpectedStateHash = playback.ExpectedStateHash, .StopReason = playback.StopReason, .CheckpointsChecked = ToAutomationCounter(playback.CheckpointsChecked), .CheckpointFailed = playback.CheckpointFailed, .WithoutInput = m_WithoutInput, .CheckpointsFailed = ToAutomationCounter(playback.CheckpointsFailed), .FirstFailedCheckpointTick = ToAutomationCounter(playback.FirstCheckpointFailure.has_value() ? playback.FirstCheckpointFailure->Tick : 0), .Changes = playback.Changes, .Expect = {} };
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

			// An error of the operation; input.record stop's two playbacks add which one failed and that nothing was written.
			[[nodiscard]] Result<Json> Failed(Error error) const
			{
				if (m_Stage == Stage::Verification)
					return std::unexpected(std::move(error).WithContext(std::format("while verifying '{}' with its input; nothing was written", m_Guard->Path)));
				if (m_Stage == Stage::InputCheck)
					return std::unexpected(std::move(error).WithContext(std::format("while checking that '{}' depends on its input; nothing was written", m_Guard->Path)));
				return std::unexpected(std::move(error));
			}

			// The input check's verdict (ReplayMethods.h): a playback without input that passes the candidate's verification
			// (every checkpoint, the final hash, every expectation), whatever script errors it raised, shows its outcome does
			// not depend on its input, so nothing is written; otherwise the candidate is, with the warning of a check that
			// failed only at its checkpoints (IsFinalHashIndependent).
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
				return WriteCandidate(context, GetInputCheckTick(playback), &playback);
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
			Stage m_Stage = Stage::Replay;
			ReplayPlayer m_Player{};
			uint64_t m_Serial = 0;
			uint64_t m_Tick = 0;
			uint64_t m_ScriptErrors = 0;                     // the verification session's script error occurrences so far
			std::optional<ScriptError> m_FirstScriptError{}; // the first of them, as it appeared
			bool m_WithoutInput = false;
			bool m_Released = false;
		};

		// input.record's Start: the outcome is known when the handler returns, and the first Poll (in the same dispatch)
		// resolves it.
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
			// The play session restarts for the candidate, played with its input (the verification), then, when it has input
			// events, without it (the recording guard), and the candidate is written only when the first playback reproduces it
			// and the second does not. A pure determinism replay has nothing to depend on, so it is written after its
			// verification (Docs/Decisions/0030-m19-contract.md decision 4). The recording has ended; a failure from here on
			// writes nothing. The candidate carries no start fingerprint: nothing changed since it was recorded.
			auto replay = CreateRef<ReplayData>();
			replay->Header = document.Header;
			replay->Events = document.Events;
			replay->Expect = std::move(compiledExpectations);
			replay->Checkpoints = document.Checkpoints;
			replay->FinalTick = document.FinalTick;
			replay->FinalStateHash = document.FinalStateHash;
			auto operation = CreateScope<ReplayMethodOperation>(AssetRef<ReplayData>(std::move(replay)), RecordingCandidate{ .Document = std::move(document), .Path = params.Path });
			ENGINE_TRY(operation->StartVerification(context));
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
			// Docs/Decisions/0030-m19-contract.md decision 3: only compared checkpoints can be continued past.
			if (params.ContinuePastCheckpoint && !params.StrictHash)
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/continuePastCheckpoint", "continuePastCheckpoint requires strictHash:true", {}));
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
			ENGINE_TRY(operation->Restart(context, { .Verify = params.Verify, .StrictHash = params.StrictHash, .WithoutInput = params.WithoutInput, .NameDivergenceCauses = true, .ContinuePastCheckpoint = params.ContinuePastCheckpoint }));
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
			.Field("inputCheckTick", &InputRecordResult::InputCheckTick, "Stop with input events: the tick of the first checkpoint at which the input check (the recording replayed without its input events, continuing past checkpoints that differ) differed, else the tick where that run ended; the play session waits paused where the check ended, the replay's final tick unless it stopped early. 0 when no check ran.")
			.Field("warnings", &InputRecordResult::Warnings, "Stop: notes on the written replay; empty on Start. The input check warns when its run without input differed only at checkpoints but reached the recorded final state hash: the final hash then proves only the end state, and only the checkpoints prove the play.");
		registry.Struct<InputReplayParams>("InputReplayParams", "Replay an asset in a fresh deterministic session.")
			.Field("path", &InputReplayParams::Path, "Replay asset path, or a Runtime recording's returned user://Replays identity.")
			.Field("verify", &InputReplayParams::Verify, "Evaluate cooked expectations.")
			.Field("strictHash", &InputReplayParams::StrictHash, "Compare the recorded state hash at every checkpoint and at the final tick; requires verify. The first checkpoint that differs ends the playback there, unless continuePastCheckpoint.")
			.Field("withoutInput", &InputReplayParams::WithoutInput, "Apply none of the replay's input events: the check that its outcome depends on its input; requires verify. A replay whose verification still passes this way proves nothing about its input.")
			.Field("continuePastCheckpoint", &InputReplayParams::ContinuePastCheckpoint, "Play on past every checkpoint whose hash differs, to the final tick, and compare the final hash too; requires strictHash. The replay still fails when any checkpoint differed (checkpointsFailed, firstFailedCheckpointTick). With withoutInput it tells a replay whose final hash alone does not depend on its input.");
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
			.Field("checkpointFailed", &InputReplayResult::CheckpointFailed, "A checkpoint's hash differed. Without continuePastCheckpoint playback stopped at its tick (finalTick), and stateHash and expectedStateHash are the observed and recorded hashes there; with it, playback went on to the final tick and firstFailedCheckpointTick names the first.")
			.Field("withoutInput", &InputReplayResult::WithoutInput, "None of the replay's input events were applied.")
			.Field("checkpointsFailed", &InputReplayResult::CheckpointsFailed, "continuePastCheckpoint: the checkpoints whose hash differed; 0 otherwise.")
			.Field("firstFailedCheckpointTick", &InputReplayResult::FirstFailedCheckpointTick, "continuePastCheckpoint: the tick of the first checkpoint whose hash differed; 0 when none differed or without continuePastCheckpoint.")
			.Field("changes", &InputReplayResult::Changes, "When a hash differed in a playback with the replay's input: what changed since the recording, by the replay's start fingerprint (an entity, a component of an entity, a script, a material or another referenced asset), one line each; empty for a replay recorded without a fingerprint.")
			.Field("expect", &InputReplayResult::Expect, "Every expectation outcome in authored order.");
		registry.Struct<InputReadReplayParams>("InputReadReplayParams", "Read a replay back, its input events a page at a time.")
			.Field("path", &InputReadReplayParams::Path, "Required: the project's .replay source, project-relative or project://.")
			.Field("cursor", &InputReadReplayParams::Cursor, "Opaque cursor of the next page of events; empty for the first.")
			.Field("limit", &InputReadReplayParams::Limit, "Events per page, 1 to 1000.", FieldMeta{ .Min = 1.0, .Max = 1000.0 });
		registry.Struct<InputReadReplayCheckpoint>("InputReadReplayCheckpoint", "One state-hash checkpoint of a replay.")
			.Field("tick", &InputReadReplayCheckpoint::Tick, "Completed-tick boundary.")
			.Field("stateHash", &InputReadReplayCheckpoint::StateHash, "The recorded state hash there.");
		registry.Struct<InputReadReplayResult>("InputReadReplayResult", "A replay read back: its header, a page of its input events in play.step's input form, its expectations and checkpoints.")
			.Field("path", &InputReadReplayResult::Path, "Canonical project-relative path.")
			.Field("formatVersion", &InputReadReplayResult::FormatVersion, "The file's own format version, before migration.")
			.Field("header", &InputReadReplayResult::Header, "The header as the file writes it: Scene {Handle, Path}, Parameters, Seed, FixedHz, EngineVersion and Config.")
			.Field("finalTick", &InputReadReplayResult::FinalTick, "Completed ticks the replay runs.")
			.Field("finalStateHash", &InputReadReplayResult::FinalStateHash, "The recorded state hash at finalTick.")
			.Field("eventCount", &InputReadReplayResult::EventCount, "Every input event of the replay.")
			.Field("events", &InputReadReplayResult::Events, "This page of events, in play.step's input form (camelCase, only the members each type reads) with each tick the replay's absolute tick: play.step {ticks: finalTick, input: events} from tick 0 of the header's session applies them when they fit one page.")
			.Field("nextCursor", &InputReadReplayResult::NextCursor, "Cursor of the next page; empty after the last event.")
			.Field("expect", &InputReadReplayResult::Expect, "The replay's expectations, in the form input.record's and test.run's expect take.")
			.Field("checkpoints", &InputReadReplayResult::Checkpoints, "The replay's state-hash checkpoints.")
			.Field("hasFingerprint", &InputReadReplayResult::HasFingerprint, "The replay carries a start fingerprint, so a failed replay names what changed since its recording.");
	}

	void RegisterReplayMethods(MethodRegistry& methods)
	{
		methods.AddPending<AutomationMethodContext, InputRecordParams, InputRecordResult>(
			{ .Name = "input.record", .Description = "Starts a tick-zero recording or saves its validated applied inputs, expectations and state-hash checkpoints (one every 60 ticks). Stop checks output write permission. Every recording is first replayed with its input events, if any (the verification, which replaces the play session as input.replay does): unless that run reproduces every checkpoint, the final hash and every expectation and raises no script error, Stop refuses to write it (Validation naming the first divergence, the run's outcome in error data verification), which catches a script.eval that wrote script state during the recording. Once the run passed, its scripts are stopped, and an error their OnDestroy raises refuses the recording too (named at teardown). A recording without input events is then written, and the play session is the verification's, paused at its final tick with its scripts stopped (script.eval in the play context is refused there; scene.query and entity.get read its final state). One with input events is then replayed without them (the input check); when that run also reproduces every checkpoint, the final hash and every expectation, whatever script errors it raises, the recording's outcome does not depend on its input and Stop refuses to write it (Validation, the check's outcome in error data inputCheck). The play session is then the last run's, paused where it ended. End such a recording while the played state is still in the scene.", .RequiredParams = { "action" }, .ExposeAsTool = true, .AvailableInRuntime = true, .TimeoutSeconds = 600, .Examples = { { .Description = "Start a fresh recording.", .Params = Json{ { "action", "Start" } } }, { .Description = "Save the recording.", .Params = Json{ { "action", "Stop" }, { "path", "Assets/Tests/Recorded.replay" } } } } }, &Automation::InputRecord);
		methods.AddPending<AutomationMethodContext, InputReplayParams, InputReplayResult>(
			{ .Name = "input.replay", .Description = "Replays recorded inputs in a fresh session; verification failures retain every expectation and hash outcome in error data. strictHash compares the state hash at each checkpoint and at the final tick and, unless continuePastCheckpoint, stops at the first checkpoint that differs. The state hash covers the simulated world (entities and their serialized components, the tick, Random, id draws, physics), never script tables, so expectations observe script state. withoutInput replays without the input events, the check that the replay depends on its input. Afterwards the session stays paused at the replay's last tick (or where playback stopped), so script.eval, scene.query and entity.get read its final state; play.stop ends it.", .RequiredParams = { "path" }, .ExposeAsTool = true, .AvailableInRuntime = true, .TimeoutSeconds = 600, .Examples = { { .Description = "Verify a replay.", .Params = Json{ { "path", "Assets/Tests/Recorded.replay" }, { "verify", true }, { "strictHash", true } } }, { .Description = "Check that a replay's verification fails without its input.", .Params = Json{ { "path", "Assets/Tests/Recorded.replay" }, { "verify", true }, { "strictHash", true }, { "withoutInput", true } } } } }, &Automation::InputReplay);
		methods.Add(
			{ .Name = "input.readReplay", .Description = "Reads a replay back: its header, its input events a page at a time in play.step's input form with absolute ticks, its expectations and checkpoints. So a replay made with input.record can be re-recorded from its own events: start a recording of the header's scene, seed and parameters, play.step {ticks: finalTick, input: events}, and stop with its expectations.", .RequiredParams = { "path" }, .ExposeAsTool = true, .Examples = { { .Description = "Read the first page of a replay.", .Params = Json{ { "path", "Assets/Tests/Replays/Opening.replay" } } } } }, &Automation::InputReadReplay);
	}

}
