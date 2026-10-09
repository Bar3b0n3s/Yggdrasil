#pragma once
#include "EditorCore/Thumbnails/ThumbnailCache.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <functional>
#include <map>
#include <tuple>

namespace Engine {

	class GraphicsDevice;
	class ImGuiRenderer;
	// Main-thread host. Draw only records requested keys; Pump loads/uploads outside ImGui traversal.
	// Borrowed editor/device/renderer outlive this owner; Reset retires texture keys before project unmount.
	class EditorHostThumbnails
	{
	public:
		using Capture = std::function<Result<Image>(const RenderSnapshot&, uint32_t)>;
		EditorHostThumbnails(EditorContext& editor, Capture capture);
		~EditorHostThumbnails();
		void SetGraphics(GraphicsDevice& device, ImGuiRenderer& renderer);
		[[nodiscard]] ThumbnailCache& GetCache() { return m_Cache; }
		[[nodiscard]] uint64_t FindTexture(const ThumbnailRequest& request);
		[[nodiscard]] Status Pump();
		void Reset();
		[[nodiscard]] Result<Image> Render(const ThumbnailRequest& request);
	private:
		struct Entry
		{
			ThumbnailRequest Request{};
			uint64_t Texture = 0;
			uint64_t LastUse = 0;
			bool Failed = false;
		};
		EditorContext& m_Editor;
		Capture m_Capture{};
		ThumbnailCache m_Cache;
		GraphicsDevice* m_Device = nullptr;
		ImGuiRenderer* m_Renderer = nullptr;
		using Key = std::tuple<uint64_t, AssetHandle, uint64_t, uint32_t>;
		std::map<Key, Entry> m_Entries{};
		uint64_t m_Use = 0;
	};

}
