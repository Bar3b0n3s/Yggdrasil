#include "EnginePCH.h"
#include "Engine/Platform/PollingFileWatcher.h"

// M2 contract stub (Roadmap rule 3): stream C (file watcher and socket) implements scanning, debouncing and MarkKnown.
// Until then Start, Poll and MarkKnown fail with Unsupported.

namespace Engine {

	PollingFileWatcher::PollingFileWatcher(const VirtualFileSystem& /*vfs*/, PollingFileWatcherSpecification /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Status PollingFileWatcher::Start()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PollingFileWatcher::Start is not implemented yet");
	}

	Result<std::vector<FileChange>> PollingFileWatcher::Poll(double /*nowSeconds*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PollingFileWatcher::Poll is not implemented yet");
	}

	Status PollingFileWatcher::MarkKnown(const VfsPath& /*path*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PollingFileWatcher::MarkKnown is not implemented yet");
	}

	const PollingFileWatcherSpecification& PollingFileWatcher::GetSpecification() const
	{
		ENGINE_CONTRACT_STUB();
		static const PollingFileWatcherSpecification EmptySpecification;
		return EmptySpecification;
	}

	std::string_view FileChangeKindToString(FileChangeKind /*kind*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
