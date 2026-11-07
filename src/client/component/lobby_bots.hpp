#pragma once

namespace lobby_bots
{
	// Nombre del siguiente bot de la sala que entra en la partida (vacío si no hay)
	std::optional<std::string> take_bot_name();

	// Hay bots de la sala pública (en la sala o en su partida)
	bool has_bots();

	// Nombres de los bots: los de iw7s\bots.txt o, si no hay, los de Infinity Ward (con el clan IW)
	const std::vector<std::string>& get_names();

	// Clan del bot: "IW" para los nombres de Infinity Ward, vacío para los de bots.txt
	const char* get_clan_tag(const std::string& name);
}
