#include "EnginePCH.h"
#include "Engine/Physics/ContactBuffer.h"

// M11 contract stubs (Docs/Decisions/0014-m11-decisions.md): stream A implements the buffer.

namespace Engine {

	void ContactBuffer::Append(const ContactEvent& /*event*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	std::vector<ContactEvent> ContactBuffer::Drain()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void ContactBuffer::Clear()
	{
		ENGINE_CONTRACT_STUB();
	}

	size_t ContactBuffer::GetSize() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

}
