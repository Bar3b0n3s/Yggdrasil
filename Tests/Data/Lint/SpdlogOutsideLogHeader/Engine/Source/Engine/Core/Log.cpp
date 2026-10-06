#include "Engine/Core/Log.h"

// Control: Core's source files may use spdlog too.
#include <spdlog/sinks/stdout_sinks.h>

namespace Engine {

	void FlushLogs()
	{
		spdlog::default_logger()->flush();
	}

}
