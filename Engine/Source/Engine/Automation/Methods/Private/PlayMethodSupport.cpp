#include "EnginePCH.h"
#include "Engine/Automation/Methods/Private/PlayMethodSupport.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Session/PlaySession.h"

#include <format>
#include <string>

namespace Engine {

	namespace Utils {

		Result<PlaySession*> RequirePlaySession(const AutomationMethodContext& context)
		{
			PlaySession* session = context.GetPlaySession();
			if (session == nullptr)
				return std::unexpected(Error(ErrorCode::InvalidState, "not playing: there is no play session").WithHint("start one with play.start"));
			return session;
		}

		Status CheckLockstepOwner(const AutomationMethodContext& context, const PlaySession& session)
		{
			if (!session.IsLockstep())
				return {};
			const ClientId owner = session.GetLockstepOwner();
			if (owner != NoClient && owner == context.GetRequest().Client)
				return {};
			const std::string name = owner == NoClient ? std::string() : context.GetClientName(owner);
			const std::string ownerText = owner == NoClient ? std::string("an in-process driver") : std::format("client '{}'", name.empty() ? std::to_string(owner) : name);
			return std::unexpected(Error(ErrorCode::InvalidState, std::format("lockstep is owned by {}: only it advances or controls the session", ownerText))
					.WithHint("read the session with play.state, or wait until the owner stops it or disconnects"));
		}

		std::string FormatStateHash(uint64_t hash)
		{
			return std::format("{:016x}", hash);
		}

	}

}
