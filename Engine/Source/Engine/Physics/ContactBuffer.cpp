#include "EnginePCH.h"
#include "Engine/Physics/ContactBuffer.h"

namespace Engine {

	void ContactBuffer::Append(const ContactEvent& event)
	{
		const std::scoped_lock lock(m_Mutex);
		m_Events.push_back(event);
	}

	std::vector<ContactEvent> ContactBuffer::Drain()
	{
		std::vector<ContactEvent> drained;
		{
			const std::scoped_lock lock(m_Mutex);
			drained.swap(m_Events);
		}
		return drained;
	}

	void ContactBuffer::Clear()
	{
		const std::scoped_lock lock(m_Mutex);
		m_Events.clear();
	}

	size_t ContactBuffer::GetSize() const
	{
		const std::scoped_lock lock(m_Mutex);
		return m_Events.size();
	}

}
