#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine {

	// Fixed ticks of a script wait of `seconds` (Task.Wait, Task.Delay): ceil(seconds / fixedDelta). Both operands are
	// decimal values rounded to double, so an exact multiple of the tick can divide to a few ulps above its integer (0.14 s
	// at 50 Hz is 7.000000000000001); such a quotient is that integer, and anything farther away still rounds up. The
	// tolerance stays below the spacing of one tick. Not clamped: callers apply their minimum of one tick and their range
	// checks, and a non-finite quotient is returned unchanged for them to reject. Deterministic: exact operations only.
	[[nodiscard]] inline double ScriptSecondsToTicks(double seconds, double fixedDelta)
	{
		const double exact = seconds / fixedDelta;
		if (!std::isfinite(exact))
			return exact;
		const double nearest = std::round(exact);
		const double tolerance = std::min(1e-9, 8.0 * std::numeric_limits<double>::epsilon() * std::abs(exact));
		return std::abs(exact - nearest) <= tolerance ? nearest : std::ceil(exact);
	}

}
