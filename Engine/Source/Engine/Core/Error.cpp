#include "EnginePCH.h"
#include "Engine/Core/Error.h"

// M1 contract stub (Roadmap rule 3): stream A implements the code names and the one-line rendering. The builders and
// accessors are inline in Error.h.

namespace Engine {

	std::string_view ErrorCodeToString(ErrorCode /*code*/)
	{
		return {};
	}

	std::string Error::ToString() const
	{
		return m_Message;
	}

}
