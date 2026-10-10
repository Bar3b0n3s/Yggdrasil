#pragma once

#include "Engine/Asset/ReplayData.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Automation/Protocol/MethodContext.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Scene/RenderExtraction.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The request context of the method handlers the Editor and the Runtime share (Architecture §3 "Automation/Methods",
// §13.5 "Runtime subset"; Docs/Decisions/0008-m4-decisions.md decisions 7 and 26, Docs/Decisions/0012-m7-decisions.md
// decision 12). It is the intermediate base of MethodContext.h's file comment: EditorCore's EditorMethodContext and the
// Runtime's context (RuntimeAutomationServer.h) derive from it, it answers IsHostType for its own key, and a handler typed
// on it is registered by both hosts. Its virtual functions are the host-neutral services those handlers need; each host
// implements them over its own state (the editor over EditorContext and its AutomationServer, the Runtime over its play
// session and server).
//
// Frozen by the M7 contract with the services the M7 handlers need (play.*, input.inject, viewport.screenshot). Stream C,
// which moved the M4 to M6 handlers of the Runtime subset here (session.*, rpc.discover, scene.tree|query|get,
// entity.get, entity.bounds, log.read, events.read), added the pure virtual functions those handlers need at the end, as
// reviewed additions (ADR 0012 decisions 12 and 21); the integration's review gave DescribeSession a return value and
// added GetWallClockTime. The M12 contract added GetAudioEngine (audio.stats, Docs/Decisions/0015-m12-decisions.md). One
// context per request; main thread only.

namespace Engine {

	struct ProjectSettings;
	struct StatsGetResult;

	class AssetManager;
	class AudioEngine;
	class ConstEntity;
	class Entity;
	class EventLog;
	class PlaySession;
	class Scene;
	class ScriptErrorStream;
	struct ScriptEvaluation;
	struct PlayStartOptions;
	struct RenderSnapshot;
	struct SceneSummary;
	struct SessionHostDescription;
	struct SessionShutdownParams;
	struct SessionShutdownResult;
	struct ViewportScreenshotRequest;

	class AutomationMethodContext : public MethodContext
	{
	public:
		~AutomationMethodContext() override;

		// TypeKeyOf<AutomationMethodContext>(), then MethodContext::IsHostType.
		[[nodiscard]] bool IsHostType(TypeKey key) const override;

		// --- Scenes and entities (§13.4) --------------------------------------------------------------------------------

		// The scene a request addresses (§13.4 "Target"): `target` as given when `given`, else the play scene for reads while
		// playing and the edit scene otherwise (`mutation` true: the edit scene). The pointer is non-owning and valid until
		// the open scene or the play session changes. Errors: InvalidState "no scene open" (editor without an open scene),
		// InvalidState "not playing" for "play" without a play session (located at /target), InvalidState for "edit" in the
		// Runtime, which has no edit scene (located at /target).
		[[nodiscard]] virtual Result<Scene*> ResolveTargetScene(SceneTarget target, bool given, bool mutation) const = 0;

		// Resolves an EntityRef (§13.4) in `scene`: 16 hex digits (either case) is an exact id; 6 to 15 hex digits a unique
		// prefix of an entity's id; text starting with '/' an entity path (Scene::ResolveEntityPath). `pointer` locates the
		// param in errors ("/entity", "/camera"). Errors: InvalidArgument for anything else, or for an ambiguous prefix or
		// path (every candidate listed as an ErrorIssue with its path); NotFound for a valid reference that names no entity.
		[[nodiscard]] virtual Result<Entity> ResolveEntity(Scene& scene, std::string_view reference, std::string_view pointer) const = 0;

		// {id, name, path} of `entity` (valid, asserted).
		[[nodiscard]] virtual EntitySummary MakeEntitySummary(ConstEntity entity) const = 0;

		// --- Play (§5.6, §13.5 play.*, input.*) -------------------------------------------------------------------------

		// The host's play session: the editor's while it plays or simulates (null in Edit mode), the Runtime's always. The
		// pointer is non-owning and valid until play.stop.
		[[nodiscard]] virtual PlaySession* GetPlaySession() const = 0;

		// play.start (editor only, Automation/Methods/PlayMethods.h): starts a session of `options.ScenePath` (empty: the open
		// edit scene, through the serializer copy of §5.6) with the project's settings and `options`; the requesting client
		// (GetRequest().Client) owns lockstep when options.Lockstep. Appends PlayStateChanged. Errors: InvalidState while a
		// session runs, without an open scene, or while the project has asset diagnostics of severity Error ("fix them first";
		// §7.2); the errors of PlaySession::Create; NotFound or Validation for options.ScenePath; Unsupported in the Runtime,
		// whose session starts with the process.
		[[nodiscard]] virtual Status StartPlay(const PlayStartOptions& options) = 0;

		// play.stop (editor only): destroys the session (§5.6 Stop: the edit scene was never touched) and appends
		// PlayStateChanged; the deferred asset reloads of a lockstep session are applied (§7.5 race rule 4). Errors:
		// InvalidState "not playing"; Unsupported in the Runtime.
		[[nodiscard]] virtual Status StopPlay() = 0;

		// M13 recording/replay host services, shared with Runtime automation. These are transient session operations,
		// never a project-setting edit. Implementations prepare/validate the new start before replacing the old session,
		// preserve the requesting client's identity, and use a fresh session serial. Defaults are Unsupported until wired.
		[[nodiscard]] virtual Status StartRecordingSession(const PlayStartOptions& options, bool restart);
		[[nodiscard]] virtual Status RestartForReplay(const ReplayHeader& header);
		// Current session's original scene asset identity/path, initial load parameters, seed and FixedHz, plus engine
		// version/build config. An unsaved editor scene cannot produce a reproducible asset header (InvalidState).
		[[nodiscard]] virtual Result<ReplayHeader> DescribeReplayHeader() const;
		// Confined paths, never arbitrary native I/O. The editor imports/loads the project replay; Runtime reads its
		// cooked replay assets or its user://Replays/ recordings. Editor writes require project write permission;
		// Runtime writes confine relative paths below user://Replays/ and return that identity (ADR 0019). Absolute
		// paths, traversal and other schemes are rejected. Write validates before atomic replacement, with provenance.
		[[nodiscard]] virtual Result<AssetRef<ReplayData>> LoadReplay(std::string_view path);
		// Non-writing preflight for permission/confinement and canonical path, before consuming a recorder. WriteReplay
		// repeats validation when it writes, so permission or filesystem changes cannot bypass publication checks.
		[[nodiscard]] virtual Result<std::string> ValidateReplayOutput(std::string_view path) const;
		[[nodiscard]] virtual Result<std::string> WriteReplay(std::string_view path, const ReplayDocument& document);

		// Host-owned diagnostics outlive individual play/scene/edit-eval VMs. Cursor IDs remain monotonic across their
		// replacement. The pointer is borrowed for this request only; null means no installed script-error service.
		[[nodiscard]] virtual ScriptErrorStream* GetScriptErrors() const;
		// Editor-only, fresh read-only VM over the edit scene, with fields/class links but no lifecycle callbacks.
		// Entity is an optional EntityRef, resolved at /entity. Runtime returns Unsupported at /context before trying
		// to resolve a nonexistent edit scene. Returns owned value/prints; errors retain full script error data.
		[[nodiscard]] virtual Result<ScriptEvaluation> EvalInEdit(std::string_view code, std::string_view entity);

		// The display name of `client` (session.hello's client.name, or "batch", "cli", "test" for in-process clients); empty
		// for NoClient or a client that is gone. play.state reports the lockstep owner by it.
		[[nodiscard]] virtual std::string GetClientName(ClientId client) const = 0;

		// The context's event log (§4.9): PlayStateChanged is appended there.
		[[nodiscard]] virtual EventLog& GetEventLog() const = 0;

		// --- Screenshots (§8.13, §13.5 viewport.screenshot) ---------------------------------------------------------------

		// Renders `snapshot` (extracted for request.Width x request.Height) through the host's ViewportCapture
		// (ViewportCapture::Capture) and reads it back at full size; the handler downscales. Errors: Unsupported "start the
		// <host> without --renderer none" without a device; those of ViewportCapture::Capture.
		[[nodiscard]] virtual Result<Image> CaptureView(const RenderSnapshot& snapshot, const ViewportScreenshotRequest& request) = 0;

		// The camera of the scene view (viewport.screenshot {view: "scene"} without "camera"): the editor's (the M7 default
		// ExplicitRenderCamera until the editor camera of M10); nullopt in the Runtime, which has no scene view (§13.5: the
		// Runtime serves the game view only).
		[[nodiscard]] virtual std::optional<ExplicitRenderCamera> GetSceneViewCamera() const = 0;

		// Writes a method's output file, such as a screenshot's PNG, into the host's output directory and returns its absolute
		// native path (the editor's AutomationServer::WriteOutputFile; the Runtime's server writes under
		// user://Automation/Out/ with the same naming). `extension` is lowercase letters and digits without the dot. Errors:
		// those of the writes.
		[[nodiscard]] virtual Result<std::string> WriteOutputFile(std::string_view extension, std::span<const std::byte> bytes) = 0;

		// --- The moved M4 to M6 domains (ADR 0012 decisions 12 and 21: added with their handlers) -----------------------

		// The host's part of session.hello and session.info (SessionMethods.h): its capabilities, project, renderer, read-only
		// and headless flags and clients. The handlers add the versions, the process id, the play state and the lockstep
		// owner, and session.info sorts the clients by id.
		[[nodiscard]] virtual SessionHostDescription DescribeSession() const = 0;

		// session.shutdown's effect: the editor first saves the open scene when params.Save asks (refusing a dirty scene
		// without save or force, SessionMethods.h); the Runtime has no scene file to save. The host then exits with code 0 at
		// its next frame, after the response. Errors: those of the editor's save and its dirty-scene check.
		[[nodiscard]] virtual Result<SessionShutdownResult> Shutdown(const SessionShutdownParams& params) = 0;

		// scene.tree's summary of `scene`, a scene ResolveTargetScene returned: its file, name, revision (the "_meta"
		// revision), dirty flag and entity count.
		[[nodiscard]] virtual SceneSummary MakeSceneSummary(const Scene& scene) const = 0;

		// The host's asset manager (entity.bounds reads mesh bounds through it): the editor's EditorAssetManager, the
		// Runtime's RuntimeAssetManager (RuntimeAutomationServerSpecification::Assets); null for a host without one, where
		// entity.bounds is Unsupported. The pointer is non-owning and valid for the request.
		[[nodiscard]] virtual AssetManager* GetAssets() const = 0;

		// The host's wall clock: std::chrono::steady_clock::now unless the host's specification supplies another
		// (AutomationServerSpecification::WallClock, RuntimeAutomationServerSpecification::WallClock), which tests script.
		// play.step measures its frame budget with it (PlayStepFrameBudget, PlayMethods.h); nothing on the simulation path
		// reads it.
		[[nodiscard]] virtual std::chrono::steady_clock::time_point GetWallClockTime() const = 0;

		// --- Audio (M12) ----------------------------------------------------------------------------------------------------

		// The host's audio engine (audio.stats, Automation/Methods/AudioMethods.h): the editor's and the Runtime's
		// EngineContext::GetAudioEngine (RuntimeAutomationServerSpecification::Audio); null for a host without one, where
		// audio.stats is Unsupported. The pointer is non-owning and valid for the request.
		[[nodiscard]] virtual AudioEngine* GetAudioEngine() const = 0;

		// Project settings observed by render extraction, scene raycast layer filtering and screenshots. Borrowed for the
		// current request only; null without an open project. Editor: open project. Runtime: PlaySession project copy.
		[[nodiscard]] virtual const ProjectSettings* GetProjectSettings() const;
		// Copies CPU/session counts plus independent scene/game RenderStats histories. Never initiates a GPU wait/render.
		// Unsupported until host service is injected; renderer none must still return CPU/session counts when integrated.
		[[nodiscard]] virtual Result<StatsGetResult> GetHostStatistics() const;
		// Copy of the editor selection for capture-only "selected" annotations; Runtime returns an empty vector. The
		// snapshot filters ids against its target scene, so selection cannot retain an entity/component reference.
		[[nodiscard]] virtual std::vector<UUID> GetSelectedEntities() const;
	protected:
		// `hostKey` is the most-derived context's TypeKeyOf, as for MethodContext.
		AutomationMethodContext(TypeKey hostKey, MethodRequest request);
	};

}
