#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>

// What the play and input methods share (Automation/Methods/PlayMethods.cpp, InputMethods.cpp): the session a request
// addresses, the lockstep rule of §13.6 and the state hash's text. What every method domain shares (located param errors,
// the PlayStateChanged event) is in SharedMethodSupport.h.

namespace Engine {

	class AutomationMethodContext;
	class PlaySession;

	namespace Utils {

		// The host's play session. Errors: InvalidState "not playing" (hint: play.start).
		[[nodiscard]] Result<PlaySession*> RequirePlaySession(const AutomationMethodContext& context);

		// §13.6 "Lockstep is owned by one client; others get InvalidState": OK unless `session` is in lockstep and the
		// requesting client does not own it; the error names the owner (or the in-process driver that owns it).
		[[nodiscard]] Status CheckLockstepOwner(const AutomationMethodContext& context, const PlaySession& session);

		// A state hash as automation reports it: 16 lowercase hex digits (§4.8).
		[[nodiscard]] std::string FormatStateHash(uint64_t hash);

	}

}
