#pragma once

// Seeded defect: another public Core header includes spdlog, which would leak it into every module.
#include <spdlog/fmt/chrono.h>

namespace Engine {

	double ElapsedSeconds();

}
