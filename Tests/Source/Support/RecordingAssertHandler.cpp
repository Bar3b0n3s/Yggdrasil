#include "TestsPCH.h"
#include "Support/RecordingAssertHandler.h"

#include "Engine/Core/FatalError.h"

// M1 contract stub (Roadmap rule 3): stream E implements logging the assertion and recording the doctest failure. The
// stub keeps the essential property: it never returns.

namespace Engine {

	namespace Test {

		void RecordingAssertHandler(const AssertInfo& info)
		{
			FatalError(FatalErrorKind::Assert, info.Message);
		}

		void InstallRecordingAssertHandler()
		{
		}

	}

}
