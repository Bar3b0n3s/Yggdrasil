#include "EnginePCH.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"

#include "Engine/Automation/Methods/PlayMethods.h"
#include "Engine/Core/Assert.h"
#include "Engine/Reflection/ComponentInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Session/PlaySession.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <utility>

namespace Engine {

	namespace Utils {

		// `source` with a new code, message and issues; its contexts, hint and location are kept.
		static Error RebuildError(const Error& source, ErrorCode code, std::string message, std::vector<ErrorIssue> issues)
		{
			Error rebuilt(code, std::move(message));
			for (const std::string& context : source.GetContexts())
			{
				Error next = std::move(rebuilt).WithContext(context);
				rebuilt = std::move(next);
			}
			if (!source.GetHint().empty())
			{
				Error next = std::move(rebuilt).WithHint(source.GetHint());
				rebuilt = std::move(next);
			}
			Error located = std::move(rebuilt).WithLocation(source.GetLocation());
			return std::move(located).WithIssues(std::move(issues));
		}

		static bool IsHexDigit(char character)
		{
			return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
		}

		static bool IsHexText(std::string_view text)
		{
			return !text.empty() && std::all_of(text.begin(), text.end(), &IsHexDigit);
		}

		Error ReplaceIssues(const Error& error, std::vector<ErrorIssue> issues)
		{
			return RebuildError(error, error.GetCode(), error.GetMessageText(), std::move(issues));
		}

		Error PrefixPointers(const Error& error, std::string_view prefix)
		{
			std::vector<ErrorIssue> issues = error.GetIssues();
			for (ErrorIssue& issue : issues)
				issue.JsonPointer = std::string(prefix) + issue.JsonPointer;

			ErrorLocation location;
			location.JsonPointer = std::string(prefix) + error.GetLocation().JsonPointer.value_or(std::string());
			return ReplaceIssues(Error(error).WithLocation(std::move(location)), std::move(issues));
		}

		Error LocateAtParam(const Error& error, std::string_view pointer)
		{
			std::vector<ErrorIssue> issues = error.GetIssues();
			for (ErrorIssue& issue : issues)
			{
				if (issue.JsonPointer.empty())
					issue.JsonPointer = std::string(pointer);
			}
			ErrorLocation location;
			location.JsonPointer = std::string(pointer);
			return ReplaceIssues(Error(error).WithLocation(std::move(location)), std::move(issues));
		}

		Error MakeParamError(ErrorCode code, std::string_view pointer, std::string message, std::string hint)
		{
			ErrorLocation location;
			location.JsonPointer = std::string(pointer);
			ErrorIssue issue;
			issue.JsonPointer = std::string(pointer);
			issue.Message = message;
			issue.Hint = hint;
			return Error(code, std::move(message)).WithHint(std::move(hint)).WithLocation(std::move(location)).WithIssue(std::move(issue));
		}

		std::string FormatOptionalUUID(UUID id)
		{
			return id.IsValid() ? id.ToString() : std::string();
		}

		Result<Entity> ResolveEntityReference(Scene& scene, std::string_view reference, std::string_view pointer)
		{
			if (reference.empty())
			{
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, pointer, "an entity reference must not be empty",
					"give 16 hex digits, a unique id prefix of at least 6 hex digits, or a path such as \"/Game/Board\""));
			}

			if (reference.front() == '/')
			{
				Result<Entity> entity = scene.ResolveEntityPath(reference);
				if (!entity)
					return std::unexpected(LocateAtParam(entity.error(), pointer));
				return *entity;
			}

			if (IsHexText(reference) && reference.size() == UUID::TextLength)
			{
				const std::optional<UUID> id = UUID::FromString(reference);
				const Entity entity = id.has_value() ? scene.FindEntityByID(*id) : Entity();
				if (!entity.IsValid())
					return std::unexpected(MakeParamError(ErrorCode::NotFound, pointer, std::format("no entity has the id {}", reference)));
				return entity;
			}

			if (UUID::IsValidPrefix(reference))
			{
				std::vector<Entity> matches;
				scene.ForEachCanonical([&matches, reference](Entity entity)
				{
					if (entity.GetUUID().MatchesPrefix(reference))
						matches.push_back(entity);
				});
				if (matches.empty())
					return std::unexpected(MakeParamError(ErrorCode::NotFound, pointer, std::format("no entity id starts with '{}'", reference)));
				if (matches.size() > 1)
				{
					std::vector<ErrorIssue> candidates;
					for (const Entity match : matches)
					{
						ErrorIssue issue;
						issue.JsonPointer = std::string(pointer);
						issue.Message = std::format("candidate {} '{}'", match.GetUUID().ToString(), scene.GetEntityPath(match));
						issue.Suggestions = { match.GetUUID().ToString() };
						candidates.push_back(std::move(issue));
					}
					ErrorLocation location;
					location.JsonPointer = std::string(pointer);
					return std::unexpected(
						Error(ErrorCode::InvalidArgument, std::format("the id prefix '{}' is ambiguous: {} entities match", reference, matches.size()))
							.WithHint("give more hex digits, or the full 16-digit id")
							.WithLocation(std::move(location))
							.WithIssues(std::move(candidates)));
				}
				return matches.front();
			}

			return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("'{}' is not an entity reference", reference),
				"give 16 hex digits, a unique id prefix of at least 6 hex digits, or a path starting with '/' such as \"/Game/Board\""));
		}

		EntitySummary SummarizeEntity(ConstEntity entity)
		{
			ENGINE_ASSERT(entity.IsValid(), "SummarizeEntity needs a valid entity");
			EntitySummary summary;
			summary.Id = entity.GetUUID().ToString();
			summary.Name = entity.GetName();
			summary.Path = entity.GetScene()->GetEntityPath(entity);
			return summary;
		}

		std::vector<const ComponentInfo*> GetEntityComponents(ConstEntity entity)
		{
			std::vector<const ComponentInfo*> components;
			for (const ComponentInfo* info : entity.GetScene()->GetTypeRegistry().GetComponents())
			{
				if (info->HasFlag(ComponentFlags::Serializable) && !info->HasFlag(ComponentFlags::EntityLevel) && info->GetHostOps() != nullptr
					&& info->GetHostOps()->Has(entity))
				{
					components.push_back(info);
				}
			}
			return components;
		}

		Result<std::optional<uint64_t>> ParseSequenceCursor(std::string_view cursor)
		{
			if (cursor.empty())
				return std::optional<uint64_t>(0);
			if (cursor == "end")
				return std::optional<uint64_t>();

			uint64_t value = 0;
			const bool digitsOnly = std::all_of(cursor.begin(), cursor.end(), [](char character)
			{
				return character >= '0' && character <= '9';
			});
			const std::from_chars_result parsed = std::from_chars(cursor.data(), cursor.data() + cursor.size(), value);
			if (!digitsOnly || parsed.ec != std::errc() || parsed.ptr != cursor.data() + cursor.size())
			{
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, "/cursor", std::format("'{}' is not a cursor", cursor),
					"pass \"\" (the oldest entry), \"end\" or the nextCursor of the previous read"));
			}
			return std::optional<uint64_t>(value);
		}

		EngineEvent MakePlayStateChangedEvent(const PlaySession* session)
		{
			EngineEvent event;
			event.Type = EngineEventType::PlayStateChanged;
			if (session != nullptr)
				event.Tick = session->GetTick();
			event.Name = std::string(PlayRunStateToString(GetPlayRunState(session)));
			return event;
		}

		DisconnectedLockstep ReleaseDisconnectedLockstep(PlaySession& session, ClientId client)
		{
			if (client == NoClient || !session.IsLockstep() || session.GetLockstepOwner() != client)
				return {};
			const PlayRunState before = GetPlayRunState(&session);
			session.SetLockstep(false);
			session.SetPaused(true);
			return DisconnectedLockstep{ .Released = true, .StateChanged = GetPlayRunState(&session) != before };
		}

	}

}
