#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "command.hpp"
#include "console/console.hpp"
#include "scripting.hpp"
#include "scheduler.hpp"
#include "lobby_bots.hpp"
#include "gsc/script_extension.hpp"

#include "game/game.hpp"
#include "game/scripting/execution.hpp"

#include <utils/hook.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>

#include <random>

// Bots que entran en la sala pública como si fueran jugadores.
//
// Se añaden como miembros del party de la sala (Lobby_GetPartyData, 0x14524C4A0). El juego los cuenta como al
// resto: salen en la lista, cuentan para party_minplayers y entran en el reparto de equipos.
// Al empezar la partida se crea un bot por miembro (scripts/mp/bots/bots::spawn_bots) con su nombre (bots.cpp,
// SV_BotGetRandomName -> take_bot_name).
//
// Party (sala):
//   +0x128 + i * 0x1F8: miembros (18)        +0x2D08: somos el host   +0x2D10: en el party   +0x2D14: activo
// Miembro (0x1F8):
//   +0x000 uint8 estado (>= 5 dentro de la sala, 0x1409CDF70)
//   +0x004 ClientAuthoritativeMemberInfo: +0x004 id del party privado, +0x010 rango, +0x014 prestigio,
//          +0x024 especialista (mp/battleRigTable.csv), +0x04C nombre (32), +0x06C clan (5), +0x074 equipo elegido,
//          +0x09C cabeza (mp/cac/heads.csv), +0x0A0 cuerpo (mp/cac/bodies.csv)   (datos de la lista, 0x1400eedc0)
//   +0x134 hora del último paquete (Sys_Milliseconds)
//   +0x184 equipo de la sala (1 / 2)          +0x1A0 xuid (Lobby.GetActivePartyMemberXUID, 0x1406A1C60)

namespace lobby_bots
{
	namespace
	{
		constexpr auto max_members = 18;
		constexpr auto member_size = 0x1F8;
		constexpr auto members_offset = 0x128;

		namespace member
		{
			constexpr auto status = 0x0;
			constexpr auto party_id = 0x4;
			constexpr auto rank = 0x10;
			constexpr auto prestige = 0x14;
			constexpr auto rig = 0x24;
			constexpr auto head = 0x9C;
			constexpr auto body = 0xA0;
			constexpr auto name = 0x4C;
			constexpr auto name_size = 32;
			constexpr auto clan_tag = 0x6C;
			constexpr auto team_selection = 0x74;
			constexpr auto last_packet = 0x134;
			constexpr auto lobby_team = 0x184;
			constexpr auto xuid = 0x1A0;
		}

		constexpr std::uint8_t status_in_lobby = 5;

		constexpr auto rig_count = 6;

		// mp/cac/heads.csv (desbloqueos de rango/desafío)
		const std::vector<int> rig_heads[] = {
			{2, 3, 4, 5, 8, 9, 37, 118, 296, 302, 303, 304, 305, 326}, // archetype_assault
			{12, 13, 14, 15, 16, 18, 103, 106, 295, 306, 307, 308, 309, 327}, // archetype_heavy
			{28, 30, 80, 82, 83, 84, 86, 110, 300, 322, 323, 324, 325, 328}, // archetype_scout
			{22, 24, 53, 57, 58, 60, 88, 89, 298, 310, 311, 312, 313, 329}, // archetype_assassin
			{11, 38, 39, 45, 46, 48, 50, 108, 299, 314, 315, 316, 317, 330}, // archetype_engineer
			{25, 26, 27, 61, 70, 76, 96, 98, 297, 318, 319, 320, 321, 331}, // archetype_sniper
		};

		// mp/cac/bodies.csv (desbloqueos de rango/desafío)
		const std::vector<int> rig_bodies[] = {
			{16, 17, 18, 19, 20, 22, 23, 64, 226, 232, 233, 234, 235, 256}, // archetype_assault
			{4, 5, 6, 7, 8, 9, 11, 13, 225, 236, 237, 238, 239, 257}, // archetype_heavy
			{1, 3, 60, 61, 62, 63, 71, 73, 230, 252, 253, 254, 255, 258}, // archetype_scout
			{26, 27, 28, 47, 51, 52, 53, 54, 228, 240, 241, 242, 243, 259}, // archetype_assassin
			{25, 36, 37, 39, 40, 42, 44, 69, 229, 244, 245, 246, 247, 260}, // archetype_engineer
			{29, 30, 31, 48, 49, 65, 66, 67, 227, 248, 249, 250, 251, 261}, // archetype_sniper
		};


		game::dvar_t* lobby_bots_enabled = nullptr;
		game::dvar_t* lobby_bots_join_min = nullptr;
		game::dvar_t* lobby_bots_join_max = nullptr;
		game::dvar_t* lobby_bots_first_min = nullptr;
		game::dvar_t* lobby_bots_first_max = nullptr;
		game::dvar_t* lobby_bots_leave_chance = nullptr;
		game::dvar_t* lobby_bots_public_root = nullptr;
		game::dvar_t* lobby_bots_team_choice = nullptr;    // equipo elegido con L2 / R2 en la sala pública: 0 auto, 1 SDF, 2 UNSA
		game::dvar_t* lobby_bots_team_map = nullptr;       // equipos de la sala para la lista (publish_team_map)
		game::dvar_t* lobby_bots_new_lobby = nullptr;      // ui_scripts/GameSetup/gamesetupoptions.lua: modo elegido en la playlist
		game::dvar_t* lobby_bots_lobby_menu = nullptr;     // ui_scripts/public_lobby: se ha abierto la sala (LobbyMission)
		game::dvar_t* lobby_bots_members = nullptr;        // para ui_scripts/public_lobby: jugadores en la sala pública (0 fuera)
		game::dvar_t* lobby_bots_game_start_requested = nullptr; // LobbyMissionVerticalLayout: "Iniciar partida" pulsado (botón bloqueado)

		// Menú de bots de la sala (ui_scripts/GameSetup/gamesetupbots.lua). -1 = automático (como hasta ahora: la sala se
		// llena hasta party_maxplayers y la playlist pone bot_autoconnectdefault / bot_difficultydefault)
		game::dvar_t* lobby_bots_allies = nullptr;         // bots en el equipo del jugador
		game::dvar_t* lobby_bots_enemies = nullptr;        // bots en el equipo contrario (todos en partidas sin equipos)
		game::dvar_t* lobby_bots_difficulty_allies = nullptr;   // 0 recluta .. 3 veterano (custom_scripts/mp/lobby_bots.gsc)
		game::dvar_t* lobby_bots_difficulty_enemies = nullptr;

		std::recursive_mutex mutex;
		std::vector<std::uint64_t> fake_xuids{};
		std::vector<std::string> pending_names{};   // nombres para los bots de la partida, en orden
		std::vector<std::string> name_list{};
		bool default_names = false;                 // name_list son los de Infinity Ward (clan IW)

		// Apellidos de gente de Infinity Ward en los créditos de Infinite Warfare (el juego original también los usa
		// para sus bots). Máximo 16 caracteres.
		const std::vector<std::string> iw_names = {
			"xLebessisx", "Eurek_88", "Monacelli-M", "WellmanW_7", "BinderBlitz",
			"Carpenter_92", "Schwartz-21", "ii-Cohen-ii", "Ozzy_Marvin", "Surmenian_14",
			"BruceTheBrute", "Rodriguez_09", "xGreenbergx", "Bansi_Blaze", "ChangGang_23",
			"OHara_77", "Negus-N", "CarlyleCam", "Hernandez_HH", "ChenChen_99",
			"SkillmanX", "Nichols-12", "Buchanan_6", "StaplesSnipe", "Dicdican_08",
			"HaileStorm", "Valenton_V", "xValentinx", "Burrichter_44", "TTV-Su",
			"Sutherland_S", "Montoya_13", "BoonDoggle", "Hagar-Hammer", "Benavente_91",
			"SnyderNoScope", "Dunford_D", "KerbyKills", "Fonzy_Elenes", "MrSelf_7"
		};
		int next_join_time = 0;
		bool first_wave_done = false;               // tanda inicial de bots al abrir la sala

		// Bots de la última partida: al volver a la sala siguen (salvo los que se van, lobby_bots_leave_chance)
		struct saved_bot
		{
			std::string name;
			int rank;
			int prestige;
			int rig;
			int head;
			int body;
			int difficulty;     // 0 recluta, 1 regular, 2 curtido, 3 veterano
		};

		std::vector<saved_bot> match_identities{};  // bots de la partida en curso (rango para lobby_bots.gsc)
		std::map<std::string, int> match_teams{};   // equipo de la sala de cada bot de la partida (1 axis, 2 allies)
		std::map<std::uint64_t, int> bot_difficulties{};   // dificultad de cada bot de la sala (por xuid)

		std::vector<saved_bot> saved_roster{};
		bool restore_pending = false;
		bool came_from_match = false;
		std::atomic_bool game_start_requested{false};    // "Iniciar partida" pulsado: la sala pública puede empezar la cuenta atrás
		std::map<std::string, std::string> saved_dvars{};     // valores de la playlist antes del modo manual
		std::atomic_bool has_lobby_bots{false};     // hay bots de la sala (para ranked.cpp)
		int applied_allies = -2;                    // cantidades del menú ya aplicadas a la sala (para repartir de nuevo)
		int applied_enemies = -2;

		int next_host_team = 0;                     // equipo del jugador en la próxima sala (se alterna)
		bool bots_spawned = false;                  // ya se han creado los bots de esta partida
		int match_seen_time = 0;

		std::mt19937& rng()
		{
			static std::mt19937 engine{std::random_device{}()};
			return engine;
		}

		int random_int(const int min, const int max)
		{
			std::uniform_int_distribution<int> distribution(min, std::max(min, max));
			return distribution(rng());
		}

		std::uint8_t* get_party()
		{
			return reinterpret_cast<std::uint8_t*>(game::Lobby_GetPartyData());
		}

		std::uint8_t* get_member(const int index)
		{
			return get_party() + members_offset + index * member_size;
		}

		template <typename T>
		T& field(std::uint8_t* base, const std::size_t offset)
		{
			return *reinterpret_cast<T*>(base + offset);
		}

		bool get_dvar_bool(const char* name)
		{
			const auto* dvar = game::Dvar_FindVar(name);
			return dvar && dvar->current.enabled;
		}

		int get_dvar_int(const char* name, const int default_value)
		{
			const auto* dvar = game::Dvar_FindVar(name);
			return dvar ? dvar->current.integer : default_value;
		}

		bool is_lobby_host()
		{
			auto* party = get_party();
			return field<int>(party, 0x2D14) != 0 && field<int>(party, 0x2D08) != 0 && field<int>(party, 0x2D10) != 0;
		}

		// Sala pública: solo la sala de partida pública de multijugador (ui_scripts/public_lobby:
		// Lobby.SetPartyUIRoot(PartyUIRoot.PUBLIC_MATCH_LOBBY) pone lobby_bots_public_root; las otras salas lo quitan),
		// en el menú y como anfitrión del party
		bool is_public_lobby()
		{
			return game::Com_FrontEnd_IsInFrontEnd() && lobby_bots_public_root && lobby_bots_public_root->current.enabled &&
				game::Com_GameMode_GetActiveGameMode() == game::GAME_MODE_MP && !get_dvar_bool("systemlink") && is_lobby_host();
		}

		// La sala pública no depende de party_minplayers: el juego lo lee (Dvar_GetInt) para decidir si empieza o
		// mantiene la cuenta atrás y cuántos jugadores faltan. En la sala pública se responde 1 con "Iniciar partida"
		// pulsado y, si no, uno más que party_maxplayers (nunca se alcanza: la sala espera). Fuera de ella, el valor real.
		int party_game_start_requested_stub(game::dvar_t* dvar)
		{
			if (!is_public_lobby())
			{
				return utils::hook::invoke<int>(0x140035D30, dvar);
			}

			return game_start_requested ? 1 : get_dvar_int("party_maxplayers", 18) + 1;
		}

		bool is_in_match()
		{
			return game::SV_Loaded() && !game::Com_FrontEnd_IsInFrontEnd();
		}

		bool is_fake(const std::uint64_t xuid)
		{
			return std::find(fake_xuids.begin(), fake_xuids.end(), xuid) != fake_xuids.end();
		}

		bool is_active(std::uint8_t* m)
		{
			return field<std::uint8_t>(m, member::status) >= status_in_lobby;
		}

		std::string get_member_name(std::uint8_t* m)
		{
			const auto* name = reinterpret_cast<const char*>(m + member::name);
			return std::string(name, strnlen(name, member::name_size));
		}

		int count_active_members()
		{
			auto count = 0;
			for (auto i = 0; i < max_members; ++i)
			{
				if (is_active(get_member(i)))
				{
					++count;
				}
			}
			return count;
		}

		int count_fake_members()
		{
			auto count = 0;
			for (auto i = 0; i < max_members; ++i)
			{
				auto* m = get_member(i);
				if (is_active(m) && is_fake(field<std::uint64_t>(m, member::xuid)))
				{
					++count;
				}
			}
			return count;
		}

		// Modo manual: el menú de bots ha fijado cuántos bots hay en cada equipo
		bool is_manual()
		{
			return (lobby_bots_allies && lobby_bots_allies->current.integer >= 0) ||
				(lobby_bots_enemies && lobby_bots_enemies->current.integer >= 0);
		}

		int manual_allies()
		{
			if (!get_dvar_bool("party_teambased"))
			{
				return 0;
			}
			return lobby_bots_allies ? std::max(0, lobby_bots_allies->current.integer) : 0;
		}

		int manual_enemies()
		{
			return lobby_bots_enemies ? std::max(0, lobby_bots_enemies->current.integer) : 0;
		}

		std::uint8_t* find_real_member()
		{
			for (auto i = 0; i < max_members; ++i)
			{
				auto* m = get_member(i);
				if (is_active(m) && !is_fake(field<std::uint64_t>(m, member::xuid)))
				{
					return m;
				}
			}
			return nullptr;
		}

		void load_names()
		{
			if (!name_list.empty())
			{
				return;
			}

			std::string data;
			if (utils::io::read_file("iw7s/bots.txt", &data))
			{
				for (auto& name : utils::string::split(data, '\n'))
				{
					name = utils::string::replace(name, "\r", "");
					if (!name.empty())
					{
						name_list.emplace_back(name);
					}
				}
			}

			default_names = name_list.empty();
			if (default_names)
			{
				name_list = iw_names;
			}
		}

		std::string pick_name()
		{
			load_names();

			std::vector<std::string> used{};
			for (auto i = 0; i < max_members; ++i)
			{
				auto* m = get_member(i);
				if (is_active(m))
				{
					used.emplace_back(get_member_name(m));
				}
			}

			std::vector<std::string> free{};
			for (const auto& name : name_list)
			{
				if (std::find(used.begin(), used.end(), name) == used.end())
				{
					free.emplace_back(name);
				}
			}

			if (free.empty())
			{
				return utils::string::va("Bot%d", random_int(100, 999));
			}

			return free[random_int(0, static_cast<int>(free.size()) - 1)];
		}

		// Rango y prestigio según la dificultad (la de la partida: bots::bot_set_difficulty)
		//   veterano: prestigio alto   curtido: prestigio 1 o 2   regular: rango normal   recluta: rango bajo
		void roll_rank(std::uint8_t* m, const int difficulty)
		{
			switch (difficulty)
			{
			case 3:
				field<int>(m, member::rank) = random_int(0, 54);
				field<int>(m, member::prestige) = random_int(5, 10);
				break;
			case 2:
				field<int>(m, member::rank) = random_int(0, 54);
				field<int>(m, member::prestige) = random_int(1, 2);
				break;
			case 1:
				field<int>(m, member::rank) = random_int(19, 54);
				field<int>(m, member::prestige) = 0;
				break;
			default:
				field<int>(m, member::rank) = random_int(0, 24);
				field<int>(m, member::prestige) = 0;
				break;
			}
		}

		// Dificultad mixta: la menos repetida entre los bots de la sala (quedan más o menos los mismos de cada una)
		int get_difficulty(const std::uint64_t xuid)
		{
			const auto itr = bot_difficulties.find(xuid);
			return itr == bot_difficulties.end() ? 1 : itr->second;
		}

		int pick_mixed_difficulty()
		{
			int counts[4]{};
			for (const auto& [xuid, difficulty] : bot_difficulties)
			{
				if (is_fake(xuid) && difficulty >= 0 && difficulty <= 3)
				{
					++counts[difficulty];
				}
			}

			const auto least = *std::min_element(std::begin(counts), std::end(counts));
			std::vector<int> candidates{};
			for (auto i = 0; i < 4; ++i)
			{
				if (counts[i] == least)
				{
					candidates.push_back(i);
				}
			}
			return candidates[random_int(0, static_cast<int>(candidates.size()) - 1)];
		}

		bool add_fake_member(const saved_bot* identity = nullptr)
		{
			auto* host = find_real_member();
			if (!host)
			{
				return false;
			}

			std::uint8_t* slot = nullptr;
			for (auto i = 0; i < max_members; ++i)
			{
				auto* m = get_member(i);
				if (field<std::uint8_t>(m, member::status) == 0)
				{
					slot = m;
					break;
				}
			}

			if (!slot)
			{
				return false;
			}

			std::memcpy(slot, host, member_size);

			std::uint64_t xuid{};
			do
			{
				xuid = 0x0110000100000000ull | static_cast<std::uint32_t>(rng()());
			}
			while (is_fake(xuid));

			const auto name = identity ? identity->name : pick_name();

			field<std::uint64_t>(slot, member::xuid) = xuid;
			field<std::uint64_t>(slot, member::party_id) = (static_cast<std::uint64_t>(rng()()) << 32) | rng()();
			const auto difficulty = identity ? identity->difficulty : pick_mixed_difficulty();
			bot_difficulties[xuid] = difficulty;
			if (identity)
			{
				field<int>(slot, member::rank) = identity->rank;
				field<int>(slot, member::prestige) = identity->prestige;
			}
			else
			{
				roll_rank(slot, difficulty);
			}

			// Especialista y aspecto propios (no los del jugador)
			const auto rig = identity ? identity->rig : random_int(0, rig_count - 1);
			const auto& heads = rig_heads[rig];
			const auto& bodies = rig_bodies[rig];
			field<int>(slot, member::rig) = rig;
			field<int>(slot, member::head) = identity ? identity->head : heads[random_int(0, static_cast<int>(heads.size()) - 1)];
			field<int>(slot, member::body) = identity ? identity->body : bodies[random_int(0, static_cast<int>(bodies.size()) - 1)];
			std::memset(slot + member::clan_tag, 0, 5);
			const auto* clan = get_clan_tag(name);
			std::memcpy(slot + member::clan_tag, clan, std::min<std::size_t>(std::strlen(clan), 4));
			field<int>(slot, member::team_selection) = 0;
			field<int>(slot, member::lobby_team) = 0;

			std::memset(slot + member::name, 0, member::name_size);
			std::memcpy(slot + member::name, name.data(), std::min<std::size_t>(name.size(), member::name_size - 1));

			field<int>(slot, member::last_packet) = game::Sys_Milliseconds();

			fake_xuids.push_back(xuid);
			field<std::uint8_t>(slot, member::status) = status_in_lobby;

			return true;
		}

		void remove_all_fake_members()
		{
			for (auto i = 0; i < max_members; ++i)
			{
				auto* m = get_member(i);
				if (is_active(m) && is_fake(field<std::uint64_t>(m, member::xuid)))
				{
					std::memset(m, 0, member_size);
				}
			}
			fake_xuids.clear();
			bot_difficulties.clear();
		}

		void schedule_next_join()
		{
			const auto min = lobby_bots_join_min ? lobby_bots_join_min->current.integer : 1500;
			const auto max = lobby_bots_join_max ? lobby_bots_join_max->current.integer : 5000;
			next_join_time = game::Sys_Milliseconds() + random_int(min, max);
		}

		// BG_Bots_IsBotMatchMakingAllowedForPlaylist (0x140036FD0, isbotmatchmakingenabled en GSC). La versión nueva
		// lo activa en partida pública (ranked.cpp, nop en 0x140036FFB) y bots::spawn_bots crea los bots con
		// addmpbottoteam, que no pasa por SV_BotGetRandomName: los bots de la sala salían con otro nombre y rango.
		// Con bots de la sala se usa addbot como en la versión anterior.
		utils::hook::detour bot_matchmaking_allowed_hook;

		bool bot_matchmaking_allowed_stub()
		{
			if (has_lobby_bots)
			{
				return false;
			}
			return bot_matchmaking_allowed_hook.invoke<bool>();
		}

		// PartyHost_RemoveMember (0x1409D86F0): el anfitrión echa a los miembros sin conexión o que no mandan paquetes
		// (0x1409DA2D0, llamada 0x1409DA338). A los bots de la sala no se les echa.
		utils::hook::detour party_host_remove_member_hook;

		void party_host_remove_member_stub(std::uint8_t* party, const int index, const int reason, void* a4)
		{
			if (party == get_party() && index >= 0 && index < max_members)
			{
				std::lock_guard _(mutex);
				auto* m = get_member(index);
				if (is_active(m) && is_fake(field<std::uint64_t>(m, member::xuid)) && is_public_lobby())
				{
					return;
				}
			}

			party_host_remove_member_hook.invoke<void>(party, index, reason, a4);
		}

		struct match_bot
		{
			std::string name;
			int team;
		};

		const char* get_team_name(const int team)
		{
			if (!get_dvar_bool("party_teambased"))
			{
				return "autoassign";
			}

			return team == 1 ? "axis" : (team == 2 ? "allies" : "autoassign");
		}

		// Un bot cada medio segundo con su nombre y el equipo de la sala (bots::spawn_bots(1, equipo)). Los nombres van
		// en cola: cada SV_BotGetRandomName toma el siguiente
		void spawn_match_bots(const std::vector<match_bot>& bots)
		{
			pending_names.clear();

			for (auto i = 0u; i < bots.size(); ++i)
			{
				const auto bot = bots[i];
				scheduler::once([bot]
				{
					if (!is_in_match())
					{
						return;
					}

					{
						std::lock_guard _(mutex);
						pending_names.push_back(bot.name);
					}

					scripting::call_script_function(0, "scripts/mp/bots/bots", "spawn_bots",
						{1u, get_team_name(bot.team)});
				}, scheduler::pipeline::server, std::chrono::milliseconds(500 * i));
			}
		}

		int count_team(const int team)
		{
			auto count = 0;
			for (auto i = 0; i < max_members; ++i)
			{
				auto* m = get_member(i);
				if (is_active(m) && field<int>(m, member::lobby_team) == team)
				{
					++count;
				}
			}
			return count;
		}

		// Equipos de la sala (miembro + 0x184: 1 = axis, 2 = allies). Sin el emparejamiento de Demonware el juego no
		// los reparte (0x1409CC060). El servidor los usa para meter a cada jugador en su equipo (0x140C50BC0).
		void assign_lobby_teams()
		{
			if (!get_dvar_bool("party_teambased"))
			{
				return;
			}

			// Jugador: el equipo elegido con L2 / R2 (lobby_setteam) o, en automático, alternado partida a partida
			if (auto* host = find_real_member(); host)
			{
				auto& team = field<int>(host, member::lobby_team);
				const auto choice = lobby_bots_team_choice ? lobby_bots_team_choice->current.integer : 0;
				if (team != 1 && team != 2 && (choice == 1 || choice == 2))
				{
					team = choice;
				}
				else if (team != 1 && team != 2)
				{
					if (next_host_team != 1 && next_host_team != 2)
					{
						next_host_team = random_int(1, 2);
					}
					team = next_host_team;
				}
			}

			// La lista de la sala agrupa por equipos (frontEnd.lobby.teamsVisible)
			if (auto* teams_visible = game::Dvar_FindVar("party_teamsVisible"); teams_visible && !teams_visible->current.enabled)
			{
				game::Dvar_SetBool(teams_visible, true);
			}

			// Modo manual: los aliados del menú van con el jugador y los enemigos al otro equipo
			auto* host = find_real_member();
			const auto host_team = host ? field<int>(host, member::lobby_team) : 0;
			const auto manual = is_manual() && (host_team == 1 || host_team == 2);
			const auto other_team = host_team == 1 ? 2 : 1;

			for (auto i = 0; i < max_members; ++i)
			{
				auto* m = get_member(i);
				if (!is_active(m))
				{
					continue;
				}

				auto& team = field<int>(m, member::lobby_team);
				if (team == 1 || team == 2)
				{
					continue;
				}

				if (manual && m != host)
				{
					const auto bots_with_host = count_team(host_team) - 1;
					team = bots_with_host < manual_allies() ? host_team : other_team;
					continue;
				}

				const auto axis = count_team(1);
				const auto allies = count_team(2);
				team = axis < allies ? 1 : (allies < axis ? 2 : random_int(1, 2));
			}
		}

		// Equipo de cada jugador de la sala para ui_scripts/MainMenu/lobbymemberteam.lua (lobby_bots_team_map:
		// "nombre\tequipo" por línea, el del jugador con '*' delante). La lista de la sala solo recibe los equipos al
		// empezar la cuenta atrás; con esto el script pinta los colores y agrupa la lista por equipos en todo momento.
		void publish_team_map()
		{
			if (!lobby_bots_team_map)
			{
				return;
			}

			std::string map{};
			auto* host = find_real_member();
			for (auto i = 0; i < max_members; ++i)
			{
				auto* m = get_member(i);
				if (!is_active(m))
				{
					continue;
				}

				map += utils::string::va("%s%s\t%d\n", m == host ? "*" : "", get_member_name(m).data(),
					field<int>(m, member::lobby_team));
			}

			if (map != lobby_bots_team_map->current.string)
			{
				game::Dvar_SetFromStringByName("lobby_bots_team_map", map.data(), game::DVAR_SOURCE_INTERNAL);
			}
		}

		// Dificultad elegida en el menú de bots para el equipo de cada bot: rango y prestigio según ella
		void apply_manual_difficulties()
		{
			auto* host = find_real_member();
			const auto host_team = host ? field<int>(host, member::lobby_team) : 0;
			const auto teambased = get_dvar_bool("party_teambased");

			for (auto i = 0; i < max_members; ++i)
			{
				auto* m = get_member(i);
				const auto xuid = field<std::uint64_t>(m, member::xuid);
				if (!is_active(m) || !is_fake(xuid))
				{
					continue;
				}

				const auto team = field<int>(m, member::lobby_team);
				if (teambased && team != 1 && team != 2)
				{
					continue;
				}

				const auto ally = teambased && team == host_team;
				auto* dvar = ally ? lobby_bots_difficulty_allies : lobby_bots_difficulty_enemies;
				const auto wanted = dvar ? dvar->current.integer : -1;
				if (wanted < 0 || wanted > 3 || get_difficulty(xuid) == wanted)
				{
					continue;
				}

				bot_difficulties[xuid] = wanted;
				roll_rank(m, wanted);
			}
		}

		// Modo manual: quita los bots que sobran (si se baja la cantidad en el menú)
		void trim_manual_bots()
		{
			const auto target = manual_allies() + manual_enemies();
			for (auto i = max_members - 1; i >= 0 && count_fake_members() > target; --i)
			{
				auto* m = get_member(i);
				const auto xuid = field<std::uint64_t>(m, member::xuid);
				if (is_active(m) && is_fake(xuid))
				{
					std::memset(m, 0, member_size);
					fake_xuids.erase(std::remove(fake_xuids.begin(), fake_xuids.end(), xuid), fake_xuids.end());
				}
			}
		}

		// bot_autoconnectdefault: la playlist lo pone a 1; en modo manual los bots de la partida son solo los de la sala
		// En modo manual se fuerza el dvar (como texto: no todos son enteros) y al volver a automático se restaura
		void force_dvar(const char* name, const char* value, const bool force)
		{
			auto* dvar = game::Dvar_FindVar(name);
			if (!dvar)
			{
				return;
			}

			const std::string current = game::Dvar_ValueToString(dvar, dvar->current);
			const auto saved = saved_dvars.find(name);

			if (force)
			{
				if (current != value)
				{
					if (saved == saved_dvars.end())
					{
						saved_dvars[name] = current;
					}
					game::Dvar_SetFromStringByName(name, value, game::DVAR_SOURCE_INTERNAL);
				}
			}
			else if (saved != saved_dvars.end())
			{
				game::Dvar_SetFromStringByName(name, saved->second.data(), game::DVAR_SOURCE_INTERNAL);
				saved_dvars.erase(saved);
			}
		}

		// Modo manual: sin bots automáticos de la playlist (bot_autoconnectdefault) y sin equilibrar equipos
		// (scr_teambalance, scripts/mp/teams: pasa jugadores al otro equipo hasta dejarlos iguales)
		void update_autoconnect(const bool manual)
		{
			force_dvar("bot_autoconnectdefault", "0", manual);
			force_dvar("scr_teambalance", "0", manual);
		}

		void reset_manual()
		{
			if (lobby_bots_allies && lobby_bots_allies->current.integer != -1)
			{
				game::Dvar_SetInt(lobby_bots_allies, -1);
			}
			if (lobby_bots_enemies && lobby_bots_enemies->current.integer != -1)
			{
				game::Dvar_SetInt(lobby_bots_enemies, -1);
			}
			if (lobby_bots_difficulty_allies && lobby_bots_difficulty_allies->current.integer != -1)
			{
				game::Dvar_SetInt(lobby_bots_difficulty_allies, -1);
			}
			if (lobby_bots_difficulty_enemies && lobby_bots_difficulty_enemies->current.integer != -1)
			{
				game::Dvar_SetInt(lobby_bots_difficulty_enemies, -1);
			}
			applied_allies = -2;
			applied_enemies = -2;
		}

		// La sala vuelve a esperar al botón "Iniciar partida" (si había cuenta atrás, el juego la cancela)
		void cancel_game_start()
		{
			if (!game_start_requested)
			{
				return;
			}

			game_start_requested = false;

			if (lobby_bots_game_start_requested)
			{
				game::Dvar_SetBool(lobby_bots_game_start_requested, false);
			}
		}

		void update()
		{
			std::lock_guard _(mutex);

			if (!lobby_bots_enabled || !lobby_bots_enabled->current.enabled)
			{
				return;
			}

			// Lobby.GetNumLobbyMembers devuelve 0 a ratos y el texto "faltan N jugadores" salta al máximo: la sala usa
			// este recuento (ui_scripts/public_lobby)
			if (lobby_bots_members)
			{
				const auto members = is_public_lobby() ? count_active_members() : 0;
				if (lobby_bots_members->current.integer != members)
				{
					game::Dvar_SetInt(lobby_bots_members, members);
				}
			}

			has_lobby_bots = !fake_xuids.empty();

			// Sala pública con bots y sus partidas: sin bots automáticos de la playlist (los creaba bots.gsc antes que
			// los de la sala, con otro nombre y rango, y llenaban la partida) ni equilibrado de equipos
			update_autoconnect((is_manual() || !fake_xuids.empty()) && (is_in_match() || is_public_lobby()));

			// Partida: crear los bots de la sala
			if (is_in_match())
			{
				came_from_match = true;

				if (bots_spawned || fake_xuids.empty())
				{
					return;
				}

				const auto now = game::Sys_Milliseconds();
				if (!match_seen_time)
				{
					match_seen_time = now;
					return;
				}

				// Margen para que el jugador local termine de conectarse
				if (now - match_seen_time < 3000)
				{
					return;
				}

				std::vector<match_bot> bots{};
				saved_roster.clear();
				for (auto i = 0; i < max_members; ++i)
				{
					auto* m = get_member(i);
					if (is_active(m) && is_fake(field<std::uint64_t>(m, member::xuid)))
					{
						bots.push_back({get_member_name(m), field<int>(m, member::lobby_team)});
						saved_roster.push_back({get_member_name(m), field<int>(m, member::rank), field<int>(m, member::prestige),
							field<int>(m, member::rig), field<int>(m, member::head), field<int>(m, member::body), get_difficulty(field<std::uint64_t>(m, member::xuid))});
					}
				}

				restore_pending = !saved_roster.empty();
				match_identities = saved_roster;
				match_teams.clear();
				for (const auto& bot : bots)
				{
					match_teams[bot.name] = bot.team;
				}

				if (auto* host = find_real_member(); host)
				{
					const auto host_team = field<int>(host, member::lobby_team);
					next_host_team = host_team == 1 ? 2 : (host_team == 2 ? 1 : 0);
				}

				bots_spawned = true;
				if (!bots.empty())
				{
					spawn_match_bots(bots);
				}
				return;
			}

			bots_spawned = false;
			match_seen_time = 0;

			// Vuelta de una partida que no ha terminado (el jugador se ha salido): el equipo elegido con L2 / R2 vuelve a
			// automático. Antes solo se hacía sin sala, y el menú de partidas públicas crea otra enseguida (somos
			// anfitrión), así que el equipo se mantenía. lobby_bots.gsc pone lobby_bots_match_finished a 0 al empezar
			// la partida y a 1 con game_ended.
			if (came_from_match && game::Com_FrontEnd_IsInFrontEnd() && !get_dvar_int("lobby_bots_match_finished", 0) &&
				lobby_bots_team_choice && lobby_bots_team_choice->current.integer != 0)
			{
				game::Dvar_SetInt(lobby_bots_team_choice, 0);
			}

			if (game_start_requested && game::Com_FrontEnd_IsInFrontEnd() && (came_from_match || !is_public_lobby()))
			{
				cancel_game_start();
			}

			// Modo nuevo elegido en la playlist: como en una búsqueda nueva, sala nueva con otros jugadores. Se van los
			// bots y entra de golpe una tanda inicial nueva (antes entraban de uno en uno)
			if (lobby_bots_new_lobby && lobby_bots_new_lobby->current.enabled)
			{
				game::Dvar_SetBool(lobby_bots_new_lobby, false);
				if (game::Com_FrontEnd_IsInFrontEnd())
				{
					remove_all_fake_members();
					saved_roster.clear();
					restore_pending = false;
					first_wave_done = false;
					next_join_time = 0;
				}
			}

			if (!is_public_lobby())
			{
				if (!fake_xuids.empty() && !game::Com_FrontEnd_IsInFrontEnd())
				{
					return; // cargando la partida
				}

				if (!fake_xuids.empty())
				{
					remove_all_fake_members();
				}

				// Sala cerrada (el jugador ha vuelto atrás): el menú de partidas públicas crea otra sala enseguida y
				// los bots no deben entrar hasta que se vuelva a abrir la pantalla de la sala (LobbyMission)
				if (game::Com_FrontEnd_IsInFrontEnd() && !is_lobby_host() && lobby_bots_lobby_menu &&
					lobby_bots_lobby_menu->current.enabled)
				{
					game::Dvar_SetBool(lobby_bots_lobby_menu, false);
				}

				// Fuera de la sala pública en el menú (has salido de ella o de una partida; el menú de partidas públicas
				// crea otra sala enseguida, así que no se mira si somos anfitrión): el equipo elegido con L2 / R2
				// vuelve a automático. Al terminar una partida (custom_scripts/mp/lobby_bots.gsc pone
				// lobby_bots_match_finished con game_ended) se vuelve a la misma sala y se mantiene.
				if (game::Com_FrontEnd_IsInFrontEnd() && lobby_bots_team_choice &&
					lobby_bots_team_choice->current.integer != 0 && !(restore_pending && get_dvar_int("lobby_bots_match_finished", 0)))
				{
					game::Dvar_SetInt(lobby_bots_team_choice, 0);
				}

				// Fuera de la sala sin venir de una partida (el jugador ha salido): se olvidan los bots
				if (!restore_pending && game::Com_FrontEnd_IsInFrontEnd() && !is_lobby_host())
				{
					saved_roster.clear();
					reset_manual();
				}

				next_join_time = 0;
				first_wave_done = false;
				return;
			}

			// Los bots no mandan paquetes: se les renueva la hora del último paquete (miembro + 0x134) para que el
			// anfitrión no los eche por inactividad (0x1409DA2D0). Los que ya no están se olvidan.
			const auto now_ms = game::Sys_Milliseconds();
			for (auto it = fake_xuids.begin(); it != fake_xuids.end();)
			{
				auto found = false;
				for (auto i = 0; i < max_members; ++i)
				{
					auto* m = get_member(i);
					if (is_active(m) && field<std::uint64_t>(m, member::xuid) == *it)
					{
						field<int>(m, member::last_packet) = now_ms;
						found = true;
						break;
					}
				}

				it = found ? std::next(it) : fake_xuids.erase(it);
			}

			// Vuelta de una partida con la misma sala: algunos bots se van y se hacen equipos nuevos (el jugador cambia
			// de equipo). Si la sala es nueva (sin bots), los recupera la tanda inicial (saved_roster)
			if (came_from_match)
			{
				came_from_match = false;

				if (!fake_xuids.empty())
				{
					restore_pending = false;
					saved_roster.clear();
					first_wave_done = true;

					const auto leave_chance = lobby_bots_leave_chance ? lobby_bots_leave_chance->current.integer : 25;
					for (auto i = 0; i < max_members; ++i)
					{
						auto* m = get_member(i);
						if (!is_active(m))
						{
							continue;
						}

						field<int>(m, member::lobby_team) = 0;

						const auto xuid = field<std::uint64_t>(m, member::xuid);
						if (is_fake(xuid) && random_int(1, 100) <= leave_chance)
						{
							std::memset(m, 0, member_size);
							fake_xuids.erase(std::remove(fake_xuids.begin(), fake_xuids.end(), xuid), fake_xuids.end());
						}
					}

					schedule_next_join();
				}
			}

			// Menú de bots: al cambiar las cantidades se quitan los que sobran y se reparten de nuevo los equipos
			if (is_manual())
			{
				const auto allies = manual_allies();
				const auto enemies = manual_enemies();
				if (allies != applied_allies || enemies != applied_enemies)
				{
					applied_allies = allies;
					applied_enemies = enemies;

					trim_manual_bots();
					auto* host = find_real_member();
					for (auto i = 0; i < max_members; ++i)
					{
						auto* m = get_member(i);
						if (is_active(m) && m != host)
						{
							field<int>(m, member::lobby_team) = 0;
						}
					}
				}
			}
			else if (applied_allies != -2)
			{
				// Vuelta a automático: reparto normal
				applied_allies = -2;
				applied_enemies = -2;
				for (auto i = 0; i < max_members; ++i)
				{
					auto* m = get_member(i);
					if (is_active(m) && m != find_real_member())
					{
						field<int>(m, member::lobby_team) = 0;
					}
				}
			}

			assign_lobby_teams();
			apply_manual_difficulties();

			publish_team_map();

			// Al empezar la cuenta atrás (estado 4 -> 8, 0x1409D7D80) el anfitrión reparte los equipos con los grupos del
			// emparejamiento (0x1409D74F0 -> 0x140CBD7D0) y, sin emparejamiento, falla y la sala vuelve a "esperando"
			// (0x1409DA0F0) salvo con online_matchmaking_ignore_team_check. Los equipos ya los pone assign_lobby_teams.
			if (auto* ignore_team_check = game::Dvar_FindVar("online_matchmaking_ignore_team_check");
				ignore_team_check && !ignore_team_check->current.enabled)
			{
				game::Dvar_SetBool(ignore_team_check, true);
			}

			// Modo manual: el jugador más los bots del menú (como mucho party_maxplayers / 18)
			auto max_players = get_dvar_int("party_maxplayers", 12);
			if (is_manual())
			{
				max_players = std::min(max_members, 1 + manual_allies() + manual_enemies());
			}

			if (count_active_members() >= max_players)
			{
				return;
			}

			// Los bots no entran hasta que se abre la sala (ui_scripts/public_lobby: SetPartyUIRoot(PUBLIC_MATCH_LOBBY)).
			// En el menú de partidas públicas la sala ya existe y la cuenta atrás se retiene, pero sin bots.
			if (!lobby_bots_lobby_menu || !lobby_bots_lobby_menu->current.enabled)
			{
				return;
			}

			const auto now = game::Sys_Milliseconds();

			// Tanda inicial: al volver de una partida siguen los bots de la anterior (algunos se van); en una sala
			// nueva entran de golpe entre lobby_bots_wave_min y lobby_bots_wave_max
			if (!first_wave_done)
			{
				if (!next_join_time)
				{
					next_join_time = now + random_int(800, 2000);
					return;
				}

				if (now < next_join_time)
				{
					return;
				}

				first_wave_done = true;

				if (restore_pending)
				{
					restore_pending = false;

					// Equipos nuevos: el jugador cambia de equipo y los bots se reparten de nuevo
					for (auto i = 0; i < max_members; ++i)
					{
						auto* m = get_member(i);
						if (is_active(m))
						{
							field<int>(m, member::lobby_team) = 0;
						}
					}
					const auto leave_chance = lobby_bots_leave_chance ? lobby_bots_leave_chance->current.integer : 25;
					for (const auto& bot : saved_roster)
					{
						if (count_active_members() >= max_players)
						{
							break;
						}

						if (random_int(1, 100) <= leave_chance)
						{
							continue;
						}

						add_fake_member(&bot);
					}
				}
				else
				{
					const auto min = lobby_bots_first_min ? lobby_bots_first_min->current.integer : 8;
					const auto max = lobby_bots_first_max ? lobby_bots_first_max->current.integer : 12;
					const auto wave = random_int(min, max);
					for (auto i = 0; i < wave && count_active_members() < max_players; ++i)
					{
						add_fake_member();
					}
				}

				saved_roster.clear();
				schedule_next_join();
				return;
			}

			if (!next_join_time)
			{
				schedule_next_join();
				return;
			}

			if (now < next_join_time)
			{
				return;
			}

			add_fake_member();
			schedule_next_join();
		}
	}

	bool has_bots()
	{
		return has_lobby_bots;
	}

	const std::vector<std::string>& get_names()
	{
		std::lock_guard _(mutex);
		load_names();
		return name_list;
	}

	const char* get_clan_tag(const std::string& name)
	{
		std::lock_guard _(mutex);
		load_names();
		return default_names && std::ranges::find(iw_names, name) != iw_names.end() ? "IW" : "";
	}

	std::optional<std::string> take_bot_name()
	{
		std::lock_guard _(mutex);
		if (pending_names.empty())
		{
			return {};
		}

		auto name = pending_names.front();
		pending_names.erase(pending_names.begin());
		return name;
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

			lobby_bots_enabled = game::Dvar_RegisterBool("lobby_bots", true, game::DVAR_FLAG_SAVED,
				"Fill the public lobby with bots that join like players");
			lobby_bots_join_min = game::Dvar_RegisterInt("lobby_bots_join_min", 1500, 0, 60000, game::DVAR_FLAG_SAVED,
				"Minimum time in ms between bots joining the lobby");
			lobby_bots_join_max = game::Dvar_RegisterInt("lobby_bots_join_max", 5000, 0, 60000, game::DVAR_FLAG_SAVED,
				"Maximum time in ms between bots joining the lobby");

			lobby_bots_members = game::Dvar_RegisterInt("lobby_bots_members", 0, 0, 18, game::DVAR_FLAG_NONE,
				"Players in the public lobby (0 outside it)");
			// ui_scripts/OnlineServicesDelay: la ventana "Conectando con los servicios en línea" ya se ha mostrado en esta
			// sesión (el LUI se reinicia al volver de cada partida y perdería el estado)
			game::Dvar_RegisterBool("ui_onlineServicesDelayDone", false, game::DVAR_FLAG_NONE,
				"The fake online services popup has already been shown this session");
			lobby_bots_game_start_requested = game::Dvar_RegisterBool("lobby_bots_game_start_requested", false, game::DVAR_FLAG_NONE,
				"The player started the public lobby countdown (lobby_partygo)");
			// custom_scripts/mp/lobby_bots.gsc: 0 al empezar la partida, 1 si termina normalmente. Sin registrarlo, setdvar lo
			// creaba como texto y get_dvar_int leía basura (nunca 0): el equipo elegido no volvía a automático
			game::Dvar_RegisterInt("lobby_bots_match_finished", 0, 0, 1, game::DVAR_FLAG_NONE,
				"The last match ended normally (set by lobby_bots.gsc)");
			lobby_bots_public_root = game::Dvar_RegisterBool("lobby_bots_public_root", false, game::DVAR_FLAG_NONE,
				"Set by ui_scripts/public_lobby: the open lobby is the public match lobby");
			lobby_bots_team_choice = game::Dvar_RegisterInt("lobby_bots_team_choice", 0, 0, 2, game::DVAR_FLAG_NONE,
				"Team chosen with L2 / R2 in the public lobby (0 auto, 1 SDF, 2 UNSA)");
			// Los dvars de texto usan la memoria de scripts (MT_AllocIndex), que aún no existe aquí: se registra después,
			// como g_consoleLog (console.cpp)
			scheduler::once([]
			{
				lobby_bots_team_map = game::Dvar_RegisterString("lobby_bots_team_map", "", game::DVAR_FLAG_NONE,
					"Teams of the public lobby members for the lobby list (name\\tteam per line, the player marked with *)");
			}, scheduler::main);
			lobby_bots_new_lobby = game::Dvar_RegisterBool("lobby_bots_new_lobby", false, game::DVAR_FLAG_NONE,
				"Set by the playlist menu: a new mode was chosen, the public lobby gets new bots");
			lobby_bots_lobby_menu = game::Dvar_RegisterBool("lobby_bots_lobby_menu", false, game::DVAR_FLAG_NONE,
				"Set by ui_scripts/public_lobby: the public lobby menu has been opened (bots can join)");
			lobby_bots_first_min = game::Dvar_RegisterInt("lobby_bots_wave_min", 8, 0, 17, game::DVAR_FLAG_SAVED,
				"Minimum bots joining together when a new public lobby opens");
			lobby_bots_first_max = game::Dvar_RegisterInt("lobby_bots_wave_max", 12, 0, 17, game::DVAR_FLAG_SAVED,
				"Maximum bots joining together when a new public lobby opens");
			lobby_bots_leave_chance = game::Dvar_RegisterInt("lobby_bots_leave_chance", 25, 0, 100, game::DVAR_FLAG_SAVED,
				"Chance (%) of each bot leaving the lobby after a match");

			lobby_bots_allies = game::Dvar_RegisterInt("lobby_bots_allies", -1, -1, 17, game::DVAR_FLAG_NONE,
				"Bot setup: bots on the player's team (-1 = automatic)");
			lobby_bots_enemies = game::Dvar_RegisterInt("lobby_bots_enemies", -1, -1, 17, game::DVAR_FLAG_NONE,
				"Bot setup: bots on the enemy team, all bots in free-for-all (-1 = automatic)");
			lobby_bots_difficulty_allies = game::Dvar_RegisterInt("lobby_bots_difficulty_allies", -1, -1, 3, game::DVAR_FLAG_NONE,
				"Bot setup: difficulty of allied bots (-1 = playlist / mixed, 0 recruit .. 3 veteran)");
			lobby_bots_difficulty_enemies = game::Dvar_RegisterInt("lobby_bots_difficulty_enemies", -1, -1, 3, game::DVAR_FLAG_NONE,
				"Bot setup: difficulty of enemy bots (-1 = playlist / mixed, 0 recruit .. 3 veteran)");

			// Botón "Iniciar partida" de la sala pública: da permiso para empezar la cuenta atrás (party_game_start_requested_stub).
			// Se quita al volver de la partida, al salir de la sala o al cancelar.
			utils::hook::call(0x1409D717B, party_game_start_requested_stub); // ¿puede empezar la cuenta atrás?
			utils::hook::call(0x1409D7F31, party_game_start_requested_stub); // ¿sigue la cuenta atrás?
			utils::hook::call(0x1409CA704, party_game_start_requested_stub); // jugadores que faltan
			utils::hook::call(0x1409CCC7A, party_game_start_requested_stub); // estado de la sala (esperando jugadores)

			command::add("lobby_partygo", []
			{
				std::lock_guard _(mutex);
				if (!is_public_lobby())
				{
					return;
				}

				if (game_start_requested)
				{
					return;
				}

				// Cuenta atrás como en la partida privada (8 s): pt_pregameStartTimerLength es la espera sin mostrar
				// (5 s por defecto) y pt_gameStartTimerLength la cuenta visible (8 s por defecto); queda 3 + 5.
				if (auto* pregame = game::Dvar_FindVar("pt_pregameStartTimerLength"))
				{
					game::Dvar_SetInt(pregame, 3);
				}
				if (auto* game_start = game::Dvar_FindVar("pt_gameStartTimerLength"))
				{
					game::Dvar_SetInt(game_start, 5);
				}

				game_start_requested = true;
				game::Dvar_SetBool(lobby_bots_game_start_requested, true);
			});

			// L2 / R2 en la sala pública (ui_scripts/MainMenu/lobbymission.lua): lobby_setteam <0 auto | 1 SDF | 2 UNSA>,
			// como el cambio de equipo de la partida privada. El jugador se pasa al equipo elegido (se mantiene entre
			// partidas); en automático se queda donde está y vuelve el reparto automático. Modo automático de bots: si el
			// equipo nuevo queda con 2 o más que el otro, un bot se pasa al que deja el jugador. Modo manual: los aliados del
			// menú siguen con el jugador (se cambian todos de equipo).
			command::add("lobby_setteam", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					return;
				}

				const auto choice = std::atoi(params.get(1));
				if (choice < 0 || choice > 2)
				{
					return;
				}

				std::lock_guard _(mutex);

				// Con "Iniciar partida" pulsado (cuenta atrás) ya no se puede cambiar de equipo
				if (lobby_bots_game_start_requested && lobby_bots_game_start_requested->current.enabled)
				{
					return;
				}

				if (lobby_bots_team_choice)
				{
					game::Dvar_SetInt(lobby_bots_team_choice, choice);
				}

				if (!choice || !game::Com_FrontEnd_IsInFrontEnd() || !is_public_lobby() || !get_dvar_bool("party_teambased"))
				{
					return;
				}

				auto* host = find_real_member();
				if (!host)
				{
					return;
				}

				auto& host_team = field<int>(host, member::lobby_team);
				if (host_team == choice || (host_team != 1 && host_team != 2))
				{
					host_team = choice;
					return;
				}

				const auto old_team = host_team;
				const auto new_team = choice;

				if (is_manual())
				{
					for (auto i = 0; i < max_members; ++i)
					{
						auto* m = get_member(i);
						if (m == host || !is_active(m) || !is_fake(field<std::uint64_t>(m, member::xuid)))
						{
							continue;
						}

						auto& team = field<int>(m, member::lobby_team);
						if (team == 1 || team == 2)
						{
							team = team == 1 ? 2 : 1;
						}
					}
					host_team = new_team;
					return;
				}

				host_team = new_team;
				if (count_team(new_team) - count_team(old_team) < 2)
				{
					return;
				}

				for (auto i = max_members - 1; i >= 0; --i)
				{
					auto* m = get_member(i);
					if (m != host && is_active(m) && is_fake(field<std::uint64_t>(m, member::xuid)) &&
						field<int>(m, member::lobby_team) == new_team)
					{
						field<int>(m, member::lobby_team) = old_team;
						break;
					}
				}
			});

			// Círculo / Escape en la sala pública con la cuenta atrás en marcha: se cancela, como en la partida privada
			command::add("lobby_cancelstartserver", []
			{
				std::lock_guard _(mutex);
				cancel_game_start();
			});

			// custom_scripts/mp/lobby_bots.gsc: rango y prestigio de la sala para cada bot (-1 si no es de la sala)
			const auto find_identity = [](const gsc::function_args& args) -> const saved_bot*
			{
				const auto name = args[0].as<std::string>();
				for (const auto& bot : match_identities)
				{
					if (bot.name == name)
					{
						return &bot;
					}
				}
				return nullptr;
			};

			gsc::function::add("lobbybotdifficulty", [find_identity](const gsc::function_args& args)
			{
				std::lock_guard _(mutex);
				const auto* bot = find_identity(args);
				return scripting::script_value(bot ? bot->difficulty : -1);
			});

			gsc::function::add("lobbybotrank", [find_identity](const gsc::function_args& args)
			{
				std::lock_guard _(mutex);
				const auto* bot = find_identity(args);
				return scripting::script_value(bot ? bot->rank : -1);
			});

			gsc::function::add("lobbybotprestige", [find_identity](const gsc::function_args& args)
			{
				std::lock_guard _(mutex);
				const auto* bot = find_identity(args);
				return scripting::script_value(bot ? bot->prestige : -1);
			});

			// custom_scripts/mp/lobby_bots.gsc: la partida es de la sala pública con bots
			gsc::function::add("lobbybotsactive", [](const gsc::function_args&)
			{
				return scripting::script_value(has_lobby_bots ? 1 : 0);
			});

			// custom_scripts/mp/lobby_bots.gsc: equipo de la sala de cada bot ("axis" / "allies", "" si no es de la sala)
			gsc::function::add("lobbybotteam", [](const gsc::function_args& args)
			{
				std::lock_guard _(mutex);
				const auto itr = match_teams.find(args[0].as<std::string>());
				if (itr == match_teams.end() || (itr->second != 1 && itr->second != 2))
				{
					return scripting::script_value(std::string{});
				}
				return scripting::script_value(std::string(itr->second == 1 ? "axis" : "allies"));
			});

			party_host_remove_member_hook.create(0x1409D86F0, party_host_remove_member_stub);
			bot_matchmaking_allowed_hook.create(0x140036FD0, bot_matchmaking_allowed_stub);

			scheduler::loop(update, scheduler::pipeline::main, 100ms);
		}
	};
}

REGISTER_COMPONENT(lobby_bots::component)
