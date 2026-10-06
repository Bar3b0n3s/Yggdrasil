#pragma once

// Seeded defect: only Base.h defines the marker; a second definition is an occurrence like any other.
#define ENGINE_CONTRACT_STUB() static_cast<void>(0)
