#include "EnginePCH.h"
#include "Engine/Session/ReplayPlayer.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Session/PlayInputEventCodec.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <string>

namespace Engine {

	struct ReplayPlayer::State
	{
		AssetRef<ReplayData> Replay{};
		ReplayPlaybackOptions Options{};
		ReplayPlaybackResult ReportData{};
		std::vector<StampedPlayInputEvent> Events{};
		uint64_t Serial = 0;
		uint64_t Tick = 0;
		size_t NextExpectation = 0;
		size_t NextEvent = 0;
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
		// ended early and has failed, and every expectation it never reached is reported unevaluated.
		void Finish(const PlaySession& session, std::string stopReason)
		{
			auto& result = ReportData;
			result.FinalTick = Tick;
			result.StateHash = UUID(session.ComputeStateHash()).ToString();
			result.ExpectedStateHash = Replay->FinalStateHash;
			result.Verified = Options.Verify;
			result.HashChecked = Options.StrictHash;
			result.HashMatched = stopReason.empty() && result.StateHash == result.ExpectedStateHash;
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
			result.Passed = result.StopReason.empty() && (!result.HashChecked || result.HashMatched) && (!result.Verified || std::all_of(result.Expect.begin(), result.Expect.end(), [](const auto& item)
			{
				return item.Satisfied;
			}));
			Complete = true;
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
		ReplayDocument document;
		document.Header = replay->Header;
		document.Events = replay->Events;
		document.FinalTick = replay->FinalTick;
		document.FinalStateHash = replay->FinalStateHash;
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
		*m_State = {};
		m_State->Replay = std::move(replay);
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
