#pragma once

// Precompiled header of the Tests project (Architecture §2.2). Every Tests .cpp file includes it first; headers never
// include it. It includes Support/GlmApprox.h so doctest's StringMaker specializations for glm types are visible in
// every translation unit: a file that CHECKed a glm value without them would instantiate the primary template, an ODR
// violation whose winner depends on the linker (Docs/Decisions/0006-m3-decisions.md, decision 19).

#include "Engine/Core/Base.h"
#include "Support/GlmApprox.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
