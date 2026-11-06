#pragma once

namespace lobby_bots
{
	// Nombre del siguiente bot de la sala que entra en la partida (vacío si no hay)
	std::optional<std::string> take_bot_name();

	// Hay bots de la sala pública (en la sala o en su partida)
	bool has_bots();
}
