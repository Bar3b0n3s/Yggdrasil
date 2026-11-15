#include "EnginePCH.h"
#include "Engine/Automation/Methods/ScriptMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/Private/PlayMethodSupport.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>

namespace Engine {

	namespace Automation {

		Result<ScriptErrorsResult> ScriptErrors(AutomationMethodContext& context, const ScriptErrorsParams& params)
		{
			uint64_t since = 0;
			if (!params.Since.empty() && params.Since != "end")
			{
				const auto parsed = std::from_chars(params.Since.data(), params.Since.data() + params.Since.size(), since);
				if (parsed.ec != std::errc{} || parsed.ptr != params.Since.data() + params.Since.size())
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/since", "expected an unsigned decimal cursor or end", "reuse nextCursor"));
			}
			if (params.Limit < 1 || params.Limit > 1000)
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/limit", "limit must be from 1 to 1000", {}));
			const ScriptErrorStream* stream = context.GetScriptErrors();
			if (stream == nullptr)
				return MakeError(ErrorCode::Unsupported, "the host has no script error service");
			ScriptErrorsResult result;
			if (params.Since == "end")
			{
				result.NextCursor = std::to_string(stream->GetCursor());
				return result;
			}
			result.Evicted = stream->HasEvictedAfter(since);
			// The stream outlives the sessions and scenes its errors came from, so each entity's path is read now, in the scene
			// reads address by default (§13.4: the play scene while playing, else the edit scene), and stays empty when that
			// scene no longer holds the entity (destroyed, spawned by an ended session) or there is none.
			const Result<Scene*> scene = context.ResolveTargetScene(SceneTarget::Play, false, false);
			for (const ScriptError& error : stream->Read(since, params.Limit))
			{
				ScriptErrorSummary entry;
				entry.Id = ToAutomationCounter(error.ID);
				entry.Kind = ScriptErrorKindToString(error.Kind);
				entry.Script = error.Script;
				entry.Line = error.Line;
				entry.Column = error.Column;
				entry.Message = error.Message;
				entry.Callback = error.Callback;
				entry.Entity.Id = Utils::FormatOptionalUUID(error.Entity);
				entry.Entity.Name = error.EntityName;
				entry.Slot = error.Slot;
				if (scene && error.Entity.IsValid())
				{
					if (const ConstEntity owner = (*scene)->FindEntityByID(error.Entity); owner.IsValid())
						entry.Entity.Path = (*scene)->GetEntityPath(owner);
				}
				entry.Tick = ToAutomationCounter(error.Tick);
				entry.Count = ToAutomationCounter(error.Count);
				entry.JsonPointer = error.JsonPointer;
				for (const ScriptTraceFrame& frame : error.Traceback)
					entry.Traceback.push_back({ .Script = frame.Script, .Line = frame.Line, .Function = frame.Function });
				result.Errors.push_back(std::move(entry));
				since = error.ID;
			}
			result.NextCursor = std::to_string(since);
			return result;
		}

		Result<ScriptEvalResult> ScriptEval(AutomationMethodContext& context, const ScriptEvalParams& params)
		{
			if (context.HasParam("entity") && params.Entity.empty())
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/entity", "entity must name a behaviour instance when present", {}));
			if (context.HasParam("script") && (params.Script.empty() || !context.HasParam("entity")))
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/script", "script must be a behaviour's Script.Define name, given with entity",
					"name the entity whose script slot self binds to"));
			const ScriptErrorStream* stream = context.GetScriptErrors();
			const uint64_t before = stream == nullptr ? 0 : stream->GetCursor();
			// The failure's script error data: the caller's own error, which no error stream retains (ADR 0038, friction F01).
			std::optional<ScriptError> failure;
			// A behaviour name that no slot of the entity has is located at /script: the engine refuses it, unlocated, before
			// running anything (an unresolved entity is already located at /entity).
			const auto locateScript = [&params](Result<ScriptEvaluation> evaluated) -> Result<ScriptEvaluation>
			{
				if (evaluated || params.Script.empty() || evaluated.error().GetCode() != ErrorCode::NotFound || evaluated.error().GetLocation().JsonPointer.has_value())
					return evaluated;
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/script", evaluated.error().GetMessageText(), evaluated.error().GetHint()));
			};
			Result<ScriptEvaluation> result = MakeError(ErrorCode::InvalidArgument, "unknown script evaluation context");
			if (params.Context == ScriptEvaluationContext::Edit)
				result = locateScript(context.EvalInEdit(params.Code, params.Entity, params.Script, failure));
			else if (params.Context == ScriptEvaluationContext::Play)
			{
				ENGINE_TRY_ASSIGN(PlaySession * session, Utils::RequirePlaySession(context));
				// A scene target reaches the play VM only when it names the scene the session plays: with another open scene
				// there is no play VM, as "target": "play" is refused there (Docs/Decisions/0031-m20-contract.md decision 16).
				if (context.GetOptions().Scene.has_value())
				{
					if (const Result<Scene*> played = context.ResolveTargetScene(SceneTarget::Play, true, false); !played)
					{
						if (played.error().GetCode() != ErrorCode::InvalidArgument)
							return std::unexpected(played.error());
						return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/scene", played.error().GetMessageText(),
							"name the played scene, or omit scene to reach the play VM from any tab, or use context Edit to read this scene"));
					}
				}
				ENGINE_TRY(Utils::CheckLockstepOwner(context, *session));
				ScriptEngine* scripts = session->GetScripts();
				if (scripts == nullptr || session->GetMode() != PlayMode::Play || scripts->IsStopped())
					return MakeError(ErrorCode::InvalidState, "play evaluation requires a live Play VM");
				std::optional<UUID> entity;
				if (context.HasParam("entity"))
				{
					ENGINE_TRY_ASSIGN(Entity resolved, context.ResolveEntity(session->GetScene(), params.Entity, "/entity"));
					entity = resolved.GetUUID();
					const std::vector<ScriptInstanceInfo> instances = scripts->GetInstances();
					if (std::ranges::none_of(instances, [entity](const ScriptInstanceInfo& instance)
					{
						return instance.Entity == *entity;
					}))
						return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/entity", "entity has no behaviour instance", {}));
					// Without script, self is the first slot's instance (decision 18).
					if (params.Script.empty() && std::ranges::none_of(instances, [entity](const ScriptInstanceInfo& instance)
					{
						return instance.Entity == *entity && instance.Slot == 0;
					}))
						return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/entity", "entity has no behaviour instance in its first script slot",
							"name the behaviour self binds to with script"));
				}
				result = locateScript(scripts->Evaluate(params.Code, VfsPath{}, entity, params.Script, &failure));
			}
			if (!result && (result.error().GetCode() == ErrorCode::Script || result.error().GetCode() == ErrorCode::CompileFailed || result.error().GetCode() == ErrorCode::Timeout))
			{
				// The evaluation's own error data when the VM raised it; one that stopped the VM was published as the game's
				// error (ScriptEngine::Evaluate), so the stream holds it; otherwise the returned error's location.
				stream = context.GetScriptErrors();
				const std::vector<ScriptError> published = stream == nullptr || failure.has_value() ? std::vector<ScriptError>{} : stream->Read(before, 1000);
				if (failure.has_value())
					context.SetErrorData("scriptError", ScriptErrorToJson(*failure));
				else if (!published.empty())
					context.SetErrorData("scriptError", ScriptErrorToJson(published.back()));
				else
				{
					const Error& raised = result.error();
					ScriptError error;
					error.Kind = raised.GetCode() == ErrorCode::CompileFailed ? ScriptErrorKind::Compile
						: raised.GetCode() == ErrorCode::Timeout              ? ScriptErrorKind::Timeout
																			  : ScriptErrorKind::Runtime;
					error.Script = raised.GetLocation().File;
					error.Line = raised.GetLocation().Line;
					error.Column = raised.GetLocation().Column;
					error.JsonPointer = raised.GetLocation().JsonPointer.value_or("");
					error.Message = raised.GetMessageText();
					BoundScriptError(error);
					context.SetErrorData("scriptError", ScriptErrorToJson(error));
				}
				return std::unexpected(Error(ErrorCode::Script, result.error().GetMessageText()).WithLocation(result.error().GetLocation()));
			}
			return result;
		}

	}

	void RegisterSharedScriptMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<ScriptEvaluationContext>("ScriptEvaluationContext", "The scene and VM used by script.eval.")
			.Entry(ScriptEvaluationContext::Edit, "Edit", "Fresh read-only editor VM without lifecycle callbacks.")
			.Entry(ScriptEvaluationContext::Play, "Play", "The live Play VM, subject to lockstep ownership.");
		registry.Struct<ScriptTraceFrameSummary>("ScriptTraceFrameSummary", "An authored script traceback frame.")
			.Field("script", &ScriptTraceFrameSummary::Script, "Authored source path.")
			.Field("line", &ScriptTraceFrameSummary::Line, "One-based source line, zero when unknown.")
			.Field("function", &ScriptTraceFrameSummary::Function, "Function name when available.");
		registry.Struct<ScriptErrorSummary>("ScriptErrorSummary", "One retained script error with its most recent occurrence.")
			.Field("id", &ScriptErrorSummary::Id, "Saturated display ID; use nextCursor for paging.")
			.Field("kind", &ScriptErrorSummary::Kind, "compile, type, runtime, timeout or memory.")
			.Field("script", &ScriptErrorSummary::Script, "Authored source path.")
			.Field("line", &ScriptErrorSummary::Line, "One-based authored line, zero when unknown.")
			.Field("column", &ScriptErrorSummary::Column, "One-based UTF-8 byte column, zero when unknown.")
			.Field("message", &ScriptErrorSummary::Message, std::format("Error message of at most {} bytes; longer text is cut and ends with [truncated: <n> bytes].", MaxScriptLogMessageBytes))
			.Field("callback", &ScriptErrorSummary::Callback, "Callback that failed.")
			.Field("entity", &ScriptErrorSummary::Entity, "The entity whose script raised the error: its id and name at the latest occurrence, and its path read now in the scene reads address by default (the play scene while playing, else the edit scene), empty when that scene no longer holds the entity (destroyed, or spawned by a play session that ended) and for an error raised outside an entity.")
			.Field("slot", &ScriptErrorSummary::Slot, "The faulting behaviour's slot in its entity's Script list; 0, naming no behaviour, for a native fault (callback NativeStep, script native:<System>).")
			.Field("tick", &ScriptErrorSummary::Tick, "The simulation tick the latest occurrence's callback ran in, as Time.GetTick() reads it there: in the fixed step the zero-based index of the tick being run (the ticks completed before it, so 0 in the first OnFixedUpdate), in the frame phase (OnUpdate, OnLateUpdate) the ticks completed.")
			.Field("count", &ScriptErrorSummary::Count, "Occurrences of this error (same script, line, message and jsonPointer) among the errors script.errors reads: a faulting behaviour instance is disabled at its first error, so each instance adds one until a hot reload of its script re-enables it.")
			.Field("traceback", &ScriptErrorSummary::Traceback, std::format("Authored traceback frames, innermost first: at most {}, the last a [truncated: <n> more frames] function without a script when deeper frames were left out.", MaxScriptErrorTracebackFrames))
			.Field("jsonPointer", &ScriptErrorSummary::JsonPointer, "Embedded source location, empty for standalone scripts.");
		registry.Struct<ScriptErrorsParams>("ScriptErrorsParams", "Read errors after an exclusive cursor.")
			.Field("since", &ScriptErrorsParams::Since, "Empty for the beginning, end for the current cursor, or a previous nextCursor.")
			.Field("limit", &ScriptErrorsParams::Limit, "Maximum returned entries.", { .Min = 1.0, .Max = 1000.0 });
		registry.Struct<ScriptErrorsResult>("ScriptErrorsResult", "Errors and a full precision decimal cursor.")
			.Field("errors", &ScriptErrorsResult::Errors, "Updated errors in cursor order.")
			.Field("nextCursor", &ScriptErrorsResult::NextCursor, "Exclusive sequence to reuse as since, also for an empty page.")
			.Field("evicted", &ScriptErrorsResult::Evicted, std::format("True when an error newer than since was evicted before this read: the stream keeps the {} distinct errors with the latest occurrences, and the retained ones still follow.", MaxScriptErrorEntries));
		registry.Struct<ScriptEvalParams>("ScriptEvalParams", "Evaluate Luau in an explicitly selected context.")
			.Field("code", &ScriptEvalParams::Code, "UTF-8 Luau expression or chunk.")
			.Field("context", &ScriptEvalParams::Context, "Edit or Play.")
			.Field("entity", &ScriptEvalParams::Entity, "Optional behaviour entity bound as self.")
			.Field("script", &ScriptEvalParams::Script, "The behaviour self binds to when entity has several scripts: its Script.Define name; absent, the entity's first script.");
		registry.Struct<ScriptEvalResult>("ScriptEvalResult", "Detached evaluation result.")
			.Field("value", &ScriptEvalResult::Value, "First returned finite, acyclic JSON value; a vector, Color or Quat at any depth as the array of its components ([x, y, z], [r, g, b, a], [x, y, z, w]).")
			.Field("prints", &ScriptEvalResult::Prints, "Printed messages in call order.");
	}

	void RegisterSharedScriptMethods(MethodRegistry& methods)
	{
		methods.Add({ .Name = "script.errors", .Description = "Reads the game's retained script errors without clearing them, including updated repeat counts, never a failed script.eval's: in the editor the current play session's (a new play session forgets the earlier sessions' errors and keeps the cursor), in the Runtime its process's. An error's tick is the tick its callback ran in, as Time.GetTick() reads it there (0 in the first fixed step; in the frame phase the ticks completed). Whatever pauseOnError says, the faulting behaviour instance is disabled at its first error and runs no further callback (its tasks cancelled) until a hot reload of its script, so its error's count does not grow; other instances and the entity's other slots run on, and pauseOnError only decides whether the editor also pauses play.", .ExposeAsTool = true, .SupportsSceneTarget = true, .AvailableInRuntime = true, .AllowedInBatch = true, .Examples = { { .Description = "Read errors from the beginning.", .Params = Json::object() } } }, &Automation::ScriptErrors);
		methods.Add({ .Name = "script.eval", .Description = "Evaluates Luau in Edit (read-only, editor only) or the live Play VM; Play obeys lockstep ownership. A failure is the caller's error, answered with its located scriptError and logged, never one of the game's script errors nor counted in the response's _meta diagnostics.", .RequiredParams = { "code", "context" }, .ExposeAsTool = true, .SupportsSceneTarget = true, .AvailableInRuntime = true, .Examples = { { .Description = "Evaluate an expression during play.", .Params = Json{ { "code", "1 + 2" }, { "context", "Play" } } } } }, &Automation::ScriptEval);
	}

}
