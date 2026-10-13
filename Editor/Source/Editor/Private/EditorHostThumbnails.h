#pragma once
#include "EditorCore/Thumbnails/ThumbnailCache.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <functional>
#include <map>
#include <string>
#include <tuple>

namespace Engine {

	class GraphicsDevice;
	class ImGuiRenderer;
	// Main-thread host. Draw only records requested keys; Pump uploads outside ImGui traversal.
	// Borrowed editor/device/renderer outlive this owner; Reset retires texture keys before project unmount.
	// A registration belongs to an asset and size in one binding and follows the cache's current image: a regenerated
	// image (another PNG path) replaces it at a safe point, so a tile keeps showing the previous image until then.
	class EditorHostThumbnails
	{
	public:
		// Renders one offscreen view; the production capture never waits for unrelated loads (EditorApp).
		using Capture = std::function<Result<Image>(const RenderSnapshot&, uint32_t)>;
		EditorHostThumbnails(EditorContext& editor, Capture capture);
		~EditorHostThumbnails();
		void SetGraphics(GraphicsDevice& device, ImGuiRenderer& renderer);
		[[nodiscard]] ThumbnailCache& GetCache() { return m_Cache; }
		// Memory only: the registered texture of the request's asset and size, or 0. A request for an older version of
		// the asset, or of another binding, gets 0.
		[[nodiscard]] uint64_t FindTexture(const ThumbnailRequest& request);
		[[nodiscard]] Status Pump();
		void Reset();
		// The renderer the cache calls: decoded pixels for textures, and for meshes, materials, prefabs and environments
		// an offscreen capture of a framed preview scene with the open project's Rendering quality settings.
		[[nodiscard]] Result<Image> Render(const ThumbnailRequest& request);
	private:
		struct Entry
		{
			ThumbnailRequest Request{};
			uint64_t Texture = 0;
			std::string Path{};       // the PNG Texture shows
			std::string FailedPath{}; // a PNG whose upload failed, not retried until the cache names another
			uint64_t LastUse = 0;
		};
		EditorContext& m_Editor;
		Capture m_Capture{};
		ThumbnailCache m_Cache;
		GraphicsDevice* m_Device = nullptr;
		ImGuiRenderer* m_Renderer = nullptr;
		using Key = std::tuple<uint64_t, AssetHandle, uint32_t>;
		std::map<Key, Entry> m_Entries{};
		uint64_t m_Use = 0;
	};

}
