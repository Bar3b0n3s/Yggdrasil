#pragma once

namespace Engine {

	class FeatureTestRunner
	{
	public:
		int GetFailureCount() const { return m_FailureCount; }
	private:
		int m_FailureCount = 0;
	};

}
