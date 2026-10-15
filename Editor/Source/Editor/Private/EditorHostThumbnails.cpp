#include "EditorPCH.h"
#include "Editor/Private/EditorHostThumbnails.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Viewport/EditorCamera.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/HostImageUpload.h"
#include "Engine/ImGui/ImGuiRenderer.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/EntityBounds.h"
#include "Engine/Scene/PrefabAsset.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace Engine {

	EditorHostThumbnails::EditorHostThumbnails(EditorContext& editor, Capture capture)
		: m_Editor(editor), m_Capture(std::move(capture)), m_Cache(editor, [this](const ThumbnailRequest& request)
	{
		return Render(request);
	})
	{
	}
	EditorHostThumbnails::~EditorHostThumbnails()
	{
		Reset();
	}
	void EditorHostThumbnails::SetGraphics(GraphicsDevice& device, ImGuiRenderer& renderer)
	{
		m_Device = &device;
		m_Renderer = &renderer;
	}
	void EditorHostThumbnails::Reset()
	{
		if (m_Renderer)
			for (const auto& [key, entry] : m_Entries)
			{
				(void)key;
				if (entry.Texture != 0)
					m_Renderer->RemoveTexture(entry.Texture);
			}
		m_Entries.clear();
		m_Cache.Reset();
	}
	uint64_t EditorHostThumbnails::FindTexture(const ThumbnailRequest& request)
	{
		if (request.ProjectGeneration == 0 || request.ProjectGeneration != m_Cache.GetProjectGeneration())
			return 0;
		if (m_Editor.GetAssets().GetVersion(request.Asset) != request.Version || m_Editor.GetAssets().GetAssetType(request.Asset) == AssetType::None)
			return 0;
		const Key key{ request.ProjectGeneration, request.Asset, request.Size };
		if (!m_Entries.contains(key) && m_Entries.size() >= 256)
			return 0;
		Entry& entry = m_Entries.try_emplace(key, Entry{ .Request = request }).first->second;
		entry.Request = request;
		entry.LastUse = ++m_Use;
		return entry.Texture;
	}
	Status EditorHostThumbnails::Pump()
	{
		// Deletion and project changes retire registrations after the previous UI frame was submitted.
		for (auto item = m_Entries.begin(); item != m_Entries.end();)
		{
			const auto& request = item->second.Request;
			if (request.ProjectGeneration != m_Cache.GetProjectGeneration() || m_Editor.GetAssets().GetAssetType(request.Asset) == AssetType::None)
			{
				if (m_Renderer && item->second.Texture != 0)
					m_Renderer->RemoveTexture(item->second.Texture);
				item = m_Entries.erase(item);
			}
			else
				++item;
		}
		if (!m_Device || !m_Renderer)
			return {};
		// Bounded registrations; evict outside Draw so the current frame never references a removed key.
		if (m_Entries.size() >= 256)
		{
			const auto oldest = std::ranges::min_element(m_Entries, {}, [](const auto& value)
			{
				return value.second.LastUse;
			});
			if (oldest->second.Texture != 0)
				m_Renderer->RemoveTexture(oldest->second.Texture);
			m_Entries.erase(oldest);
		}
		std::optional<Error> failure;
		bool uploaded = false;
		for (auto& [key, entry] : m_Entries)
		{
			(void)key;
			// The asset may have changed version since Draw asked; the cache answers for the version it has now.
			ThumbnailRequest current = entry.Request;
			current.Version = m_Editor.GetAssets().GetVersion(current.Asset);
			const auto found = m_Cache.Find(current);
			if (!found || (found->has_value() && (**found).TypeIcon))
			{
				// A remembered failure or a type icon draws the vector icon: release the image.
				if (entry.Texture != 0)
					m_Renderer->RemoveTexture(entry.Texture);
				entry.Texture = 0;
				entry.Path.clear();
				continue;
			}
			// Pending regeneration keeps the previous image; an unchanged path or a failed upload is not uploaded again.
			if (!found->has_value() || (**found).Path == entry.Path || (**found).Path == entry.FailedPath || uploaded)
				continue;
			const std::string path = (**found).Path;
			auto image = ReadPng(FileSystem::PathFromUtf8(path));
			Result<uint64_t> texture = [this, &image]() -> Result<uint64_t>
			{
				if (!image)
					return std::unexpected(std::move(image).error());
				nvrhi::TextureDesc desc;
				desc.width = image->Width;
				desc.height = image->Height;
				desc.format = nvrhi::Format::RGBA8_UNORM;
				desc.debugName = "Editor thumbnail";
				const TextureSubresourceData data{ .Data = image->Pixels, .RowPitch = image->GetRowPitch() };
				ENGINE_TRY_ASSIGN(auto created, m_Device->GetHostImageUpload().CreateTexture(desc, std::span(&data, 1)));
				return m_Renderer->AddTexture(*created.Texture);
			}();
			if (!texture)
			{
				entry.FailedPath = path;
				if (!failure)
					failure = std::move(texture).error();
				continue;
			}
			if (entry.Texture != 0)
				m_Renderer->RemoveTexture(entry.Texture);
			entry.Texture = *texture;
			entry.Path = path;
			entry.FailedPath.clear();
			uploaded = true; // one upload per safe point
		}
		return failure ? Status(std::unexpected(std::move(*failure))) : Status{};
	}
	Result<Image> EditorHostThumbnails::Render(const ThumbnailRequest& request)
	{
		ENGINE_TRY_ASSIGN(const AssetRef<Asset> asset, m_Editor.GetAssets().Load(request.Asset));
		if (const auto texture = AssetCast<TextureData>(asset))
		{
			ENGINE_TRY(ValidateTextureData(*texture));
			ENGINE_TRY_ASSIGN(Image image, CreateImage(request.Size, request.Size, nvrhi::Format::RGBA8_UNORM));
			const auto& mip = texture->Mips.front();
			const uint32_t channels = GetTextureBytesPerPixel(texture->Format);
			const float scale = std::min(static_cast<float>(request.Size) / static_cast<float>(mip.Width), static_cast<float>(request.Size) / static_cast<float>(mip.Height));
			const uint32_t width = std::max(1U, static_cast<uint32_t>(static_cast<float>(mip.Width) * scale));
			const uint32_t height = std::max(1U, static_cast<uint32_t>(static_cast<float>(mip.Height) * scale));
			for (uint32_t y = 0; y < request.Size; ++y)
				for (uint32_t x = 0; x < request.Size; ++x)
				{
					const size_t dest = (static_cast<size_t>(y) * request.Size + x) * 4;
					image.Pixels[dest + 3] = std::byte{ 255 };
					const uint32_t left = (request.Size - width) / 2, top = (request.Size - height) / 2;
					if (x < left || x >= left + width || y < top || y >= top + height)
						continue;
					const uint32_t sx = (x - left) * mip.Width / width, sy = (y - top) * mip.Height / height;
					const size_t source = (static_cast<size_t>(sy) * mip.Width + sx) * channels;
					for (uint32_t c = 0; c < 3; ++c)
						image.Pixels[dest + c] = texture->Pixels[source + (channels == 1 ? 0 : c)];
					if (channels == 4)
						image.Pixels[dest + 3] = texture->Pixels[source + 3];
				}
			return image;
		}
		if (!m_Capture)
			return MakeError(ErrorCode::Unsupported, "This thumbnail needs a rendering device");
		UUIDGenerator ids = UUIDGenerator::CreateDeterministic(0);
		Scene scene({ .Name = "Thumbnail", .Registry = &m_Editor.GetTypeRegistry(), .IdGenerator = &ids });
		if (asset->GetAssetType() == AssetType::Prefab)
		{
			LoadReport report;
			const PrefabInstantiateOptions instance{ .PrefabHandle = request.Asset, .RootID = ids.Next(), .Parent = {}, .SiblingIndex = {}, .RootTransform = {} };
			ENGINE_TRY_ASSIGN(const auto schemas, m_Editor.GetScriptSchemaSnapshot());
			ENGINE_TRY(InstantiatePrefabAsset(scene, m_Editor.GetAssets(), instance, { .Schemas = schemas.get() }, report));
		}
		else if (asset->GetAssetType() == AssetType::Mesh || asset->GetAssetType() == AssetType::Material)
		{
			const Entity entity = scene.CreateEntity("Preview");
			MeshRendererComponent mesh;
			mesh.Mesh = TypedAssetHandle<AssetType::Mesh>(asset->GetAssetType() == AssetType::Mesh ? request.Asset : BuiltinAssetHandles::SphereMesh);
			if (asset->GetAssetType() == AssetType::Material)
				mesh.Materials = { TypedAssetHandle<AssetType::Material>(request.Asset) };
			entity.AddComponent<MeshRendererComponent>(std::move(mesh));
		}
		else if (asset->GetAssetType() == AssetType::Environment)
		{
			EnvironmentComponent environment;
			environment.Environment = TypedAssetHandle<AssetType::Environment>(request.Asset);
			scene.CreateEntity("Environment").AddComponent<EnvironmentComponent>(environment);
		}
		else
			return MakeError(ErrorCode::Unsupported, "This asset uses a type icon");
		if (asset->GetAssetType() != AssetType::Environment)
		{
			DirectionalLightComponent light;
			light.CastShadows = false;
			const Entity lamp = scene.CreateEntity("Preview light");
			lamp.AddComponent<DirectionalLightComponent>(light);
			lamp.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(-35.0f, 45.0f, 0.0f));
			});
		}
		TransformSystem::Update(scene);
		ExplicitRenderCamera camera;
		glm::vec3 minimum(std::numeric_limits<float>::max()), maximum(std::numeric_limits<float>::lowest());
		bool hasBounds = false;
		for (const UUID id : scene.GetCanonicalOrder())
			if (const auto bounds = ComputeEntityWorldBounds(scene.FindEntityByID(id), m_Editor.GetAssets()))
			{
				minimum = glm::min(minimum, bounds->Min);
				maximum = glm::max(maximum, bounds->Max);
				hasBounds = true;
			}
		if (hasBounds)
		{
			ENGINE_TRY_ASSIGN(camera, EditorCamera::FrameBounds(camera, minimum, maximum, 1.0f));
		}
		// Thumbnails are a capture path: they render with the open project's quality settings, like every view, as the
		// cache reports them, since it keys captured images by that quality.
		const RenderExtractionRequest extraction{
			.Camera = RenderCameraSource::Explicit,
			.ExplicitCamera = camera,
			.Width = request.Size,
			.Height = request.Size,
			.Quality = m_Cache.GetRenderQuality(),
		};
		ENGINE_TRY_ASSIGN(const RenderSnapshot snapshot, ExtractRenderSnapshot(scene, extraction));
		return m_Capture(snapshot, request.Size);
	}

}
