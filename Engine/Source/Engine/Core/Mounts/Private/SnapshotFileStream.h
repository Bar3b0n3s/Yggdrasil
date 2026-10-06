#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace Engine {

	// The IFileStream of the native, memory and overlay mounts: it owns a copy of the file's content taken at Open, so it
	// keeps no host file open and outlives its mount (IFileStream contract, ADR 0003 decision 15).
	class SnapshotFileStream final : public IFileStream
	{
	public:
		explicit SnapshotFileStream(Buffer data);

		[[nodiscard]] Result<size_t> Read(std::span<std::byte> destination) override;
		[[nodiscard]] Status Seek(uint64_t position) override;
		[[nodiscard]] uint64_t GetPosition() const override { return m_Position; }
		[[nodiscard]] uint64_t GetSize() const override { return m_Data.size(); }
	private:
		Buffer m_Data;
		uint64_t m_Position = 0;
	};

}
