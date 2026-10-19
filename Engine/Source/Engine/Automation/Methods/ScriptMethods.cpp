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
			const ScriptErrorStream* stream = context.GetScriptErrors();
			const uint64_t before = stream == nullptr ? 0 : stream->GetCursor();
			Result<ScriptEvaluation> result = MakeError(ErrorCode::InvalidArgument, "unknown script evaluation context");
			if (params.Context == ScriptEvaluationContext::Edit)
				result = context.EvalInEdit(params.Code, params.Entity);
			else if (params.Context == ScriptEvaluationContext::Play)
			{
				ENGINE_TRY_ASSIGN(PlaySession * session, Utils::RequirePlaySession(context));
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
				}
				result = scripts->Evaluate(params.Code, VfsPath{}, entity);
			}
			if (!result && (result.error().GetCode() == ErrorCode::Script || result.error().GetCode() == ErrorCode::CompileFailed || result.error().GetCode() == ErrorCode::Timeout))
			{
				stream = context.GetScriptErrors();
				const std::vector<ScriptError> errors = stream == nullptr ? std::vector<ScriptError>{} : stream->Read(before, 1000);
				if (!errors.empty())
					context.SetErrorData("scriptError", ScriptErrorToJson(errors.back()));
				else
				{
					const Error& failure = result.error();
					ScriptError error;
					error.Kind = failure.GetCode() == ErrorCode::CompileFailed ? ScriptErrorKind::Compile
						: failure.GetCode() == ErrorCode::Timeout              ? ScriptErrorKind::Timeout
																			   : ScriptErrorKind::Runtime;
					error.Script = failure.GetLocation().File;
					error.Line = failure.GetLocation().Line;
					error.Column = failure.GetLocation().Column;
					error.JsonPointer = failure.GetLocation().JsonPointer.value_or("");
					error.Message = failure.GetMessageText();
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
			.Field("tick", &ScriptErrorSummary::Tick, "Simulation tick of the latest occurrence.")
			.Field("count", &ScriptErrorSummary::Count, "Number of occurrences.")
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
			.Field("entity", &ScriptEvalParams::Entity, "Optional behaviour entity bound as self.");
		registry.Struct<ScriptEvalResult>("ScriptEvalResult", "Detached evaluation result.")
			.Field("value", &ScriptEvalResult::Value, "First returned finite, acyclic JSON value.")
			.Field("prints", &ScriptEvalResult::Prints, "Printed messages in call order.");
	}

	void RegisterSharedScriptMethods(MethodRegistry& methods)
	{
		methods.Add({ .Name = "script.errors", .Description = "Reads retained script errors without clearing them, including updated repeat counts.", .ExposeAsTool = true, .AvailableInRuntime = true, .AllowedInBatch = true, .Examples = { { .Description = "Read errors from the beginning.", .Params = Json::object() } } }, &Automation::ScriptErrors);
		methods.Add({ .Name = "script.eval", .Description = "Evaluates Luau in Edit (read-only, editor only) or the live Play VM; Play obeys lockstep ownership.", .RequiredParams = { "code", "context" }, .ExposeAsTool = true, .AvailableInRuntime = true, .Examples = { { .Description = "Evaluate an expression during play.", .Params = Json{ { "code", "1 + 2" }, { "context", "Play" } } } } }, &Automation::ScriptEval);
	}

}
