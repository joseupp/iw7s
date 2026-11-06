#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "steam_proxy.hpp"

#include "game/game.hpp"

#include "scheduler.hpp"

#include <utils/nt.hpp>
#include <utils/flags.hpp>
#include <utils/string.hpp>
#include <utils/binary_resource.hpp>
#include <utils/cryptography.hpp>
#include <utils/io.hpp>
#include <utils/smbios.hpp>

#include "console/console.hpp"
#include "steam/interface.hpp"
#include "steam/steam.hpp"

namespace steam_proxy
{
	namespace
	{
		utils::nt::library steam_client_module_{};
#ifdef LOAD_STEAM_OVERLAY
		utils::nt::library steam_overlay_module_{};
#endif

		steam::interface client_engine_ {};
		steam::interface client_user_ {};
		steam::interface client_utils_ {};

		steam::HSteamPipe steam_pipe_ = 0;
		steam::HSteamUser global_user_ = 0;

		steam::client* steam_client_ {};

		enum class ownership_state
		{
			success,
			unowned,
			nosteam,
			error,
		};

		bool is_disabled()
		{
			static const auto disabled = utils::flags::has_flag("nosteam"); // runs without Steam
			return disabled;
		}

		void* load_client_engine()
		{
			if (!steam_client_module_) return nullptr;

			for (auto i = 1; i > 0; ++i)
			{
				std::string name = utils::string::va("CLIENTENGINE_INTERFACE_VERSION%03i", i);
				auto* const temp_client_engine = steam_client_module_
					.invoke<void*>("CreateInterface", name.data(), nullptr);
				if (temp_client_engine) return temp_client_engine;
			}

			return nullptr;
		}

		void load_client()
		{
			const auto app_id = std::to_string(steam::SteamUtils()->GetAppID());
			SetEnvironmentVariableA("SteamAppId", app_id.data());
			SetEnvironmentVariableA("SteamGameId", app_id.data());
			SetEnvironmentVariableA("SteamOverlayGameId", app_id.data());

			const std::filesystem::path steam_path = steam::SteamAPI_GetSteamInstallPath();
			if (steam_path.empty()) return;

			utils::nt::library::load(steam_path / "tier0_s64.dll");
			utils::nt::library::load(steam_path / "vstdlib_s64.dll");
#ifdef LOAD_STEAM_OVERLAY
			steam_overlay_module_ = utils::nt::library::load(steam_path / "gameoverlayrenderer64.dll");
#endif
			steam_client_module_ = utils::nt::library::load(steam_path / "steamclient64.dll");
			if (!steam_client_module_) return;

			client_engine_ = load_client_engine();
			if (!client_engine_) return;

			steam_pipe_ = steam_client_module_.invoke<steam::HSteamPipe>("Steam_CreateSteamPipe");
			global_user_ = steam_client_module_.invoke<steam::HSteamUser>(
				"Steam_ConnectToGlobalUser", steam_pipe_);
			client_user_ = client_engine_.invoke<void*>(8, steam_pipe_, global_user_); // GetIClientUser
			client_utils_ = client_engine_.invoke<void*>(14, steam_pipe_); // GetIClientUtils
		}

		void do_cleanup()
		{
			client_engine_ = nullptr;
			client_user_ = nullptr;
			client_utils_ = nullptr;

			steam_pipe_ = 0;
			global_user_ = 0;

			steam_client_ = nullptr;
		}

		bool perform_cleanup_if_needed()
		{
			if (steam_client_) return true;

			if (steam_client_module_
				&& steam_pipe_
				&& global_user_
				&& steam_client_module_.invoke<bool>("Steam_BConnected", global_user_, steam_pipe_)
				&& steam_client_module_.invoke<bool>("Steam_BLoggedOn", global_user_, steam_pipe_)
				)
			{
				return false;
			}

			do_cleanup();
			return true;
		}

		void clean_up_on_error()
		{	
			scheduler::schedule([]()
			{
				if (perform_cleanup_if_needed())
				{
					return scheduler::cond_end;
				}

				return scheduler::cond_continue;
			}, scheduler::main);
		}

		// Licencia comprobada: con Steam abierto se verifica la compra (BIsSubscribedApp) y se guarda una licencia
		// ligada a este equipo; sin Steam abierto el juego solo arranca si ya se verifico antes en este equipo
		// (como el modo sin conexion de Steam).
		const char* license_path = "iw7s/players2/license.dat";

		std::string make_license(const size_t app_id)
		{
			return utils::cryptography::sha256::compute(
				utils::string::va("iw7s-license|%zu|%s", app_id, utils::smbios::get_uuid().data()), true);
		}

		bool has_saved_license(const size_t app_id)
		{
			std::string data{};
			return utils::io::read_file(license_path, &data) && data == make_license(app_id);
		}

		void save_license(const size_t app_id)
		{
			utils::io::write_file(license_path, make_license(app_id), false);
		}

		ownership_state start_mod_unsafe(const std::string& title, size_t app_id)
		{
			if (!client_utils_ || !client_user_)
			{
				// Steam cerrado o sin sesion iniciada
				return has_saved_license(app_id) ? ownership_state::success : ownership_state::nosteam;
			}

			if (!client_user_.invoke<bool>("BIsSubscribedApp", app_id))
			{
				std::remove(license_path);
				return ownership_state::unowned;
			}

			save_license(app_id);
			client_utils_.invoke<void>("SetAppIDForCurrentPipe", app_id, false);

			return ownership_state::success;
		}

		ownership_state start_mod(const std::string& title, const size_t app_id)
		{
			try
			{
				return start_mod_unsafe(title, app_id);
			}
			catch (const std::exception& e)
			{
				do_cleanup();
				printf("Steam: %s\n", e.what());
				MessageBoxA(GetForegroundWindow(), e.what(), "Error", MB_ICONERROR);
				TerminateProcess(GetCurrentProcess(), 1234);
			}
			return ownership_state::success;
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			if (game::environment::is_dedi() || is_disabled() || utils::nt::is_wine() || !is_steam_running())
			{
				return;
			}

			load_client();
			perform_cleanup_if_needed();
		}

		void post_unpack() override
		{
			if (game::environment::is_dedi() || utils::nt::is_wine())
			{
				return;
			}

			try
			{
				switch (const auto res = start_mod("\xF0\x9F\x8C\xA0" "IW7S", steam::SteamUtils()->GetAppID()))
				{
				case ownership_state::nosteam:
					throw std::runtime_error("Steam must be running the first time to verify that you own the game!");
				case ownership_state::unowned:
					throw std::runtime_error("You must own the game on Steam to play this mod!");
				case ownership_state::error:
					throw std::runtime_error("Failed to verify ownership of the game, please try again!");
				case ownership_state::success:
					break;
				}
			}
			catch (const std::exception& e)
			{
				do_cleanup();
				console::debug("Steam: %s\n", e.what());
				MessageBoxA(GetForegroundWindow(), e.what(), "Error", MB_ICONERROR);
				TerminateProcess(GetCurrentProcess(), 1234);
			}
			
			clean_up_on_error();
		}

		void pre_destroy() override
		{
			if (steam_client_)
			{
				if (global_user_)
				{
					steam_client_->ReleaseUser(steam_pipe_, global_user_);
				}

				steam_client_->BReleaseSteamPipe(steam_pipe_);
			}
			else if (steam_client_module_ && steam_pipe_)
			{
				if (global_user_)
				{
					steam_client_module_.invoke<void>("Steam_ReleaseUser", steam_pipe_,
						global_user_);
				}

				steam_client_module_.invoke<bool>("Steam_BReleaseSteamPipe", steam_pipe_);
			}
		}

		component_priority priority() override
		{
			return component_priority::steam_proxy;
		}
	};

#ifdef LOAD_STEAM_OVERLAY
	const utils::nt::library& get_overlay_module()
	{
		return steam_overlay_module_;
	}
#endif

	// Steam abierto: HKCU\Software\Valve\Steam\ActiveProcess\pid apunta a un proceso vivo.
	// Sin Steam abierto no se carga steamclient64.dll (podria abrir Steam) y el juego funciona igual.
	bool is_steam_running()
	{
		static const auto running = []()
		{
			HKEY key{};
			if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
			{
				return false;
			}

			DWORD pid{};
			DWORD size = sizeof(pid);
			const auto result = RegQueryValueExA(key, "pid", nullptr, nullptr, reinterpret_cast<BYTE*>(&pid), &size);
			RegCloseKey(key);
			if (result != ERROR_SUCCESS || !pid)
			{
				return false;
			}

			const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
			if (!process)
			{
				return false;
			}

			DWORD exit_code{};
			const auto alive = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
			CloseHandle(process);
			return alive;
		}();

		return running;
	}

	void initialize()
	{
		if (client_engine_ || !steam_client_module_) return;

		steam_client_ = steam_client_module_.invoke<steam::client*>("CreateInterface", "SteamClient017", nullptr);
		if (!steam_client_) return;

		steam_pipe_ = steam_client_->CreateSteamPipe();
		global_user_ = steam_client_->ConnectToGlobalUser(steam_pipe_);
	}
}

REGISTER_COMPONENT(steam_proxy::component)