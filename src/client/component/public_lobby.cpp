#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>

// Partida pública sin conexión.
//
// IW7 crea la sala pública a través del servicio de emparejamiento de Demonware (objeto 0x1417B7340, estados en
// Lobby.MatchmakerState). Sin servidores se queda para siempre en SEARCHING, aunque el juego ya nos ha hecho
// anfitrión de la sala (party 0x145254DA0, estado de sala 4 = esperando jugadores).
//
// El anfitrión solo empieza la cuenta atrás (0x1409D7D80, estado 0x20) si 0x1409D70E0 lo permite, y en partida
// pública eso exige que el emparejamiento haya creado / unido la sala (byte +0x9A00 del objeto, 0x140521EC0,
// llamada única en 0x1409D7222). Se responde que sí: el resto de condiciones (archivos del mapa) siguen siendo las
// del juego.
//
// Al acabar la cuenta atrás (0x1409DA544 y siguientes) el anfitrión público pide al emparejamiento que cierre la
// sala (0x1405244E0, pone +0x9A01) y espera a que lo confirme (+0x9A02, 0x140521620) antes de repartir equipos
// (0x1409D74F0) y lanzar la partida (0x1409D97D0). Se responde "pedido no pendiente" (0x140521ED0 -> false) y
// "confirmado" (0x140521620 -> true) en sus cuatro llamadas.
// Antes de eso 0x1409CDD00 (llamada 0x1409DA547) exige en pública una sala creada por el emparejamiento
// (party + 0x2CD4 == 1 o flag 0x400 en + 0x2D8C): se da por buena.
//
// La cuenta atrás es la nativa, pero en la sala pública no depende de party_minplayers: solo empieza con el botón
// "Iniciar partida" (lobby_startcountdown y party_minplayers_stub, lobby_bots.cpp).

namespace public_lobby
{
	namespace
	{
		bool matchmaker_lobby_ready_stub(void* /*matchmaker*/)
		{
			return true;
		}

		bool matchmaker_start_pending_stub(void* /*matchmaker*/)
		{
			return false;
		}

		bool matchmaker_start_confirmed_stub(void* /*matchmaker*/)
		{
			return true;
		}

		int party_can_start_stub(void* /*party*/)
		{
			return 1;
		}
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

			utils::hook::call(0x1409D7222, matchmaker_lobby_ready_stub);

			utils::hook::call(0x1409DA547, party_can_start_stub);
			utils::hook::call(0x1409DA56F, matchmaker_start_pending_stub);
			utils::hook::call(0x1409DA5A0, matchmaker_start_pending_stub);
			utils::hook::call(0x1409DA57F, matchmaker_start_confirmed_stub);
			utils::hook::call(0x1409DA5B4, matchmaker_start_confirmed_stub);
		}
	};
}

REGISTER_COMPONENT(public_lobby::component)
