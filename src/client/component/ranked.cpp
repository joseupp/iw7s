#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "component/command.hpp"
#include "component/console/console.hpp"
#include "component/dvars.hpp"
#include "component/lobby_bots.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>
#include <utils/flags.hpp>
#include <utils/string.hpp>

namespace ranked
{
	namespace
	{
		game::dvar_t* ui_combat_training = nullptr;

		utils::hook::detour game_state_info_get_hook;
		utils::hook::detour playlist_run_rules_hook;
		utils::hook::detour game_state_info_is_public_hook;
		utils::hook::detour party_using_party_based_teams_hook;
		utils::hook::detour party_host_using_assigned_teams_hook;

		bool running_playlist_rules = false;

		// Reglas de la sala pública editadas en el menú de opciones (como en BO3): se mantienen aunque la playlist vuelva
		// a cargar la receta del modo (al volver de una partida, rotación...), hasta que se elige un modo desde la
		// playlist (lobby_rules_reset, ui_scripts/GameSetup/gamesetupoptions.lua) o la playlist cambia de modo.
		constexpr auto public_match_rules = 0x14B039368; // GameStateInfo público + 0x10 (privado: 0x14B038760)
		constexpr auto match_rules_size = 0xC08;
		utils::hook::detour match_rules_set_data_hook;
		std::array<std::uint8_t, match_rules_size> edited_rules{};
		std::string edited_gametype{};
		bool rules_edited = false;
		game::dvar_t* lobby_rules_reset = nullptr;

		constexpr auto max_clients = 18;

		struct bot_settings
		{
			uint8_t connect_type;
			uint8_t difficulty[2];
			uint8_t team_limit[2];
		};

		std::optional<bot_settings> saved_bot_settings{};
		std::atomic_bool has_saved_bot_settings = false;
		std::mutex saved_bot_settings_mutex;

		bool in_combat_training()
		{
			return ui_combat_training && ui_combat_training->current.enabled
				&& game::Com_GameMode_GetActiveGameMode() == game::GAME_MODE_MP;
		}

		bool use_private_game_state()
		{
			return !running_playlist_rules && in_combat_training();
		}

		game::GameStateInfo* get_private_game_state()
		{
			return utils::hook::invoke<game::GameStateInfo*>(0x140E7AEB0, 0, 0); // GameStateInfo_GetInstance(type, no reset?) 0 is private match
		}

		void update_combat_training_state()
		{
			const auto active = in_combat_training();
			if (active == has_saved_bot_settings)
			{
				return;
			}

			std::lock_guard _(saved_bot_settings_mutex);
			if (active == saved_bot_settings.has_value())
			{
				return;
			}

			auto* state = get_private_game_state();
			if (!state)
			{
				return;
			}

			if (active)
			{
				saved_bot_settings = bot_settings{
					state->usingBotsConnectType,
					{state->usingBotsDifficulty[0], state->usingBotsDifficulty[1]},
					{state->usingBotsTeamLimit[0], state->usingBotsTeamLimit[1]},
				};
				has_saved_bot_settings = true;
				return;
			}

			state->usingBotsConnectType = saved_bot_settings->connect_type;
			state->usingBotsDifficulty[0] = saved_bot_settings->difficulty[0];
			state->usingBotsDifficulty[1] = saved_bot_settings->difficulty[1];
			state->usingBotsTeamLimit[0] = saved_bot_settings->team_limit[0];
			state->usingBotsTeamLimit[1] = saved_bot_settings->team_limit[1];
			saved_bot_settings.reset();
			has_saved_bot_settings = false;
		}

		void* game_state_info_get_stub()
		{
			update_combat_training_state();

			if (use_private_game_state())
			{
				return get_private_game_state();
			}
			return game_state_info_get_hook.invoke<void*>();
		}

		int game_state_info_is_public_stub()
		{
			update_combat_training_state();

			if (use_private_game_state())
			{
				return 0;
			}
			return game_state_info_is_public_hook.invoke<int>();
		}

		int get_dvar_int(const char* name)
		{
			const auto* dvar = game::Dvar_FindVar(name);
			return dvar ? dvar->current.integer : 0;
		}

		bool is_team_based_gametype()
		{
			const auto* gametype = game::Dvar_FindVar("ui_gametype");
			const auto* table = game::DB_FindXAssetHeader(game::ASSET_TYPE_STRINGTABLE, "mp/gametypestable.csv", false).stringTable;
			if (!gametype || !table)
			{
				return true;
			}

			const auto* value = game::StringTable_Lookup(table, 0, gametype->current.string, 4);
			return !value || *value != '0';
		}

		void sync_combat_training_bots()
		{
			update_combat_training_state();

			if (!in_combat_training())
			{
				return;
			}

			const auto max_bots = max_clients - 1;
			const auto team_based = is_team_based_gametype();

			const auto allies = std::clamp(get_dvar_int(team_based ? "bot_allies" : "bot_free"), 0, max_bots);
			const auto enemies = team_based ? std::clamp(get_dvar_int("bot_enemies"), 0, max_bots - allies) : 0;

			const auto allies_difficulty = std::clamp(get_dvar_int(team_based ? "bot_difficulty_allies" : "bot_difficulty_free"), 0, 4);
			const auto enemies_difficulty = team_based ? std::clamp(get_dvar_int("bot_difficulty_enemies"), 0, 4) : allies_difficulty;

			game::BG_SetBotsConnectType((allies + enemies) > 0 ? 1 : 0);
			game::BG_SetBotsTeamLimit(0, allies);
			game::BG_SetBotsTeamLimit(1, enemies);
			game::BG_SetBotsDifficulty(0, allies_difficulty);
			game::BG_SetBotsDifficulty(1, enemies_difficulty);

			auto* party = game::Lobby_GetPartyData();
			if (party && game::Party_AreWeHost(party))
			{
				game::PartyHost_GamestateChanged(party);
			}
		}

		int party_using_party_based_teams_stub(void* party)
		{
			// Sala pública con bots (lobby_bots): como en la versión anterior, cada bot entra en el equipo de la sala
			// (bots::spawn_bots con equipo); forzado a 1 el juego los reparte a partes iguales
			if (game::Com_GameMode_GetActiveGameMode() == game::GAME_MODE_MP && !lobby_bots::has_bots())
			{
				return 1;
			}
			return party_using_party_based_teams_hook.invoke<int>(party);
		}

		int party_host_using_assigned_teams_stub(void* party)
		{
			if (game::Com_GameMode_GetActiveGameMode() == game::GAME_MODE_MP && !lobby_bots::has_bots())
			{
				return 1;
			}
			return party_host_using_assigned_teams_hook.invoke<int>(party);
		}

		int party_get_num_game_slots_stub(game::SessionData* session)
		{
			if (in_combat_training())
			{
				return max_clients;
			}
			return utils::hook::invoke<int>(0x140C72500, session); // Party_GetNumGameSlots
		}

		std::string get_ui_gametype()
		{
			const auto* gametype = game::Dvar_FindVar("ui_gametype");
			return gametype && gametype->current.string ? gametype->current.string : "";
		}

		bool using_public_rules()
		{
			if (game::Com_GameMode_GetActiveGameMode() != game::GAME_MODE_MP || in_combat_training())
			{
				return false;
			}

			const auto* state = static_cast<const std::uint8_t*>(game_state_info_get_hook.invoke<void*>());
			return state && *reinterpret_cast<const std::uint64_t*>(state + 0x10) == public_match_rules;
		}

		// MatchRules.SetData (Lua): después de cada cambio del menú se guardan las reglas de la sala pública
		int match_rules_set_data_stub(void* state)
		{
			const auto result = match_rules_set_data_hook.invoke<int>(state);
			if (game::Com_FrontEnd_IsInFrontEnd() && using_public_rules())
			{
				std::memcpy(edited_rules.data(), reinterpret_cast<void*>(public_match_rules), match_rules_size);
				edited_gametype = get_ui_gametype();
				rules_edited = true;

				// Cambios hechos después de elegir modo: la playlist ya se ha aplicado
				if (lobby_rules_reset && lobby_rules_reset->current.enabled)
				{
					game::Dvar_SetBool(lobby_rules_reset, false);
				}
			}
			return result;
		}

		void restore_edited_rules()
		{
			if (lobby_rules_reset && lobby_rules_reset->current.enabled)
			{
				game::Dvar_SetBool(lobby_rules_reset, false);
				rules_edited = false;
			}

			if (!rules_edited)
			{
				return;
			}

			// Con el mismo modo se recuperan los cambios; si la playlist cambia de modo, se descartan
			if (using_public_rules() && get_ui_gametype() == edited_gametype)
			{
				std::memcpy(reinterpret_cast<void*>(public_match_rules), edited_rules.data(), match_rules_size);
				return;
			}

			rules_edited = false;
		}

		void playlist_run_rules_stub(const int controller, const int rule)
		{
			running_playlist_rules = true;
			playlist_run_rules_hook.invoke<void>(controller, rule);
			running_playlist_rules = false;

			restore_edited_rules();

			sync_combat_training_bots();
		}
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (game::environment::is_dedi())
			{
				dvars::override::register_bool("xblive_privatematch", false, game::DVAR_FLAG_REPLICATED | game::DVAR_FLAG_WRITE);
				dvars::override::register_bool("onlinegame", true, game::DVAR_FLAG_READ);

				// Fix sessionteam always returning none (SV_ClientMP_HasAssignedTeam_Internal)
				utils::hook::set(0x140C50BC0, 0xC300B0);
			}
			else
			{
				dvars::override::register_bool("xblive_privatematch", true, game::DVAR_FLAG_REPLICATED);

				ui_combat_training = game::Dvar_RegisterBool("ui_combat_training", false, game::DVAR_FLAG_NONE,
					"Hosting a combat training match, use the private match game rules");
				game_state_info_get_hook.create(0x140E7AD70, game_state_info_get_stub);
				game_state_info_is_public_hook.create(0x140E7ADC0, game_state_info_is_public_stub);
				playlist_run_rules_hook.create(0x140CCD840, playlist_run_rules_stub);

				// override Party_GetNumGameSlots to allow 18 players in combat training
				utils::hook::call(0x1409D9868, party_get_num_game_slots_stub);

				// enables bots in public match
				utils::hook::nop(0x140036FFB, 0xD); // BG_Bots_IsBotMatchMakingAllowedForPlaylist

				// allow up to 18 players in any mode
				utils::hook::nop(0x140C3E518, 53);	// SV_BotTeamLimit
				utils::hook::nop(0x140C3E576, 0xC); // ^ remove 9 player per team check

				// fix team assignment that lead to gsc problems with invalid sessionteam
				party_using_party_based_teams_hook.create(0x14037E030, party_using_party_based_teams_stub); // Party_UsingPartyBasedTeams
				party_host_using_assigned_teams_hook.create(0x1409DB330, party_host_using_assigned_teams_stub); // PartyHost_UsingAssignedTeams

				utils::hook::nop(0x140E7ADC4, 26); // allow saving recipes in onlinegame

				// MatchRules.SetData (0x1406B6100) sale sin hacer nada en el estado público (GameStateInfo_IsPublic): así
				// el menú de opciones de la sala pública edita las reglas de la receta del modo de la playlist, como en BO3
				static constexpr std::uint8_t xor_eax_eax[] = {0x33, 0xC0, 0x90, 0x90, 0x90};
				utils::hook::copy(0x1406B6115, xor_eax_eax, sizeof(xor_eax_eax));
				match_rules_set_data_hook.create(0x1406B6100, match_rules_set_data_stub);

				lobby_rules_reset = game::Dvar_RegisterBool("lobby_rules_reset", false, game::DVAR_FLAG_NONE,
					"Set by the playlist menu: discard the public lobby rules changed in the options menu");

				command::add("xsyncbots", sync_combat_training_bots);
			}
		}
	};
}

REGISTER_COMPONENT(ranked::component)
