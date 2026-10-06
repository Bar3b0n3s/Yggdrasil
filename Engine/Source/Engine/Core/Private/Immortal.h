#pragma once

#include "Engine/Core/Base.h"

#include <utility>

namespace Engine {

	namespace Utils {

		// Storage for process-level state that is still used during static destruction, whose destructor never runs.
		// Statics are destroyed at exit in the reverse order of their initialization, so an ordinary static could be gone
		// before a static destructor that still logs or ends the process with FatalError (Log.h: nothing crashes after
		// Shutdown; FatalError.h: it works at any point of the process lifetime). The memory is reclaimed with the
		// process. The constructor is constexpr when T's is, so a constinit Immortal is constant-initialized.
		template<typename T>
		union Immortal
		{
			template<typename... Args>
			constexpr explicit Immortal(Args&&... args)
				: Value(std::forward<Args>(args)...)
			{
			}

			~Immortal()
			{
			}

			Immortal(const Immortal&) = delete;
			Immortal& operator=(const Immortal&) = delete;
			Immortal(Immortal&&) = delete;
			Immortal& operator=(Immortal&&) = delete;

			T Value;
		};

	}

}
