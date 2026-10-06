#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Frame clocks (Architecture §4.2). The frame loop asks its clock for the time that passed since the previous frame
// and feeds it to FixedStepScheduler::Advance. Simulation never reads a clock directly.

namespace Engine {

	enum class ClockKind : uint8_t
	{
		System,  // steady wall clock: windowed runs
		Manual,  // exactly one FixedDelta per frame: headless runs, lockstep play, most tests
		Scripted // a table of deltas, cycled: the FeatureTest Timing suite
	};

	// A source of frame deltas. Instances are owned by the frame loop and used from one thread at a time.
	class Clock
	{
	public:
		virtual ~Clock() = default;

		// Seconds since the previous call; finite and >= 0. The first call's value is documented per clock.
		[[nodiscard]] virtual double Delta() = 0;

		[[nodiscard]] virtual ClockKind GetKind() const = 0;
	};

	// std::chrono::steady_clock. The first Delta() returns 0; later calls return the steady time elapsed since the
	// previous call.
	class SystemClock final : public Clock
	{
	public:
		SystemClock() = default;

		[[nodiscard]] double Delta() override;
		[[nodiscard]] ClockKind GetKind() const override { return ClockKind::System; }
	private:
		int64_t m_PreviousNanoseconds = 0; // steady-clock time of the previous call
		bool m_HasPrevious = false;
	};

	// Returns its fixed delta on every call, the first one included. With a ManualClock every frame runs exactly one
	// fixed step; the frame loop uses FixedStepScheduler::StepExactly for it, so the frame's Alpha is 1 (§4.2).
	class ManualClock final : public Clock
	{
	public:
		// `fixedDelta` must be finite and > 0 (asserted).
		explicit ManualClock(double fixedDelta);

		[[nodiscard]] double Delta() override;
		[[nodiscard]] ClockKind GetKind() const override { return ClockKind::Manual; }

		[[nodiscard]] double GetFixedDelta() const { return m_FixedDelta; }
	private:
		double m_FixedDelta = 0.0;
	};

	// Replays a table of frame deltas: call n returns deltas[n % deltas.size()], the first call included. Deterministic.
	class ScriptedClock final : public Clock
	{
	public:
		// Validates the table: it must be non-empty and every delta finite and >= 0 (InvalidArgument otherwise; the
		// message names the first offending index). Tables come from test data, so they are checked, not asserted.
		[[nodiscard]] static Result<ScriptedClock> Create(std::span<const double> deltas);

		[[nodiscard]] double Delta() override;
		[[nodiscard]] ClockKind GetKind() const override { return ClockKind::Scripted; }

		[[nodiscard]] std::span<const double> GetDeltas() const { return m_Deltas; }
		// How many deltas have been returned so far.
		[[nodiscard]] uint64_t GetCallCount() const { return m_CallCount; }
	private:
		explicit ScriptedClock(std::vector<double> deltas);
	private:
		std::vector<double> m_Deltas;
		uint64_t m_CallCount = 0;
	};

}
