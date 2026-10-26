#include "EnginePCH.h"
#include "Engine/Session/ReplayRecorder.h"

#include "Engine/Session/PlayInputEventCodec.h"
#include "Engine/Session/PlaySession.h"
#include "Engine/Session/ReplayFingerprint.h"

#include <nlohmann/json.hpp>

#include <format>
#include <limits>

namespace Engine {

	struct ReplayRecorder::State
	{
		bool Recording = false;
		uint64_t Serial = 0;
		uint64_t NextTick = 0;
		ReplayDocument Document{};
		std::string InvalidReason{};
	};

	ReplayRecorder::ReplayRecorder()
		: m_State(CreateScope<State>())
	{
	}
	ReplayRecorder::~ReplayRecorder() = default;

	Status ReplayRecorder::Begin(const ReplayHeader& header, const PlaySession& session)
	{
		if (m_State->Recording || session.GetTick() != 0)
			return MakeError(ErrorCode::InvalidState, "recording requires an idle recorder and a session at tick zero");
		ReplayDocument document;
		document.Header = header;
		document.FinalStateHash = "0000000000000000";
		ENGINE_TRY(ValidateReplayDocument(document));
		const Json parameters = header.Parameters.IsNull() ? Json::object() : header.Parameters.Get();
		if (header.Seed != session.GetSeed() || header.FixedHz != session.GetProjectSettings().Simulation.FixedHz || parameters != session.GetLoadParameters())
			return MakeError(ErrorCode::InvalidArgument, "recording header does not match the session parameters, seed or fixed rate");
		// What the session holds at tick 0, so a playback whose hash differs names what changed (Docs/Decisions/0030-m19-contract.md
		// decision 2).
		ENGINE_TRY_ASSIGN(document.Fingerprint, WithContext(ComputeReplayFingerprint(session.GetScene(), session.GetScriptSchemas(), session.GetAssets()), "while taking the recording's start fingerprint"));
		*m_State = {};
		m_State->Recording = true;
		m_State->Serial = session.GetSerial();
		m_State->Document = std::move(document);
		return {};
	}

	Status ReplayRecorder::CaptureAppliedInput(uint64_t tick, std::span<const PlayInputEvent> events)
	{
		if (!m_State->Recording)
			return MakeError(ErrorCode::InvalidState, "no recording is active");
		if (tick != m_State->NextTick || tick == std::numeric_limits<uint64_t>::max())
			Invalidate("applied input tick discontinuity");
		if (!m_State->InvalidReason.empty())
			return MakeError(ErrorCode::InvalidState, "recording invalidated: {}", m_State->InvalidReason);
		std::vector<ReplayEvent> converted;
		for (const auto& event : events)
		{
			auto encoded = EncodeReplayEvent(tick, event);
			if (!encoded)
			{
				Invalidate(encoded.error().ToString());
				return std::unexpected(std::move(encoded).error());
			}
			converted.push_back(std::move(*encoded));
		}
		for (auto& event : converted)
			m_State->Document.Events.push_back(std::move(event));
		++m_State->NextTick;
		return {};
	}

	void ReplayRecorder::CaptureCheckpoint(const PlaySession& session)
	{
		const uint64_t tick = session.GetTick();
		if (!m_State->Recording || !m_State->InvalidReason.empty() || tick != m_State->NextTick || tick == 0 || tick % ReplayCheckpointInterval != 0)
			return;
		m_State->Document.Checkpoints.push_back({ .Tick = tick, .StateHash = UUID(session.ComputeStateHash()).ToString() });
	}

	void ReplayRecorder::Invalidate(std::string_view reason)
	{
		if (m_State->Recording && m_State->InvalidReason.empty())
			m_State->InvalidReason = reason.empty() ? "external mutation" : std::string(reason);
	}

	Result<ReplayDocument> ReplayRecorder::Finish(const PlaySession& session, std::span<const ReplayExpectation> expectations)
	{
		if (!m_State->Recording)
			return MakeError(ErrorCode::InvalidState, "no recording is active");
		if (session.GetSerial() != m_State->Serial || session.GetTick() != m_State->NextTick)
			Invalidate("session or applied input timeline changed");
		if (!m_State->InvalidReason.empty())
		{
			// An invalidated recording can never be finished: end it, so the owner can record again (§13.6 restart).
			Error invalidated(ErrorCode::InvalidState, std::format("recording invalidated: {}", m_State->InvalidReason));
			Cancel();
			return std::unexpected(std::move(invalidated).WithHint("start a new recording with input.record {action: \"start\", restart: true}"));
		}
		ReplayDocument candidate = m_State->Document;
		candidate.Expect.assign(expectations.begin(), expectations.end());
		candidate.FinalTick = session.GetTick();
		candidate.FinalStateHash = UUID(session.ComputeStateHash()).ToString();
		const auto valid = ValidateReplayDocument(candidate);
		if (!valid)
			return std::unexpected(Error(ErrorCode::InvalidArgument, valid.error().GetMessageText()).WithLocation(valid.error().GetLocation()));
		Cancel();
		return candidate;
	}

	void ReplayRecorder::Cancel()
	{
		*m_State = {};
	}
	bool ReplayRecorder::IsRecording() const
	{
		return m_State->Recording;
	}

}
