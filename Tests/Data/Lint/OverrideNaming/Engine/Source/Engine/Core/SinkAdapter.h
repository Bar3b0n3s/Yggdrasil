#pragma once

namespace Engine {

	// Stands in for a third-party interface whose names break the naming rules. In first-party code they are the seeded
	// defects: a declaration that is not an override is checked like any other.
	class SinkBase
	{
	public:
		virtual ~SinkBase() = default;
		virtual void flush_() = 0;
		virtual void sink_it_(int value) = 0;
	};

	// The controls: overrides keep the names of the functions they override, with either virt-specifier.
	class SinkAdapter final : public SinkBase
	{
	public:
		~SinkAdapter() override;
		void flush_() override {}
		void sink_it_(int value) final { m_Total += value; }
	private:
		int m_Total = 0;
	};

}
