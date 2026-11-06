// png2dds: convierte PNG al formato DDS de las texturas de IW7 para cargarlas desde iw7s\images (images.cpp)
//
// Uso:
//   png2dds [opciones] <archivo.png | carpeta> ...
//   png2dds              (sin nada, o con doble clic) convierte todos los PNG de iw7s\images junto al ejecutable
//   Arrastrar PNG o carpetas sobre png2dds.exe también vale.
//
// Opciones:
//   -f <formato>   fuerza el formato: bc1, bc3, bc4, bc5, bc7 o rgba
//   -srgb / -linear  fuerza sRGB (color) o lineal (normal maps, máscaras...)
//   -ref <carpeta> carpeta con los DDS originales (images_dump_dds). Por defecto: images_dump junto a la carpeta del
//                  PNG (iw7s\images -> iw7s\images_dump) y iw7s\images_dump junto al ejecutable
//   -o <carpeta>   carpeta de salida. Por defecto, la del PNG
//
// Formato elegido:
//   1. El de -f / -srgb / -linear.
//   2. El del DDS original con el mismo nombre en la carpeta de referencia (images_dump_dds en el juego).
//   3. Si no hay: BC5 lineal si el nombre parece de normal map (_n, _nml, normal), y si no BC7 sRGB.
// Siempre con todos los mips. Los formatos BC necesitan un tamaño múltiplo de 4: si no lo es se guarda en RGBA.

#include <windows.h>
#include <d3d11.h>

#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#pragma warning(push)
#pragma warning(disable: 4459)
#include <DirectXTex.h>
#pragma warning(pop)

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;

namespace
{
	struct options
	{
		std::optional<DXGI_FORMAT> format{};
		std::optional<bool> srgb{};
		std::vector<fs::path> ref_dirs{};
		std::optional<fs::path> out_dir{};
	};

	ID3D11Device* gpu = nullptr;

	std::wstring lower(std::wstring s)
	{
		for (auto& c : s)
		{
			c = static_cast<wchar_t>(towlower(c));
		}

		return s;
	}

	fs::path exe_dir()
	{
		wchar_t buffer[MAX_PATH]{};
		GetModuleFileNameW(nullptr, buffer, MAX_PATH);
		return fs::path(buffer).parent_path();
	}

	std::optional<DXGI_FORMAT> parse_format(const std::wstring& name)
	{
		const auto n = lower(name);
		if (n == L"bc1") return DXGI_FORMAT_BC1_UNORM;
		if (n == L"bc3") return DXGI_FORMAT_BC3_UNORM;
		if (n == L"bc4") return DXGI_FORMAT_BC4_UNORM;
		if (n == L"bc5") return DXGI_FORMAT_BC5_UNORM;
		if (n == L"bc7") return DXGI_FORMAT_BC7_UNORM;
		if (n == L"rgba") return DXGI_FORMAT_R8G8B8A8_UNORM;
		return {};
	}

	bool looks_like_normal_map(const std::wstring& stem)
	{
		const auto n = lower(stem);
		return n.ends_with(L"_n") || n.ends_with(L"_nml") || n.find(L"_n_") != std::wstring::npos ||
			n.find(L"normal") != std::wstring::npos;
	}

	// Formato (con tipo) del DDS original
	std::optional<DXGI_FORMAT> reference_format(const fs::path& png, const options& opt)
	{
		std::vector<fs::path> dirs = opt.ref_dirs;
		if (dirs.empty())
		{
			dirs.push_back(png.parent_path().parent_path() / "images_dump");
			dirs.push_back(exe_dir() / "iw7s" / "images_dump");
		}

		for (const auto& dir : dirs)
		{
			const auto ref = dir / (png.stem().wstring() + L".dds");
			DirectX::TexMetadata meta{};
			if (fs::exists(ref) && SUCCEEDED(DirectX::GetMetadataFromDDSFile(ref.c_str(), DirectX::DDS_FLAGS_NONE, meta)))
			{
				wprintf(L"  referencia: %ls\n", ref.c_str());
				return DirectX::IsTypeless(meta.format) ? DirectX::MakeTypelessUNORM(meta.format) : meta.format;
			}
		}

		return {};
	}

	const wchar_t* format_name(const DXGI_FORMAT f)
	{
		switch (DirectX::MakeLinear(f))
		{
		case DXGI_FORMAT_BC1_UNORM: return L"BC1";
		case DXGI_FORMAT_BC2_UNORM: return L"BC2";
		case DXGI_FORMAT_BC3_UNORM: return L"BC3";
		case DXGI_FORMAT_BC4_UNORM: return L"BC4";
		case DXGI_FORMAT_BC5_UNORM: return L"BC5";
		case DXGI_FORMAT_BC6H_UF16: return L"BC6H";
		case DXGI_FORMAT_BC7_UNORM: return L"BC7";
		case DXGI_FORMAT_R8G8B8A8_UNORM: return L"RGBA8";
		case DXGI_FORMAT_B8G8R8A8_UNORM: return L"BGRA8";
		default: return L"otro";
		}
	}

	bool convert(const fs::path& png, const options& opt)
	{
		wprintf(L"%ls\n", png.c_str());

		DirectX::TexMetadata meta{};
		DirectX::ScratchImage loaded{};
		if (const auto hr = DirectX::LoadFromWICFile(png.c_str(), DirectX::WIC_FLAGS_IGNORE_SRGB, &meta, loaded);
			FAILED(hr))
		{
			wprintf(L"  ERROR: no se puede leer el PNG (0x%08X)\n", static_cast<unsigned>(hr));
			return false;
		}

		// Formato final
		DXGI_FORMAT target{};
		if (opt.format)
		{
			target = *opt.format;
		}
		else if (const auto ref = reference_format(png, opt))
		{
			target = *ref;
		}
		else
		{
			target = looks_like_normal_map(png.stem().wstring()) ? DXGI_FORMAT_BC5_UNORM : DXGI_FORMAT_BC7_UNORM_SRGB;
		}

		if (opt.srgb)
		{
			target = *opt.srgb ? DirectX::MakeSRGB(DirectX::MakeLinear(target)) : DirectX::MakeLinear(target);
		}

		if (DirectX::IsCompressed(target) && (meta.width % 4 != 0 || meta.height % 4 != 0))
		{
			wprintf(L"  aviso: %zux%zu no es múltiplo de 4, se guarda en RGBA sin comprimir\n", meta.width, meta.height);
			target = DirectX::IsSRGB(target) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
		}

		const auto srgb = DirectX::IsSRGB(target);

		// RGBA8 con los mismos bytes del PNG; en sRGB solo se marca, para que los mips se filtren en lineal
		DirectX::ScratchImage rgba{};
		if (const auto hr = DirectX::Convert(*loaded.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
			DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, rgba); FAILED(hr))
		{
			wprintf(L"  ERROR: Convert (0x%08X)\n", static_cast<unsigned>(hr));
			return false;
		}

		if (srgb)
		{
			rgba.OverrideFormat(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
		}

		DirectX::ScratchImage mips{};
		if (const auto hr = DirectX::GenerateMipMaps(*rgba.GetImage(0, 0, 0), DirectX::TEX_FILTER_DEFAULT, 0, mips);
			FAILED(hr))
		{
			wprintf(L"  ERROR: GenerateMipMaps (0x%08X)\n", static_cast<unsigned>(hr));
			return false;
		}

		DirectX::ScratchImage result{};
		const DirectX::ScratchImage* final_image = &mips;

		if (DirectX::IsCompressed(target))
		{
			const auto linear = DirectX::MakeLinear(target);
			const auto use_gpu = gpu && (linear == DXGI_FORMAT_BC7_UNORM || linear == DXGI_FORMAT_BC6H_UF16);

			auto hr = E_FAIL;
			if (use_gpu)
			{
				hr = DirectX::Compress(gpu, mips.GetImages(), mips.GetImageCount(), mips.GetMetadata(), target,
					DirectX::TEX_COMPRESS_DEFAULT, 1.0f, result);
			}

			if (FAILED(hr))
			{
				hr = DirectX::Compress(mips.GetImages(), mips.GetImageCount(), mips.GetMetadata(), target,
					DirectX::TEX_COMPRESS_PARALLEL, DirectX::TEX_THRESHOLD_DEFAULT, result);
			}

			if (FAILED(hr))
			{
				wprintf(L"  ERROR: Compress (0x%08X)\n", static_cast<unsigned>(hr));
				return false;
			}

			final_image = &result;
		}

		const auto out_dir = opt.out_dir ? *opt.out_dir : png.parent_path();
		std::error_code ec{};
		fs::create_directories(out_dir, ec);
		const auto out = out_dir / (png.stem().wstring() + L".dds");

		if (const auto hr = DirectX::SaveToDDSFile(final_image->GetImages(), final_image->GetImageCount(),
			final_image->GetMetadata(), DirectX::DDS_FLAGS_NONE, out.c_str()); FAILED(hr))
		{
			wprintf(L"  ERROR: no se puede guardar %ls (0x%08X)\n", out.c_str(), static_cast<unsigned>(hr));
			return false;
		}

		wprintf(L"  -> %ls (%ls%ls, %zux%zu, %zu mips)\n", out.c_str(), format_name(target), srgb ? L" sRGB" : L"",
			meta.width, meta.height, final_image->GetMetadata().mipLevels);
		return true;
	}

	void collect(const fs::path& path, std::vector<fs::path>& files)
	{
		if (fs::is_directory(path))
		{
			for (const auto& entry : fs::directory_iterator(path))
			{
				if (entry.is_regular_file() && lower(entry.path().extension().wstring()) == L".png")
				{
					files.push_back(entry.path());
				}
			}
		}
		else if (fs::is_regular_file(path))
		{
			files.push_back(path);
		}
		else
		{
			wprintf(L"No existe: %ls\n", path.c_str());
		}
	}
}

int wmain(const int argc, wchar_t** argv)
{
	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
	{
		wprintf(L"ERROR: CoInitializeEx\n");
		return 1;
	}

	// Acentos en la consola
	(void)_setmode(_fileno(stdout), _O_U16TEXT);

	options opt{};
	std::vector<fs::path> inputs{};

	for (auto i = 1; i < argc; ++i)
	{
		const std::wstring arg = argv[i];
		const auto has_value = i + 1 < argc;

		if (arg == L"-f" && has_value)
		{
			opt.format = parse_format(argv[++i]);
			if (!opt.format)
			{
				wprintf(L"Formato no válido: %ls (bc1, bc3, bc4, bc5, bc7, rgba)\n", argv[i]);
				return 1;
			}
		}
		else if (arg == L"-srgb")
		{
			opt.srgb = true;
		}
		else if (arg == L"-linear")
		{
			opt.srgb = false;
		}
		else if (arg == L"-ref" && has_value)
		{
			opt.ref_dirs.emplace_back(argv[++i]);
		}
		else if (arg == L"-o" && has_value)
		{
			opt.out_dir = fs::path(argv[++i]);
		}
		else
		{
			inputs.emplace_back(arg);
		}
	}

	if (inputs.empty())
	{
		inputs.push_back(exe_dir() / "iw7s" / "images");
	}

	std::vector<fs::path> files{};
	for (const auto& input : inputs)
	{
		collect(input, files);
	}

	// GPU para BC7 (mucho más rápido); si no hay, se comprime con la CPU
	D3D_FEATURE_LEVEL level{};
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &gpu,
		&level, nullptr)))
	{
		gpu = nullptr;
	}

	auto ok = 0, failed = 0;
	for (const auto& file : files)
	{
		if (convert(file, opt))
		{
			++ok;
		}
		else
		{
			++failed;
		}
	}

	wprintf(L"\n%d convertidos, %d con error\n", ok, failed);

	if (gpu)
	{
		gpu->Release();
	}

	CoUninitialize();

	// Con doble clic o arrastrando archivos, que no se cierre la ventana sin ver el resultado
	DWORD processes[2]{};
	if (GetConsoleProcessList(processes, 2) <= 1)
	{
		wprintf(L"Pulsa una tecla para salir...");
		(void)getwchar();
	}

	return failed ? 1 : 0;
}
