#include "EnginePCH.h"
#include "Engine/Session/ReplayPlayer.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Session/PlayInputEventCodec.h"
#include "Engine/Session/PlaySession.h"
#include "Engine/Session/ReplayFingerprint.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace Engine {

	namespace {

		// M17 friction F03 (ReplayPlayer.h): the hint contains the causes word for word.
		constexpr std::string_view ReplayDivergenceCauses = "a serialized component of the scene (the look's included), a script, the game's native module, "
															"an asset or the engine changed since it was recorded, or the play is not deterministic";
		constexpr std::string_view ReplayDivergenceHint = "the world differs from the recording: a serialized component of the scene (the look's "
														  "included), a script, the game's native module, an asset or the engine changed since it was "
														  "recorded, or the play is not deterministic; re-record it with test.run {record} (the "
														  "game-building skill, \"Re-recording replays\")";
		static_assert(ReplayDivergenceHint.find(ReplayDivergenceCauses) != std::string_view::npos);
		// What a checkpoint that differs means in a playback WithoutInput, the input check: the expected outcome of a replay
		// whose verification depends on its input, never a reason to re-record it, so the causes are not named there.
		constexpr std::string_view ReplayWithoutInputNote = "the replay ran without its input events, which a replay that depends on its input is expected to fail";

		// What a failed checkpoint's stop reason ends with when the start fingerprints name a change (Docs/Decisions/
		// 0030-m19-contract.md decision 2).
		constexpr std::string_view ChangedSinceRecording = "; changed since the recording: ";
		// The one change line of a playback whose own start fingerprint could not be taken at Begin, before that error.
		constexpr std::string_view FingerprintNotTaken = "not known, because the playback's start fingerprint could not be taken: ";

	}

	std::string_view GetReplayDivergenceCauses()
	{
		return ReplayDivergenceCauses;
	}

	std::string_view GetReplayDivergenceHint()
	{
		return ReplayDivergenceHint;
	}

	std::string MakeFinalHashIndependentWarning(std::string_view replay, uint64_t checkpointTick, std::string_view finalStateHash)
	{
		return std::format("'{}': its final state hash does not depend on its input: without its input events the run first differed from the recording at "
						   "checkpoint tick {}, then reached the recorded final state hash {}, so only its checkpoints prove the play; end the recording while "
						   "the played state is still in the scene",
			replay, checkpointTick, finalStateHash);
	}

	bool IsFinalHashIndependent(const ReplayPlaybackResult& playback)
	{
		return playback.FirstCheckpointFailure.has_value() && playback.HashMatched && playback.StopReason.empty() && std::ranges::all_of(playback.Expect, [](const ReplayExpectationOutcome& outcome)
		{
			return outcome.Satisfied;
		});
	}

	struct ReplayPlayer::State
	{
		AssetRef<ReplayData> Replay{};
		ReplayPlaybackOptions Options{};
		ReplayPlaybackResult ReportData{};
		// The playback's own start fingerprint (ComputeReplayFingerprint at Begin), taken only with the replay's input for a
		// replay that has one: what ReplayPlaybackResult::Changes compares with the recording's when a hash differs.
		std::optional<ReplayFingerprint> Observed{};
		// Why Observed could not be taken (the error's text), which Changes then names instead; empty otherwise.
		std::string FingerprintFailure{};
		std::vector<StampedPlayInputEvent> Events{};
		uint64_t Serial = 0;
		uint64_t Tick = 0;
		size_t NextExpectation = 0;
		size_t NextEvent = 0;
		size_t NextCheckpoint = 0;
		bool Begun = false;
		bool Complete = false;

		static ReplayExpectationOutcome MakeOutcome(const ReplayBytecodeExpectation& expectation)
		{
			ReplayExpectationOutcome outcome;
			outcome.Tick = expectation.Tick;
			outcome.File = expectation.Script.SourceMap.Path;
			if (outcome.File.empty() && expectation.Script.SourceMap.ChunkName.size() > 1)
				outcome.File = expectation.Script.SourceMap.ChunkName.substr(1);
			return outcome;
		}

		void EvaluateBoundary(PlaySession& session, IReplayEvaluator& evaluator)
		{
			while (NextExpectation < Replay->Expect.size() && Replay->Expect[NextExpectation].Tick == Tick)
			{
				const auto& expectation = Replay->Expect[NextExpectation++];
				ReplayExpectationOutcome outcome = MakeOutcome(expectation);
				if (Options.Verify)
				{
					outcome.Evaluated = true;
					auto value = evaluator.Evaluate(session, expectation);
					if (!value)
					{
						outcome.Failure = std::move(value).error();
						outcome.File = outcome.Failure->GetLocation().File;
						outcome.Line = outcome.Failure->GetLocation().Line;
					}
					else
					{
						outcome.Value.Set(std::move(*value));
						const auto& json = outcome.Value.Get();
						outcome.Satisfied = !json.is_null() && (!json.is_boolean() || JsonReader(json).ReadBool().value_or(false));
						if (!outcome.Satisfied)
						{
							outcome.Line = 1;
							outcome.Failure = Error(ErrorCode::Validation, "replay expectation evaluated to false or nil").WithLocation({ .File = outcome.File, .Line = outcome.Line, .JsonPointer = expectation.Script.SourceMap.JsonPointer, .Entity = {} });
						}
					}
				}
				ReportData.Expect.push_back(std::move(outcome));
			}
		}

		// The completed playback at the boundary reached (Tick). `stopReason` is empty at FinalTick; otherwise playback
		// ended early and has failed, and every expectation it never reached is reported unevaluated. `failed` is the
		// checkpoint whose hash differed (its observed hash is `observed`), when that ended it. When a hash differed (that
		// checkpoint, a checkpoint the playback continued past, or the final hash), the result names what changed since the
		// recording by the start fingerprints, and a failed checkpoint's stop reason the first change.
		void Finish(const PlaySession& session, std::string stopReason, const ReplayCheckpoint* failed = nullptr, std::string observed = {})
		{
			auto& result = ReportData;
			result.FinalTick = Tick;
			result.StateHash = failed != nullptr ? std::move(observed) : UUID(session.ComputeStateHash()).ToString();
			result.ExpectedStateHash = failed != nullptr ? failed->StateHash : Replay->FinalStateHash;
			result.CheckpointFailed = failed != nullptr || result.CheckpointsFailed > 0;
			result.Verified = Options.Verify;
			result.HashChecked = Options.StrictHash;
			result.HashMatched = stopReason.empty() && result.StateHash == result.ExpectedStateHash;
			const bool finalHashDiffered = stopReason.empty() && !result.HashMatched;
			if (Replay->Fingerprint.has_value() && (result.CheckpointFailed || finalHashDiffered))
			{
				if (Observed.has_value())
					result.Changes = DescribeReplayFingerprintChanges(*Replay->Fingerprint, *Observed);
				else if (!FingerprintFailure.empty())
					result.Changes = { std::format("{}{}", FingerprintNotTaken, FingerprintFailure) };
				if (failed != nullptr && !result.Changes.empty() && Options.NameDivergenceCauses)
					stopReason += std::format("{}{}", ChangedSinceRecording, result.Changes.front());
			}
			while (NextExpectation < Replay->Expect.size())
			{
				const auto& expectation = Replay->Expect[NextExpectation++];
				ReplayExpectationOutcome outcome = MakeOutcome(expectation);
				outcome.Line = 1;
				outcome.Failure = Error(ErrorCode::Validation, std::format("the replay stopped at tick {} before this expectation's tick {}: {}", Tick, expectation.Tick, stopReason))
									  .WithLocation({ .File = outcome.File, .Line = outcome.Line, .JsonPointer = expectation.Script.SourceMap.JsonPointer, .Entity = {} });
				result.Expect.push_back(std::move(outcome));
			}
			result.StopReason = std::move(stopReason);
			result.Passed = result.StopReason.empty() && result.CheckpointsFailed == 0 && (!result.HashChecked || result.HashMatched)
				&& (!result.Verified || std::all_of(result.Expect.begin(), result.Expect.end(), [](const auto& item)
			{
				return item.Satisfied;
			}));
			Complete = true;
		}

		// StrictHash: compares the checkpoint of the boundary reached (Tick), if the replay has one there; a different hash
		// completes the playback as failed (ReplayPlaybackResult::CheckpointFailed) and returns true, or with
		// ContinuePastCheckpoint is counted (the first one kept) while the playback goes on. The stop reason names the
		// divergence causes only for a playback with the replay's input that names them (NameDivergenceCauses); WithoutInput
		// it says the input was left out.
		bool CheckCheckpoint(const PlaySession& session)
		{
			if (!Options.StrictHash || NextCheckpoint >= Replay->Checkpoints.size() || Replay->Checkpoints[NextCheckpoint].Tick != Tick)
				return false;
			const ReplayCheckpoint& checkpoint = Replay->Checkpoints[NextCheckpoint++];
			++ReportData.CheckpointsChecked;
			std::string observed = UUID(session.ComputeStateHash()).ToString();
			if (observed == checkpoint.StateHash)
				return false;
			if (Options.ContinuePastCheckpoint)
			{
				if (ReportData.CheckpointsFailed++ == 0)
					ReportData.FirstCheckpointFailure = ReplayCheckpointFailure{ .Tick = Tick, .StateHash = std::move(observed), .ExpectedStateHash = checkpoint.StateHash };
				return false;
			}
			std::string reason = std::format("the state hash at checkpoint tick {} is {}, but the recording's is {}", Tick, observed, checkpoint.StateHash);
			if (Options.WithoutInput)
				reason += std::format(": {}", ReplayWithoutInputNote);
			else if (Options.NameDivergenceCauses)
				reason += std::format(": the simulation diverged from the recording by that tick, because {}", ReplayDivergenceCauses);
			Finish(session, std::move(reason), &checkpoint, std::move(observed));
			return true;
		}

		// Why `session` refuses to advance before FinalTick, when gameplay ended it: a quit request or a fatal stop.
		std::optional<std::string> GetStopReason(const PlaySession& session) const
		{
			if (const auto& fatal = session.GetFatalError())
				return std::format("a fatal {} script error stopped the session at tick {}, before the replay's final tick {}: {}", ScriptErrorKindToString(fatal->Kind), Tick, Replay->FinalTick, fatal->Message);
			if (const auto quit = session.GetQuitRequest())
				return std::format("the game quit with exit code {} at tick {}, before the replay's final tick {}", *quit, Tick, Replay->FinalTick);
			return std::nullopt;
		}
	};

	ReplayPlayer::ReplayPlayer()
		: m_State(CreateScope<State>())
	{
	}
	ReplayPlayer::~ReplayPlayer() = default;

	Status ReplayPlayer::Begin(AssetRef<ReplayData> replay, const PlaySession& session, const ReplayPlaybackOptions& options)
	{
		if (m_State->Replay && !m_State->Complete)
			return MakeError(ErrorCode::InvalidState, "replay is already active");
		if (!replay || session.GetTick() != 0)
			return MakeError(ErrorCode::InvalidArgument, "replay needs data and a fresh session at tick zero");
		if (options.StrictHash && !options.Verify)
			return MakeError(ErrorCode::InvalidArgument, "strictHash requires verify");
		// Docs/Decisions/0030-m19-contract.md decision 3: only compared checkpoints can be continued past.
		if (options.ContinuePastCheckpoint && !options.StrictHash)
			return MakeError(ErrorCode::InvalidArgument, "continuePastCheckpoint requires strictHash");
		ReplayDocument document;
		document.Header = replay->Header;
		document.Events = replay->Events;
		document.Checkpoints = replay->Checkpoints;
		document.FinalTick = replay->FinalTick;
		document.FinalStateHash = replay->FinalStateHash;
		document.Fingerprint = replay->Fingerprint;
		for (const auto& expectation : replay->Expect)
		{
			if (expectation.Script.Kind != ScriptKind::Module || expectation.Script.Bytecode.empty())
				return MakeError(ErrorCode::Validation, "replay expectation needs compiled Module bytecode");
			document.Expect.push_back({ expectation.Tick, "compiled" });
		}
		ENGINE_TRY(ValidateReplayDocument(document));
		const auto& header = replay->Header;
		if (header.Seed != session.GetSeed() || header.FixedHz != session.GetProjectSettings().Simulation.FixedHz || (header.Parameters.IsNull() ? Json::object() : header.Parameters.Get()) != session.GetLoadParameters())
			return MakeError(ErrorCode::InvalidArgument, "replay header does not match session parameters, seed or fixed rate");
		std::vector<StampedPlayInputEvent> events;
		for (size_t i = 0; i < replay->Events.size(); ++i)
		{
			ENGINE_TRY_ASSIGN(auto event, DecodeReplayEvent(replay->Events[i], session.GetInput().GetActions(), std::format("/Events/{}", i)));
			events.push_back(std::move(event));
		}
		if (options.WithoutInput)
			events.clear();
		// A playback without its input is expected to differ, so it names no change and takes no fingerprint (decision 2).
		// The fingerprint only explains a hash that differs, so a playback whose fingerprint cannot be taken (an asset the
		// scene references whose import fails, which the simulation may never load) still plays, and names that error then.
		std::optional<ReplayFingerprint> observed;
		std::string fingerprintFailure;
		if (!options.WithoutInput && replay->Fingerprint.has_value())
		{
			Result<ReplayFingerprint> fingerprint = ComputeReplayFingerprint(session.GetScene(), session.GetScriptSchemas(), session.GetAssets());
			if (fingerprint.has_value())
				observed = std::move(*fingerprint);
			else
				fingerprintFailure = fingerprint.error().ToString();
		}
		*m_State = {};
		m_State->Replay = std::move(replay);
		m_State->Observed = std::move(observed);
		m_State->FingerprintFailure = std::move(fingerprintFailure);
		m_State->Options = options;
		m_State->Events = std::move(events);
		m_State->Serial = session.GetSerial();
		return {};
	}

	Result<bool> ReplayPlayer::Advance(PlaySession& session, IReplayEvaluator& evaluator)
	{
		if (!m_State->Replay)
			return MakeError(ErrorCode::InvalidState, "no replay is active");
		if (m_State->Complete)
			return true;
		if (session.GetSerial() != m_State->Serial || session.GetTick() != m_State->Tick)
		{
			Cancel();
			return MakeError(ErrorCode::Cancelled, "replay session or tick changed");
		}
		if (!m_State->Begun)
		{
			m_State->Begun = true;
			m_State->EvaluateBoundary(session, evaluator);
		}
		if (m_State->Tick < m_State->Replay->FinalTick)
		{
			// Keep future authored input here so Cancel cannot leave it in the surviving session. A Tap's generated
			// release was queued on the previous tick and still precedes this tick's authored events, as before.
			while (m_State->NextEvent < m_State->Events.size() && m_State->Events[m_State->NextEvent].Tick == m_State->Tick)
			{
				const auto& event = m_State->Events[m_State->NextEvent];
				ENGINE_TRY(session.GetInput().Queue(event.Tick, event.Event));
				++m_State->NextEvent;
			}
			session.Tick();
			if (session.GetTick() != m_State->Tick + 1)
			{
				// Gameplay that quits or stops fatally before FinalTick is a failed replay with its outcome, not a cancellation.
				if (session.GetTick() == m_State->Tick)
				{
					if (std::optional<std::string> reason = m_State->GetStopReason(session))
					{
						m_State->Finish(session, std::move(*reason));
						return true;
					}
				}
				Cancel();
				return MakeError(ErrorCode::Cancelled, "replay tick failed to advance");
			}
			++m_State->Tick;
			m_State->EvaluateBoundary(session, evaluator);
			if (m_State->CheckCheckpoint(session))
				return true;
		}
		if (m_State->Tick == m_State->Replay->FinalTick)
			m_State->Finish(session, {});
		return m_State->Complete;
	}

	Result<ReplayPlaybackResult> ReplayPlayer::GetResult() const
	{
		if (!m_State->Complete)
			return MakeError(ErrorCode::InvalidState, "replay has not completed");
		return m_State->ReportData;
	}

	void ReplayPlayer::Cancel()
	{
		*m_State = {};
	}

}
