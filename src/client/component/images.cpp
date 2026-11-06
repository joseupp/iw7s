#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include "command.hpp"
#include "console/console.hpp"
#include "directx.hpp"
#include "filesystem.hpp"
#include "scheduler.hpp"
#include "images.hpp"

#include <utils/hook.hpp>
#include <utils/image.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>

// DirectXMath tiene variables llamadas r11 y los registros de asmjit (utils/hook.hpp) son globales: C4459
#pragma warning(push)
#pragma warning(disable: 4459)
#include <DirectXTex.h>
#pragma warning(pop)

// Texturas sustituidas por archivos sueltos (como images.cpp de h1-mod): iw7s\images\<nombre de la imagen>.dds o .png
//
// DDS (BC1/BC3/BC4/BC5/BC7/RGBA, con mips, como las del juego: tools\png2dds o images_dump_dds): se sube tal cual a la
// GPU. PNG: se descomprime y se le calculan los mips. Si hay los dos con el mismo nombre, manda el DDS. Todos los de
// la carpeta se cargan en segundo plano al arrancar, para que no haya espera la primera vez que se usan.
//
// Imágenes sin streaming (los iconos de la interfaz, como los botones del mando): se cambian al cargarlas, cuando la
// imagen entra en la base de datos (DB_AddXAsset, fastfiles.cpp -> images::on_image_added). La textura del PNG pasa a
// ser la de la imagen para siempre: el juego no vuelve a escribirla y la libera él al descargar la zona.
//
// Imágenes con streaming (la mayoría de texturas 3D, desde los imagefile*.pak) o que ya estaban cargadas (por ejemplo
// tras images_reload): se buscan en el pool las que tienen PNG y se pone nuestra textura en su GfxImage cada frame.
// Si el streaming vuelve a escribir la suya, en el siguiente frame se vuelve a poner la nuestra.
//
// Al cargar zonas (DB_LoadXAssets, también las transitorias de armas que se cargan todo el rato en los menús) solo
// se vuelven a buscar imágenes: las que ya están puestas se mantienen. Antes de descargar zonas
// (DB_UnloadFastfilesByZoneFlags) se devuelven las texturas originales y se olvidan las imágenes, porque sus
// GfxImage pueden liberarse; se vuelven a buscar 0,3 s después.
//
// Solo imágenes 2D. El nombre del archivo es el de la imagen en minúsculas (listassetpool 18), con los caracteres que no
// valen en un nombre de archivo cambiados por '_'.
//
// images_dump [filtro]: guarda las imágenes cargadas en iw7s\images_dump (PNG las 2D, DDS el resto), con el mismo
// nombre que hay que usar en iw7s\images. Se guarda el mip más grande que tenga cargado el streaming en ese momento.
// images_dump_dds [filtro]: igual, pero todas en DDS con su formato original y sus mips (referencia para png2dds).
namespace images
{
	namespace
	{
		struct custom_texture
		{
			ID3D11Texture2D* texture{};
			ID3D11ShaderResourceView* view_srgb{};
			ID3D11ShaderResourceView* view_linear{};
			bool failed{};
		};

		struct target
		{
			game::GfxImage* image{};
			custom_texture* custom{};
			game::GfxTexture original{};
			bool installed{};
		};

		std::mutex mutex;
		std::unordered_map<std::string, std::string> available; // nombre en minúsculas -> ruta relativa del PNG
		std::unordered_map<std::string, custom_texture> textures;
		std::vector<target> targets;
		bool folder_scanned = false;
		unsigned int preload_generation = 0;
		std::unordered_map<game::GfxImage*, custom_texture*> replaced_on_load;
		bool rescan_pending = true;
		bool unload_pending = false;
		std::chrono::steady_clock::time_point last_scan{};
		std::chrono::steady_clock::time_point last_load{};

		utils::hook::detour db_load_x_assets_hook;
		utils::hook::detour db_unload_fastfiles_by_zone_flags_hook;

		std::string file_name(const char* image_name)
		{
			auto name = utils::string::to_lower(image_name);
			for (auto& c : name)
			{
				if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' ||
					c == '*' || static_cast<unsigned char>(c) < 32)
				{
					c = '_';
				}
			}

			return name;
		}

		bool is_srgb(const DXGI_FORMAT format)
		{
			switch (format)
			{
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			case DXGI_FORMAT_BC1_UNORM_SRGB:
			case DXGI_FORMAT_BC2_UNORM_SRGB:
			case DXGI_FORMAT_BC3_UNORM_SRGB:
			case DXGI_FORMAT_BC7_UNORM_SRGB:
				return true;
			default:
				return false;
			}
		}

		void release(custom_texture& tex)
		{
			const auto try_release = []<typename T>(T*& resource)
			{
				if (resource)
				{
					resource->Release();
					resource = nullptr;
				}
			};

			try_release(tex.view_srgb);
			try_release(tex.view_linear);
			try_release(tex.texture);
		}

		void start_preload();

		void scan_folder()
		{
			available.clear();
			folder_scanned = true;

			auto png = 0, dds = 0;
			for (const auto& file : filesystem::list_files("images"))
			{
				const std::filesystem::path path(file);
				const auto ext = utils::string::to_lower(path.extension().string());
				const auto name = utils::string::to_lower(path.stem().string());

				if (ext == ".dds")
				{
					available[name] = file; // el DDS manda sobre el PNG
					++dds;
				}
				else if (ext == ".png")
				{
					available.emplace(name, file);
					++png;
				}
			}

			console::info("[images] %d DDS y %d PNG en images\n", dds, png);
			start_preload();
		}

		// Mips con filtro de caja (RGBA8)
		std::vector<std::string> build_mips(const utils::image& image, std::vector<std::pair<int, int>>& sizes)
		{
			std::vector<std::string> levels{};
			levels.push_back(image.get_data());

			auto width = image.get_width();
			auto height = image.get_height();
			sizes.emplace_back(width, height);

			while (width > 1 || height > 1)
			{
				const auto& src = levels.back();
				const auto new_width = std::max(1, width / 2);
				const auto new_height = std::max(1, height / 2);

				std::string dst(static_cast<size_t>(new_width) * new_height * 4, '\0');

				for (auto y = 0; y < new_height; ++y)
				{
					for (auto x = 0; x < new_width; ++x)
					{
						for (auto c = 0; c < 4; ++c)
						{
							auto sum = 0;
							for (auto dy = 0; dy < 2; ++dy)
							{
								for (auto dx = 0; dx < 2; ++dx)
								{
									const auto sx = std::min(width - 1, x * 2 + dx);
									const auto sy = std::min(height - 1, y * 2 + dy);
									sum += static_cast<uint8_t>(src[(static_cast<size_t>(sy) * width + sx) * 4 + c]);
								}
							}

							dst[(static_cast<size_t>(y) * new_width + x) * 4 + c] = static_cast<char>((sum + 2) / 4);
						}
					}
				}

				levels.push_back(std::move(dst));
				width = new_width;
				height = new_height;
				sizes.emplace_back(width, height);
			}

			return levels;
		}

		bool create_views(custom_texture& tex, const std::string& name, const DXGI_FORMAT srgb, const DXGI_FORMAT linear,
			const UINT mips)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC view{};
			view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			view.Texture2D.MipLevels = mips;

			view.Format = srgb;
			const auto srgb_ok = SUCCEEDED(dx::device->CreateShaderResourceView(tex.texture, &view, &tex.view_srgb));
			view.Format = linear;
			const auto linear_ok = SUCCEEDED(dx::device->CreateShaderResourceView(tex.texture, &view, &tex.view_linear));

			if (!srgb_ok || !linear_ok)
			{
				console::error("[images] CreateShaderResourceView ha fallado para %s\n", name.data());
				return false;
			}

			return true;
		}

		bool create_from_png(custom_texture& tex, const std::string& name, const std::string& data)
		{
			const utils::image image(data);

			std::vector<std::pair<int, int>> sizes{};
			const auto levels = build_mips(image, sizes);

			std::vector<D3D11_SUBRESOURCE_DATA> init(levels.size());
			for (size_t i = 0; i < levels.size(); ++i)
			{
				init[i].pSysMem = levels[i].data();
				init[i].SysMemPitch = sizes[i].first * 4;
				init[i].SysMemSlicePitch = init[i].SysMemPitch * sizes[i].second;
			}

			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = image.get_width();
			desc.Height = image.get_height();
			desc.MipLevels = static_cast<UINT>(levels.size());
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

			if (FAILED(dx::device->CreateTexture2D(&desc, init.data(), &tex.texture)))
			{
				console::error("[images] CreateTexture2D ha fallado para %s\n", name.data());
				return false;
			}

			if (!create_views(tex, name, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, desc.MipLevels))
			{
				return false;
			}

			console::info("[images] Cargada %s (PNG %dx%d)\n", name.data(), image.get_width(), image.get_height());
			return true;
		}

		bool create_from_dds(custom_texture& tex, const std::string& name, const std::string& data)
		{
			DirectX::TexMetadata meta{};
			DirectX::ScratchImage scratch{};
			if (const auto hr = DirectX::LoadFromDDSMemory(reinterpret_cast<const uint8_t*>(data.data()), data.size(), DirectX::DDS_FLAGS_NONE, &meta,
				scratch); FAILED(hr))
			{
				console::error("[images] %s: DDS no válido (hr 0x%08X)\n", name.data(), static_cast<unsigned>(hr));
				return false;
			}

			if (meta.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || meta.arraySize != 1 || meta.IsCubemap())
			{
				console::error("[images] %s: solo DDS 2D (sin cubemap ni array)\n", name.data());
				return false;
			}

			// Textura sin tipo cuando el formato tiene versión sRGB, para crear las dos vistas (sRGB y lineal)
			const auto linear = DirectX::MakeLinear(meta.format);
			const auto srgb = DirectX::MakeSRGB(linear);
			const auto has_srgb = srgb != linear;
			const auto tex_format = has_srgb ? DirectX::MakeTypeless(linear) : meta.format;

			std::vector<D3D11_SUBRESOURCE_DATA> init(meta.mipLevels);
			for (size_t i = 0; i < meta.mipLevels; ++i)
			{
				const auto* level = scratch.GetImage(i, 0, 0);
				init[i].pSysMem = level->pixels;
				init[i].SysMemPitch = static_cast<UINT>(level->rowPitch);
				init[i].SysMemSlicePitch = static_cast<UINT>(level->slicePitch);
			}

			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = static_cast<UINT>(meta.width);
			desc.Height = static_cast<UINT>(meta.height);
			desc.MipLevels = static_cast<UINT>(meta.mipLevels);
			desc.ArraySize = 1;
			desc.Format = tex_format;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

			if (const auto hr = dx::device->CreateTexture2D(&desc, init.data(), &tex.texture); FAILED(hr))
			{
				console::error("[images] CreateTexture2D ha fallado para %s (formato %d, hr 0x%08X)\n", name.data(),
					meta.format, static_cast<unsigned>(hr));
				return false;
			}

			if (!create_views(tex, name, has_srgb ? srgb : meta.format, has_srgb ? linear : meta.format, desc.MipLevels))
			{
				return false;
			}

			console::info("[images] Cargada %s (DDS %zux%zu, formato %d, %zu mips)\n", name.data(), meta.width,
				meta.height, meta.format, meta.mipLevels);
			return true;
		}

		// Sin tocar el estado compartido: se puede llamar sin el mutex (precarga)
		bool create_texture(custom_texture& tex, const std::string& name, const std::string& path)
		{
			std::string data{};
			if (!filesystem::read_file(path, &data) && !utils::io::read_file(path, &data))
			{
				console::error("[images] No se puede leer %s\n", path.data());
				return false;
			}

			try
			{
				const auto is_dds = utils::string::to_lower(std::filesystem::path(path).extension().string()) == ".dds";
				if (is_dds ? create_from_dds(tex, name, data) : create_from_png(tex, name, data))
				{
					return true;
				}
			}
			catch (const std::exception& e)
			{
				console::error("[images] %s: %s\n", path.data(), e.what());
			}

			release(tex);
			return false;
		}

		custom_texture* get_texture(const std::string& name, const std::string& path)
		{
			if (const auto it = textures.find(name); it != textures.end())
			{
				return it->second.failed ? nullptr : &it->second;
			}

			custom_texture tex{};
			tex.failed = !create_texture(tex, name, path);

			auto& entry = textures[name] = tex;
			return entry.failed ? nullptr : &entry;
		}

		// Todos los archivos de la carpeta, en otro hilo (crear texturas en el dispositivo D3D11 es seguro entre hilos)
		void start_preload()
		{
			if (!dx::device || available.empty())
			{
				return;
			}

			const auto generation = ++preload_generation;
			std::vector<std::pair<std::string, std::string>> list(available.begin(), available.end());

			std::thread([generation, list = std::move(list)]
			{
				for (const auto& [name, path] : list)
				{
					{
						std::lock_guard _(mutex);
						if (generation != preload_generation)
						{
							return;
						}

						if (textures.contains(name))
						{
							continue;
						}
					}

					custom_texture tex{};
					tex.failed = !create_texture(tex, name, path);

					std::lock_guard _(mutex);
					if (generation != preload_generation || textures.contains(name))
					{
						release(tex);
						continue;
					}

					textures[name] = tex;
				}
			}).detach();
		}

		// La vista que va en cada campo respeta si la original era sRGB o lineal
		ID3D11ShaderResourceView* pick_view(const custom_texture* tex, ID3D11ShaderResourceView* original,
			const DXGI_FORMAT fallback)
		{
			auto format = fallback;
			if (original)
			{
				D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
				original->GetDesc(&desc);
				format = desc.Format;
			}

			return is_srgb(format) ? tex->view_srgb : tex->view_linear;
		}

		void install(target& t)
		{
			auto* image = t.image;
			const auto* tex = t.custom;

			if (t.installed && image->texture.map == tex->texture &&
				(image->texture.shaderView == tex->view_srgb || image->texture.shaderView == tex->view_linear))
			{
				return;
			}

			// La textura actual es la del juego (primera vez o la ha vuelto a escribir el streaming)
			t.original = image->texture;

			auto* view = pick_view(tex, t.original.shaderView, image->imageFormat);
			auto* view_alt = t.original.shaderViewAlternate
				? pick_view(tex, t.original.shaderViewAlternate, image->imageFormat)
				: nullptr;

			// Una referencia más por si el juego libera lo que hay en la imagen al cambiar de mip
			tex->texture->AddRef();
			view->AddRef();
			if (view_alt)
			{
				view_alt->AddRef();
			}

			image->texture.shaderView = view;
			image->texture.shaderViewAlternate = view_alt;
			image->texture.map = tex->texture;
			t.installed = true;
		}

		void restore_all()
		{
			for (auto& t : targets)
			{
				if (t.installed && t.image->texture.map == t.custom->texture)
				{
					t.image->texture = t.original;
				}
			}

			targets.clear();
			rescan_pending = true;
			unload_pending = true;
			last_load = std::chrono::steady_clock::now();
		}

		void scan_pool()
		{
			// Las que ya estaban puestas se mantienen (con su textura original guardada)
			auto previous = std::move(targets);
			targets.clear();
			last_scan = std::chrono::steady_clock::now();

			if (available.empty())
			{
				return;
			}

			std::vector<std::pair<game::GfxImage*, std::string>> found{};
			game::DB_EnumXAssets(game::ASSET_TYPE_IMAGE, [&](const game::XAssetHeader header)
			{
				auto* image = header.image;
				if (!image || !image->name || image->mapType != game::MAPTYPE_2D)
				{
					return;
				}

				// Ya cambiada al cargarla
				if (const auto r = replaced_on_load.find(image);
					r != replaced_on_load.end() && image->texture.map == r->second->texture)
				{
					return;
				}

				auto name = file_name(image->name);
				if (available.contains(name))
				{
					found.emplace_back(image, std::move(name));
				}
			});

			for (auto& [image, name] : found)
			{
				const auto existing = std::ranges::find(previous, image, &target::image);
				if (existing != previous.end())
				{
					targets.push_back(*existing);
					continue;
				}

				auto* tex = get_texture(name, available[name]);
				if (tex)
				{
					targets.push_back({image, tex});
				}
			}
		}

		void frame()
		{
			if (!dx::device || !game::Sys_IsDatabaseReady())
			{
				return;
			}

			std::lock_guard _(mutex);

			// Tras una descarga de zonas se espera 0,3 s, por si la base de datos sigue liberando imágenes (las zonas de
			// armas se descargan y cargan al cambiar de arma o camuflaje: con 2 s el camuflaje tardaba en verse)
			const auto now = std::chrono::steady_clock::now();
			if (unload_pending)
			{
				if (now - last_load < 300ms)
				{
					return;
				}

				unload_pending = false;
			}

			// Además de tras cada carga, cada 5 s por las imágenes que llegan sin cargar zonas
			if (rescan_pending || now - last_scan > 5s)
			{
				scan_pool();
				rescan_pending = false;
			}

			for (auto& t : targets)
			{
				install(t);
			}
		}

		void db_load_x_assets_stub(game::XZoneInfo* zone_info, const unsigned int zone_count, const char sync_mode)
		{
			db_load_x_assets_hook.invoke<void>(zone_info, zone_count, sync_mode);

			std::lock_guard _(mutex);
			rescan_pending = true;
		}

		void db_unload_fastfiles_by_zone_flags_stub(const int flags)
		{
			{
				std::lock_guard _(mutex);
				restore_all();
			}

			db_unload_fastfiles_by_zone_flags_hook.invoke<void>(flags);
		}

		// Motivo del fallo en la consola, para las imágenes que se piden con filtro
		bool dump_image(const game::GfxImage* image, const std::filesystem::path& dir, const bool verbose, const bool dds)
		{
			const auto fail = [&](const char* reason, const HRESULT hr = S_OK)
			{
				if (verbose)
				{
					console::warn("[images] %s: %s (hr 0x%08X, formato %d, %dx%d, streamed %d, categoria %d)\n",
						image->name, reason, static_cast<unsigned>(hr), image->imageFormat, image->width,
						image->height, image->streamed, image->category);
				}

				return false;
			};

			// Sin vista la textura no está creada: en el campo puede haber datos de carga (loadDef) en lugar de una
			// textura D3D11
			auto* resource = image->texture.map;
			if (!resource || !image->texture.shaderView)
			{
				return fail("sin textura cargada");
			}

			DirectX::ScratchImage captured{};
			if (const auto hr = DirectX::CaptureTexture(dx::device, dx::deviceContext, resource, captured); FAILED(hr))
			{
				return fail("CaptureTexture", hr);
			}

			const auto name = file_name(image->name);

			// Formato con tipo: el de la vista del juego (sRGB o lineal)
			auto format = captured.GetMetadata().format;
			if (DirectX::IsTypeless(format))
			{
				auto typed = DirectX::MakeTypelessUNORM(format);
				if (image->texture.shaderView)
				{
					D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
					image->texture.shaderView->GetDesc(&desc);
					if (is_srgb(desc.Format))
					{
						typed = DirectX::MakeSRGB(typed);
					}
				}

				captured.OverrideFormat(typed);
				format = typed;
			}

			if (dds || image->mapType != game::MAPTYPE_2D)
			{
				const auto path = dir / (name + ".dds");
				const auto hr = DirectX::SaveToDDSFile(captured.GetImages(), captured.GetImageCount(),
					captured.GetMetadata(), DirectX::DDS_FLAGS_NONE, path.wstring().data());
				return SUCCEEDED(hr) || fail("SaveToDDSFile", hr);
			}

			const auto target = DirectX::IsSRGB(format) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
			const auto* top = captured.GetImage(0, 0, 0);

			// Ya en RGBA8: se guarda tal cual (Convert falla con el mismo formato de origen y destino)
			DirectX::ScratchImage converted{};
			const DirectX::Image* png = top;
			if (format != target)
			{
				const auto hr = DirectX::IsCompressed(format)
					? DirectX::Decompress(*top, target, converted)
					: DirectX::Convert(*top, target, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
				if (FAILED(hr))
				{
					return fail(DirectX::IsCompressed(format) ? "Decompress" : "Convert", hr);
				}

				png = converted.GetImage(0, 0, 0);
			}

			const auto path = dir / (name + ".png");
			const auto save_hr = DirectX::SaveToWICFile(*png, DirectX::WIC_FLAGS_FORCE_SRGB,
				DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), path.wstring().data());
			return SUCCEEDED(save_hr) || fail("SaveToWICFile", save_hr);
		}

		void dump(const std::string& filter, const bool dds)
		{
			if (!dx::device || !dx::deviceContext)
			{
				console::error("[images] Sin dispositivo D3D11\n");
				return;
			}

			const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

			const std::filesystem::path dir = "iw7s/images_dump";
			std::error_code ec{};
			std::filesystem::create_directories(dir, ec);

			// El contexto inmediato también lo usa el hilo de render: se protege solo mientras dura el volcado
			if (!dx::multithread)
			{
				console::error("[images] El contexto D3D11 no se puede proteger para varios hilos\n");
				return;
			}

			const auto was_protected = dx::multithread->SetMultithreadProtected(TRUE);

			// Que termine lo que el hilo de render ya hubiera empezado sin protección
			game::R_SyncRenderThread();
			std::this_thread::sleep_for(50ms);

			std::lock_guard _(mutex);

			std::vector<game::GfxImage*> list{};
			game::DB_EnumXAssets(game::ASSET_TYPE_IMAGE, [&](const game::XAssetHeader header)
			{
				auto* image = header.image;
				if (image && image->name && (filter.empty() || utils::string::match_compare(filter, image->name, false)))
				{
					list.push_back(image);
				}
			});

			auto saved = 0, failed = 0;
			for (const auto* image : list)
			{
				// Las nuestras no: se guardaría el archivo que ya hay
				auto ours = std::ranges::any_of(targets, [&](const target& t)
				{
					return t.image == image && t.installed && image->texture.map == t.custom->texture;
				});

				if (const auto r = replaced_on_load.find(const_cast<game::GfxImage*>(image));
					r != replaced_on_load.end() && image->texture.map == r->second->texture)
				{
					ours = true;
				}

				if (ours)
				{
					continue;
				}

				dx::multithread->Enter();
				const auto ok = dump_image(image, dir, !filter.empty(), dds);
				dx::multithread->Leave();

				if (ok)
				{
					++saved;
				}
				else
				{
					++failed;
				}
			}

			// Fuera de cualquier llamada del hilo de render (Enter / Leave) antes de quitar la protección
			if (!was_protected)
			{
				dx::multithread->Enter();
				dx::multithread->Leave();
				dx::multithread->SetMultithreadProtected(FALSE);
			}

			if (SUCCEEDED(com))
			{
				CoUninitialize();
			}

			console::info("[images] %d imágenes guardadas en iw7s\\images_dump (%d sin poder guardar)\n", saved, failed);
		}

		void reload()
		{
			std::lock_guard _(mutex);
			restore_all();

			// Las cambiadas al cargarlas son del juego desde entonces: sus texturas no se liberan aquí (el juego tiene sus
			// referencias) y siguen con el PNG anterior hasta que se vuelvan a cargar
			for (auto& [name, tex] : textures)
			{
				release(tex);
			}

			textures.clear();
			replaced_on_load.clear();
			scan_folder();
		}
	}

	void on_image_added(game::GfxImage* image)
	{
		if (!image || !image->name || image->mapType != game::MAPTYPE_2D || !dx::device)
		{
			return;
		}

		std::lock_guard _(mutex);

		if (!folder_scanned)
		{
			scan_folder();
		}

		if (available.empty())
		{
			return;
		}

		const auto name = file_name(image->name);
		const auto file = available.find(name);
		if (file == available.end())
		{
			return;
		}

		// Con streaming o sin textura creada todavía: se encarga el cambio de cada frame
		if (image->streamed || !image->texture.map || !image->texture.shaderView)
		{
			rescan_pending = true;
			return;
		}

		auto* tex = get_texture(name, file->second);
		if (!tex)
		{
			return;
		}

		auto* view = pick_view(tex, image->texture.shaderView, image->imageFormat);
		auto* view_alt = image->texture.shaderViewAlternate
			? pick_view(tex, image->texture.shaderViewAlternate, image->imageFormat)
			: nullptr;

		// Las referencias pasan al juego, que las libera al descargar la zona. Las texturas originales no se liberan
		// (no se sabe si las dos vistas comparten referencia): se pierde poca memoria por imagen.
		tex->texture->AddRef();
		view->AddRef();
		if (view_alt)
		{
			view_alt->AddRef();
		}

		image->texture.shaderView = view;
		image->texture.shaderViewAlternate = view_alt;
		image->texture.map = tex->texture;

		replaced_on_load[image] = tex;
		console::info("[images] %s cambiada al cargarla\n", image->name);
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			db_load_x_assets_hook.create(game::DB_LoadXAssets, db_load_x_assets_stub);
			db_unload_fastfiles_by_zone_flags_hook.create(game::DB_UnloadFastfilesByZoneFlags,
				db_unload_fastfiles_by_zone_flags_stub);

			scheduler::once([]
			{
				std::lock_guard _(mutex);
				scan_folder();
			}, scheduler::main);

			scheduler::loop(frame, scheduler::main);

			// Vuelve a leer la carpeta y los PNG (para probar cambios sin reiniciar)
			command::add("images_reload", reload);

			// Guarda las imágenes cargadas (todas o las que coinciden con el filtro, con * de comodín)
			command::add("images_dump", [](const command::params& params)
			{
				const std::string filter = params.size() > 1 ? params.join(1) : "";
				scheduler::once([filter]
				{
					dump(filter, false);
				}, scheduler::main);
			});

			// Igual, pero todas en DDS con el formato original del juego y sus mips
			command::add("images_dump_dds", [](const command::params& params)
			{
				const std::string filter = params.size() > 1 ? params.join(1) : "";
				scheduler::once([filter]
				{
					dump(filter, true);
				}, scheduler::main);
			});
		}
	};
}

REGISTER_COMPONENT(images::component)
