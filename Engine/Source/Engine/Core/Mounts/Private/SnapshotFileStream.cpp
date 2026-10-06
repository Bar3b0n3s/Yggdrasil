#include "EnginePCH.h"
#include "Engine/Core/Mounts/Private/SnapshotFileStream.h"

namespace Engine {

	SnapshotFileStream::SnapshotFileStream(Buffer data)
		: m_Data(std::move(data))
	{
	}

	Result<size_t> SnapshotFileStream::Read(std::span<std::byte> destination)
	{
		const auto position = static_cast<size_t>(m_Position);
		const size_t count = std::min(destination.size(), m_Data.size() - position);
		if (count != 0)
			std::memcpy(destination.data(), m_Data.data() + position, count);
		m_Position += count;
		return count;
	}

	Status SnapshotFileStream::Seek(uint64_t position)
	{
		if (position > m_Data.size())
			return MakeError(ErrorCode::InvalidArgument, "cannot seek to offset {}: the file has {} bytes", position, m_Data.size());
		m_Position = position;
		return {};
	}

}
