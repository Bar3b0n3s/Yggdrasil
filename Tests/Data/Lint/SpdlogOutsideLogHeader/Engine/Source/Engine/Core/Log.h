#pragma once

// Control: Log.h is the one public Core header that may include spdlog (Architecture section 3).
#include <spdlog/spdlog.h>

namespace Engine {

	void FlushLogs();

}
