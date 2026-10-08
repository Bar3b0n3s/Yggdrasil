#include "EnginePCH.h"
#include "Engine/Automation/Methods/PlayMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/Private/PlayMethodSupport.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Platform/Input/InputState.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		// play.step's operation (PlayMethods.h): ticks run from Poll, as many per frame as fit PlayStepFrameBudget on the
		// host's wall clock (AutomationMethodContext::GetWallClockTime).
		//
		// It holds no session pointer: every Poll and the Cancel resolve the host's session again and use it only when it is
		// the one the step started on: the same serial (PlaySession::GetSerial, which a host never repeats, unlike an
		// address a new session may reuse), still stepping and at the tick this step left it. A session that ended
		// meanwhile, or another that took its place, fails that test, so the step resolves with Cancelled.
		class PlayStepOperation final : public PendingOperation
		{
		public:
			PlayStepOperation(const PlaySession& session, uint32_t ticks, PlayStepRender render)
				: m_SessionSerial(session.GetSerial()), m_FirstTick(session.GetTick()), m_Ticks(ticks), m_Render(render), m_ExtractionsBefore(session.GetExtractionCount())
			{
			}

			[[nodiscard]] std::optional<Result<Json>> Poll(MethodContext& context) override
			{
				// The registry ran the handler on this host's AutomationMethodContext, and polls with the same context.
				auto& automation = static_cast<AutomationMethodContext&>(context);
				PlaySession* session = FindSession(automation);
				if (session == nullptr)
				{
					return std::optional<Result<Json>>(std::unexpected(
						Error(ErrorCode::Cancelled, std::format("the play session ended after {} of {} ticks of this play.step", m_Ran, m_Ticks))));
				}

				++m_Frames;
				const std::chrono::steady_clock::time_point start = automation.GetWallClockTime();
				do
				{
					// render: "every" extracts every tick (one per frame), "last" only the call's last tick, "none" no tick.
					const bool last = m_Ran + 1 == m_Ticks;
					session->SetExtractionEnabled(m_Render == PlayStepRender::Every || (m_Render == PlayStepRender::Last && last));
					session->Tick();
					++m_Ran;
				} while (m_Ran < m_Ticks && m_Render != PlayStepRender::Every && automation.GetWallClockTime() - start < PlayStepFrameBudget);

				if (m_Ran < m_Ticks)
					return std::nullopt;

				Release(*session);
				PlayStepResult result;
				result.Tick = ToAutomationCounter(session->GetTick());
				result.StateHash = Utils::FormatStateHash(session->ComputeStateHash());
				result.Ticks = m_Ticks;
				result.Frames = m_Frames;
				result.Rendered = ToAutomationCounter(session->GetExtractionCount() - m_ExtractionsBefore);
				return context.SerializeResult(result);
			}

			void Cancel(MethodContext& context) override
			{
				// The ticks already run stay (§13.2 "Disconnect"); the session is left as play.step found it otherwise.
				if (PlaySession* session = FindSession(static_cast<AutomationMethodContext&>(context)))
					Release(*session);
			}

			[[nodiscard]] std::string GetPhase() const override
			{
				return std::format("Play:step {}/{}", m_Ran, m_Ticks);
			}
		private:
			// The host's session when it is still the one this step runs on (see the class comment); nullptr otherwise.
			[[nodiscard]] PlaySession* FindSession(const AutomationMethodContext& context) const
			{
				PlaySession* session = context.GetPlaySession();
				if (session == nullptr || session->GetSerial() != m_SessionSerial || !session->IsStepping() || session->GetTick() != m_FirstTick + m_Ran)
					return nullptr;
				return session;
			}

			// Ends the step's hold on `session`: the host's loop is throttled again and every frame phase extracts.
			static void Release(PlaySession& session)
			{
				session.SetStepping(false);
				session.SetExtractionEnabled(true);
			}
		private:
			uint64_t m_SessionSerial = 0; // PlaySession::GetSerial of the session the step started on
			uint64_t m_FirstTick = 0;
			uint32_t m_Ticks = 0;
			PlayStepRender m_Render = PlayStepRender::Last;
			uint64_t m_ExtractionsBefore = 0;
			uint32_t m_Ran = 0;
			uint32_t m_Frames = 0;
		};

	}

	PlayRunState GetPlayRunState(const PlaySession* session)
	{
		if (session == nullptr)
			return PlayRunState::Edit;
		if (session->IsPaused())
			return PlayRunState::Paused;
		return session->GetMode() == PlayMode::Simulate ? PlayRunState::Simulate : PlayRunState::Play;
	}

	std::string_view PlayRunStateToString(PlayRunState state)
	{
		switch (state)
		{
			case PlayRunState::Edit:     return "Edit";
			case PlayRunState::Play:     return "Play";
			case PlayRunState::Simulate: return "Simulate";
			case PlayRunState::Paused:   return "Paused";
		}
		ENGINE_CORE_ASSERT(false, "unknown PlayRunState {}", std::to_underlying(state));
		return "Edit";
	}

	namespace Automation {

		PlayStateResult MakePlayStateResult(AutomationMethodContext& context)
		{
			PlayStateResult result;
			const PlaySession* session = context.GetPlaySession();
			if (session == nullptr)
				return result;

			result.Mode = session->GetMode();
			result.State = GetPlayRunState(session);
			result.Tick = ToAutomationCounter(session->GetTick());
			result.StateHash = Utils::FormatStateHash(session->ComputeStateHash());
			result.Lockstep = session->IsLockstep();
			if (result.Lockstep)
			{
				const ClientId owner = session->GetLockstepOwner();
				result.LockstepOwner = owner == NoClient ? std::string() : context.GetClientName(owner);
				result.OwnedByCaller = owner != NoClient && owner == context.GetRequest().Client;
			}
			result.TimeScale = static_cast<float>(session->GetTimeScale());
			result.Modified = session->IsModified();
			result.Recording = false;
			result.EntityCount = ToAutomationCounter(session->GetScene().GetEntityCount());
			result.MaxEntities = session->GetMaxEntities();
			result.Input = session->GetInput().GetSummary(InputPhase::Step);
			return result;
		}

		Result<PlayStateResult> PlayStart(AutomationMethodContext& context, const PlayStartParams& params)
		{
			// Members of later milestones are refused wherever they are present (ADR 0012 decision 16).
			if (context.HasParam("parameters"))
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::Unsupported, "/parameters",
					"play.start {parameters} arrives with scripts (M13): Scene.GetLoadParameters reads them", "remove parameters"));
			}
			if (context.HasParam("pauseOnError"))
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::Unsupported, "/pauseOnError",
					"play.start {pauseOnError} arrives with scripts (M13): only script errors pause play", "remove pauseOnError"));
			}

			PlayStartOptions options;
			options.Mode = params.Mode;
			options.Lockstep = params.Lockstep;
			options.LockstepOwner = params.Lockstep ? context.GetRequest().Client : NoClient;
			options.Seed = context.HasParam("seed") ? std::optional<uint64_t>(params.Seed) : std::nullopt;
			options.ScenePath = params.Scene;
			options.Paused = params.Paused;
			options.TimeScale = params.TimeScale;
			ENGINE_TRY(context.StartPlay(options));
			return MakePlayStateResult(context);
		}

		Result<PlayStopResult> PlayStop(AutomationMethodContext& context, const NoParams& /*params*/)
		{
			ENGINE_TRY_ASSIGN(PlaySession * session, Utils::RequirePlaySession(context));
			ENGINE_TRY(Utils::CheckLockstepOwner(context, *session));
			const PlayStopResult result{ .Tick = ToAutomationCounter(session->GetTick()), .StateHash = Utils::FormatStateHash(session->ComputeStateHash()) };
			ENGINE_TRY(context.StopPlay());
			return result;
		}

		Result<PlayStateResult> PlayPause(AutomationMethodContext& context, const NoParams& /*params*/)
		{
			ENGINE_TRY_ASSIGN(PlaySession * session, Utils::RequirePlaySession(context));
			ENGINE_TRY(Utils::CheckLockstepOwner(context, *session));
			const PlayRunState before = GetPlayRunState(session);
			// The owner's pause leaves lockstep, as its disconnect does (§13.2): the session then waits for play.resume or
			// play.step like any paused session. Paused first, so its voices pause before the audio engine's time goes back
			// to the device (PlaySession "Audio").
			session->SetPaused(true);
			session->SetLockstep(false);
			if (GetPlayRunState(session) != before)
				context.GetEventLog().Append(Utils::MakePlayStateChangedEvent(session));
			return MakePlayStateResult(context);
		}

		Result<PlayStateResult> PlayResume(AutomationMethodContext& context, const NoParams& /*params*/)
		{
			ENGINE_TRY_ASSIGN(PlaySession * session, Utils::RequirePlaySession(context));
			ENGINE_TRY(Utils::CheckLockstepOwner(context, *session));
			if (session->IsLockstep())
			{
				return std::unexpected(Error(ErrorCode::InvalidState, "lockstep sessions advance only through play.step")
						.WithHint("step it with play.step, or leave lockstep with play.pause first"));
			}
			if (session->IsStepping())
			{
				return std::unexpected(
					Error(ErrorCode::InvalidState, "a play.step is running on the paused session").WithHint("resume once its play.step has finished"));
			}
			const bool wasPaused = session->IsPaused();
			session->SetPaused(false);
			if (wasPaused)
				context.GetEventLog().Append(Utils::MakePlayStateChangedEvent(session));
			return MakePlayStateResult(context);
		}

		Result<Scope<PendingOperation>> PlayStep(AutomationMethodContext& context, const PlayStepParams& params)
		{
			ENGINE_TRY_ASSIGN(PlaySession * session, Utils::RequirePlaySession(context));
			ENGINE_TRY(Utils::CheckLockstepOwner(context, *session));
			if (!session->IsLockstep() && !session->IsPaused())
			{
				return std::unexpected(Error(ErrorCode::InvalidState, "the play session is running: play.step needs a paused session or lockstep")
						.WithHint("pause first with play.pause, or start with play.start {lockstep: true}"));
			}
			if (session->IsStepping())
			{
				return std::unexpected(
					Error(ErrorCode::InvalidState, "a play.step is already running on this play session").WithHint("wait for it to finish, then step again"));
			}
			if (params.Ticks < 1 || params.Ticks > MaxPlayStepTicks)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/ticks",
					std::format("ticks must be 1 to {} (got {})", MaxPlayStepTicks, params.Ticks), "split a longer run into several play.step calls"));
			}

			// The events are stamped from the first tick this call runs, and all are checked before any is queued.
			PlayInput& input = session->GetInput();
			std::vector<PlayInputEvent> events;
			events.reserve(params.Input.size());
			for (size_t index = 0; index < params.Input.size(); ++index)
			{
				const InputEventParams& event = params.Input[index];
				if (event.Tick >= params.Ticks)
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("/input/{}/tick", index),
						std::format("tick offset {} lies outside this play.step's {} ticks", event.Tick, params.Ticks),
						std::format("give an offset from 0 to {}", params.Ticks - 1)));
				}
				ENGINE_TRY_ASSIGN(PlayInputEvent converted, MakePlayInputEvent(context, event, std::format("/input/{}", index), input.GetActions()));
				events.push_back(std::move(converted));
			}
			const uint64_t first = session->GetTick();
			for (size_t index = 0; index < events.size(); ++index)
				ENGINE_TRY(input.Queue(first + params.Input[index].Tick, events[index]));

			session->SetStepping(true);
			return Scope<PendingOperation>(CreateScope<PlayStepOperation>(*session, params.Ticks, params.Render));
		}

		Result<PlayStateResult> PlayState(AutomationMethodContext& context, const NoParams& /*params*/)
		{
			return MakePlayStateResult(context);
		}

		Result<PlayStateResult> PlaySetTimeScale(AutomationMethodContext& context, const PlaySetTimeScaleParams& params)
		{
			ENGINE_TRY_ASSIGN(PlaySession * session, Utils::RequirePlaySession(context));
			ENGINE_TRY(Utils::CheckLockstepOwner(context, *session));
			const Status scaled = session->SetTimeScale(static_cast<double>(params.Scale));
			if (!scaled)
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/scale", scaled.error().GetMessageText(), scaled.error().GetHint()));
			return MakePlayStateResult(context);
		}

	}

	void RegisterPlayMethodTypes(TypeRegistry& registry)
	{
		const FieldMeta timeScaleMeta{ .Min = 0.0, .Max = PlaySession::MaxTimeScale };

		registry.Enum<PlayMode>("PlayMode", "How a play session runs (§5.6).")
			.Entry(PlayMode::Play, "Play", "The game as it ships: scripts and audio run.")
			.Entry(PlayMode::Simulate, "Simulate", "Play without scripts and audio, viewed through the editor camera.");

		registry.Enum<PlayRunState>("PlayRunState", "The play state, in the vocabulary of _meta.playState.")
			.Entry(PlayRunState::Edit, "Edit", "No play session (the editor edits its scene).")
			.Entry(PlayRunState::Play, "Play", "A running Play session.")
			.Entry(PlayRunState::Simulate, "Simulate", "A running Simulate session.")
			.Entry(PlayRunState::Paused, "Paused", "A paused session of either mode.");

		registry.Enum<PlayStepRender>("PlayStepRender", "Which ticks of a play.step extract the game view.")
			.Entry(PlayStepRender::Every, "Every", "Every tick, one tick per frame.")
			.Entry(PlayStepRender::Last, "Last", "Only the last tick; the ticks before it run as fast as the frame budget allows.")
			.Entry(PlayStepRender::None, "None", "No tick.");

		registry.Struct<PlayStartParams>("PlayStartParams", "The params of play.start (§13.5).")
			.Field("mode", &PlayStartParams::Mode, "Play (the default) or Simulate.")
			.Field("lockstep", &PlayStartParams::Lockstep, "Advance only through this client's play.step calls (§13.6).")
			.Field("seed", &PlayStartParams::Seed, "The session seed; absent: the project's Simulation.Seed xor the scene's Seed.")
			.Field("scene", &PlayStartParams::Scene, "A project scene to play, such as \"Assets/Scenes/Level2.scene\"; absent: the open scene.")
			.Field("parameters", &PlayStartParams::Parameters, "Load parameters for scripts (M13; refused when present).")
			.Field("paused", &PlayStartParams::Paused, "Start paused at tick 0.")
			.Field("timeScale", &PlayStartParams::TimeScale, "The time scale, 0 to 100 (1 is real time).", timeScaleMeta)
			.Field("pauseOnError", &PlayStartParams::PauseOnError, "Pause on a script error (M13; refused when present).");

		registry.Struct<PlayStateResult>("PlayStateResult", "A play session's state (play.state, and the result of play.start, pause, resume and setTimeScale).")
			.Field("state", &PlayStateResult::State, "Edit, Play, Simulate or Paused (as _meta.playState).")
			.Field("mode", &PlayStateResult::Mode, "Play or Simulate; tells them apart while Paused.")
			.Field("tick", &PlayStateResult::Tick, "The next tick: the ticks run so far.")
			.Field("stateHash", &PlayStateResult::StateHash, "The state hash (16 lowercase hex digits); empty in Edit.")
			.Field("lockstep", &PlayStateResult::Lockstep, "Ticks advance only through the owner's play.step.")
			.Field("lockstepOwner", &PlayStateResult::LockstepOwner, "The name of the client that owns lockstep; empty without lockstep.")
			.Field("ownedByCaller", &PlayStateResult::OwnedByCaller, "The requesting client owns lockstep.")
			.Field("timeScale", &PlayStateResult::TimeScale, "The time scale (1 is real time).")
			.Field("modified", &PlayStateResult::Modified, "An asset or script reloaded during play (§7.5).")
			.Field("recording", &PlayStateResult::Recording, "An input recording runs (M13; always false).")
			.Field("entityCount", &PlayStateResult::EntityCount, "The entities of the play scene.")
			.Field("maxEntities", &PlayStateResult::MaxEntities, "The project's entity cap of a session (Simulation.MaxEntities).")
			.Field("input", &PlayStateResult::Input, "The game input as the last tick's step view saw it.");

		registry.Struct<PlayStopResult>("PlayStopResult", "Where a stopped play session ended.")
			.Field("tick", &PlayStopResult::Tick, "The ticks the session ran.")
			.Field("stateHash", &PlayStopResult::StateHash, "Its final state hash (16 lowercase hex digits).");

		registry.Struct<PlayStepParams>("PlayStepParams", "The params of play.step (§13.6).")
			.Field("ticks", &PlayStepParams::Ticks, "The ticks to run (required), each a fixed step and a frame phase.",
				{ .Min = 1.0, .Max = static_cast<double>(MaxPlayStepTicks) })
			.Field("input", &PlayStepParams::Input, "Input events, each with a tick offset from the first tick this call runs (below ticks).")
			.Field("render", &PlayStepParams::Render, "Which ticks extract the game view: every, last (the default) or none.");

		registry.Struct<PlayStepResult>("PlayStepResult", "Where a play.step left the session.")
			.Field("tick", &PlayStepResult::Tick, "The session's tick after the call.")
			.Field("stateHash", &PlayStepResult::StateHash, "The state hash after the call (16 lowercase hex digits).")
			.Field("ticks", &PlayStepResult::Ticks, "The ticks this call ran.")
			.Field("frames", &PlayStepResult::Frames, "The host frames it spanned (ticks per frame fit a 50 ms budget).")
			.Field("rendered", &PlayStepResult::Rendered, "The game-view extractions during the call.");

		registry.Struct<PlaySetTimeScaleParams>("PlaySetTimeScaleParams", "The params of play.setTimeScale.")
			.Field("scale", &PlaySetTimeScaleParams::Scale, "The time scale, 0 to 100 (required; 1 is real time).", timeScaleMeta);
	}

	void RegisterPlayMethods(MethodRegistry& methods, bool includeEditorMethods)
	{
		Json startExample = Json::object();
		startExample["lockstep"] = true;
		startExample["seed"] = 42;
		Json stepExample = Json::object();
		stepExample["ticks"] = 600;
		stepExample["render"] = "last";
		Json stepInputExample = Json::object();
		Json tap = Json::object();
		tap["tick"] = 0;
		tap["type"] = "action";
		tap["name"] = "Jump";
		tap["state"] = "tap";
		stepInputExample["ticks"] = 60;
		stepInputExample["input"] = Json::array({ tap });
		Json scaleExample = Json::object();
		scaleExample["scale"] = 0.5;

		if (includeEditorMethods)
		{
			methods.Add(
				{
					.Name = "play.start",
					.Description = "Starts a play session of the open scene (or of scene) through the serializer copy; with lockstep, ticks advance "
								   "only through this client's play.step. Returns the session's state.",
					.ExposeAsTool = true,
					.Examples = { { .Description = "Play in lockstep with seed 42.", .Params = startExample } },
				},
				&Automation::PlayStart);

			methods.Add(
				{
					.Name = "play.stop",
					.Description = "Stops the play session; the edit scene was never touched. Returns the final tick and state hash.",
					.ExposeAsTool = true,
					.Examples = { { .Description = "Stop playing.", .Params = Json::object() } },
				},
				&Automation::PlayStop);
		}

		methods.Add(
			{
				.Name = "play.pause",
				.Description = "Pauses the play session; the lockstep owner's pause also leaves lockstep. Returns the session's state.",
				.AvailableInRuntime = true,
				.Examples = { { .Description = "Pause.", .Params = Json::object() } },
			},
			&Automation::PlayPause);

		methods.Add(
			{
				.Name = "play.resume",
				.Description = "Resumes a paused session that is not in lockstep. Returns the session's state.",
				.AvailableInRuntime = true,
				.Examples = { { .Description = "Resume.", .Params = Json::object() } },
			},
			&Automation::PlayResume);

		methods.AddPending<AutomationMethodContext, PlayStepParams, PlayStepResult>(
			{
				.Name = "play.step",
				.Description = "Runs ticks (fixed step + frame phase each) of a lockstep or paused session, as many per frame as fit 50 ms, with "
							   "optional tick-stamped input. Returns the tick, the state hash, and the frames and renders it took.",
				.RequiredParams = { "ticks" },
				.ExposeAsTool = true,
				.AvailableInRuntime = true,
				.TimeoutSeconds = 600,
				.Examples = { { .Description = "Fast-forward 10 s at 60 Hz and render the last tick.", .Params = stepExample },
					{ .Description = "Tap Jump on the first of 60 ticks.", .Params = stepInputExample } },
			},
			&Automation::PlayStep);

		methods.Add(
			{
				.Name = "play.state",
				.Description = "The play state (Edit without a session), with the tick, the state hash, lockstep and the input the last tick saw.",
				.AvailableInRuntime = true,
				.Examples = { { .Description = "Read the play state.", .Params = Json::object() } },
			},
			&Automation::PlayState);

		methods.Add(
			{
				.Name = "play.setTimeScale",
				.Description = "Sets the time scale of the play session (0 to 100, 1 is real time). Returns the session's state.",
				.RequiredParams = { "scale" },
				.AvailableInRuntime = true,
				.Examples = { { .Description = "Half speed.", .Params = scaleExample } },
			},
			&Automation::PlaySetTimeScale);
	}

}
