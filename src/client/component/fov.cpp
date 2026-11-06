#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include "dvars.hpp"
#include "fov.hpp"

#include <utils/hook.hpp>

#include <algorithm>
#include <atomic>

namespace fov
{
	constexpr float max_fov = 160.0f;

	namespace
	{
		game::dvar_t* cg_fov;
		game::dvar_t* cg_fovScale;
		game::dvar_t* cg_use_fov_comp;

		// Zoom afectado por el FOV (cg_fovAdsAffected), como en s1-mod / boiii:
		//   CG_GetViewFov (0x14087CCB0): cadera = clamp(cg_fov * com_fovUserScale) (0x140898C10), apuntado = mezcla con
		//   el FOV del arma (0x1408979A0) y el resultado * cg_fovScale (0x14087DFA0 / 0x14087DFC6), todo en grados.
		//   Con la opción se calcula como con FOV 65 y cg_fovScale * FOV elegido / 65: sin apuntar se ve el FOV
		//   elegido y al apuntar el zoom es el de FOV 65 escalado igual.
		game::dvar_t* cg_fov_ads_affected = nullptr;
		constexpr float base_fov = 65.0f;
		constexpr auto com_fov_user_scale_ptr = 0x146005570;

		std::atomic<float> effective_fov_scale{1.0f};
		bool in_get_view_fov = false;

		// Círculo de ganadores (scripts\mp\broshot.gsc): la cámara va pegada al jugador (cameralinkto) y el juego usa
		// el FOV del jugador con todas sus escalas (com_fovUserScale, cg_fovScale): los personajes se veían alejados.
		// custom_scripts/mp/broshot_fov.gsc pone cg_broshotActive mientras dura (level.broshotrunning) y aquí se usa
		// FOV 65 sin escalas, como en consola.
		game::dvar_t* cg_broshot_active = nullptr;

		utils::hook::detour cg_get_view_fov_hook;
		float cg_get_view_fov_unscaled(const int local_client_num, const int a2)
		{
			const auto user_fov = cg_fov->current.value;
			const auto user_scale = cg_fovScale->current.value;
			auto* user_fov_scale = *reinterpret_cast<game::dvar_t**>(com_fov_user_scale_ptr);
			const auto user_menu_scale = user_fov_scale ? user_fov_scale->current.value : 1.0f;

			cg_fov->current.value = base_fov;
			cg_fovScale->current.value = 1.0f;
			if (user_fov_scale) user_fov_scale->current.value = 1.0f;

			in_get_view_fov = true;
			const auto result = cg_get_view_fov_hook.invoke<float>(local_client_num, a2);
			in_get_view_fov = false;

			cg_fov->current.value = user_fov;
			cg_fovScale->current.value = user_scale;
			if (user_fov_scale) user_fov_scale->current.value = user_menu_scale;
			return result;
		}

		float cg_get_view_fov_player(const int local_client_num, const int a2);

		float cg_get_view_fov_stub(const int local_client_num, const int a2)
		{
			if (in_get_view_fov || a2)
			{
				return cg_get_view_fov_player(local_client_num, a2);
			}

			if (cg_broshot_active && cg_broshot_active->current.enabled)
			{
				if (game::Com_FrontEnd_IsInFrontEnd())
				{
					game::Dvar_SetBool(cg_broshot_active, false); // por si la partida acaba sin que el script lo quite
				}
				else
				{
					effective_fov_scale = 1.0f;
					return cg_get_view_fov_unscaled(local_client_num, a2);
				}
			}

			return cg_get_view_fov_player(local_client_num, a2);
		}

		float cg_get_view_fov_player(const int local_client_num, const int a2)
		{
			const auto user_scale = cg_fovScale->current.value;
			auto* user_fov_scale = *reinterpret_cast<game::dvar_t**>(com_fov_user_scale_ptr);

			// a2 != 0: FOV del arma con mira dual (francotiradores): cg_dualViewScopeFOV (0x1408979A0) * com_fovUserScale
			// * cg_fovScale (0x14087DF7D). CG_CalcFov lo usa para las tangentes del arma: se calcula como con FOV 65
			// y sin escalas, para que el arma quede como en el juego original.
			if (a2 && !in_get_view_fov)
			{
				return cg_get_view_fov_unscaled(local_client_num, a2);
			}

			if (in_get_view_fov || !cg_fov_ads_affected || !cg_fov_ads_affected->current.enabled || !user_fov_scale)
			{
				if (!in_get_view_fov)
				{
					effective_fov_scale = user_scale;
				}
				return cg_get_view_fov_hook.invoke<float>(local_client_num, a2);
			}

			const auto user_fov = cg_fov->current.value;
			const auto user_menu_scale = user_fov_scale->current.value;
			const auto hip_fov = user_fov * std::clamp(user_menu_scale, 1.0f / base_fov, 1.6923077f); // límites de 0x140898C10
			const auto scale = user_scale * hip_fov / base_fov;

			cg_fov->current.value = base_fov;
			user_fov_scale->current.value = 1.0f;
			cg_fovScale->current.value = scale;

			in_get_view_fov = true;
			const auto result = cg_get_view_fov_hook.invoke<float>(local_client_num, a2);
			in_get_view_fov = false;

			cg_fov->current.value = user_fov;
			user_fov_scale->current.value = user_menu_scale;
			cg_fovScale->current.value = user_scale;

			effective_fov_scale = scale;
			return result;
		}

		utils::hook::detour cg_view_calc_fov_compensation_hook;
		float cg_view_calc_fov_compensation_stub(game::cg_s* blob)
		{
			if (!cg_use_fov_comp->current.enabled)
			{
				return 0.0f;
			}

			return cg_view_calc_fov_compensation_hook.invoke<float>(blob);
		}
	}

	float get_world_fov_scale()
	{
		return effective_fov_scale;
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (game::environment::is_dedi())
			{
				return;
			}
			
			cg_fov = game::Dvar_RegisterFloat("cg_fov", 65.f, 1.f, 160.f, game::DVAR_FLAG_SAVED,
				"The field of view angle in degrees");
			cg_fovScale = game::Dvar_RegisterFloat("cg_fovScale", 1.f, 0.1f, 2.f, game::DVAR_FLAG_SAVED,
				"Scale applied to the field of view");
			dvars::override::register_float("cg_fovMin", 1.f, 1.f, 160.f, game::DVAR_FLAG_SAVED);

			// FOV del arma con mira dual (francotiradores): 63 por defecto deja la mira pequeña y alejada; 40.5 coincide con el juego original (vídeo de referencia)
			dvars::override::register_float("cg_dualViewScopeFOV", 40.5f, 15.f, 120.f, game::DVAR_FLAG_SAVED);

			*reinterpret_cast<game::dvar_t**>(0x141FA6DA0) = cg_fov;
			dvars::disable::re_register("cg_fov");
			dvars::disable::de_register("cg_fov");

			*reinterpret_cast<game::dvar_t**>(0x141FA6DB0) = cg_fovScale;
			dvars::disable::re_register("cg_fovScale");
			dvars::disable::de_register("cg_fovScale");

			// don't reset our fov
			utils::hook::set<uint8_t>(0x1408A6160, 0xC3);

			// don't set cg_fov value
			utils::hook::nop(0x1408B5659, 5);
			utils::hook::set<uint8_t>(0x1408B5659, 0xC3);
			
			// ^
			utils::hook::set<uint8_t>(0x1408B674A, 0xEB);

			// disable FOV compensation by default
			cg_use_fov_comp = game::Dvar_RegisterBool("cg_use_fov_comp", false, game::DVAR_FLAG_SAVED, "Use FOV offset compensation for the viewmodel");
			cg_view_calc_fov_compensation_hook.create(0x140889B60, cg_view_calc_fov_compensation_stub);

			cg_fov_ads_affected = game::Dvar_RegisterBool("cg_fovAdsAffected", true, game::DVAR_FLAG_SAVED,
				"ADS zoom scales with the FOV (FOV 65 scaled up to the chosen FOV)");
			cg_get_view_fov_hook.create(0x14087CCB0, cg_get_view_fov_stub);

			cg_broshot_active = game::Dvar_RegisterBool("cg_broshotActive", false, game::DVAR_FLAG_NONE,
				"Set by custom_scripts/mp/broshot_fov.gsc while the winners circle is shown (FOV 65 without scales)");

			// patch max fov values
			utils::hook::inject(0x14087E08B + 4, &max_fov);
			utils::hook::inject(0x140898C4B + 4, &max_fov); // mp max
			utils::hook::inject(0x140898C55 + 4, &max_fov); // sp max
		}
	};
}

REGISTER_COMPONENT(fov::component)