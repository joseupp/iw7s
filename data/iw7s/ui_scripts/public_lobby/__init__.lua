-- Partida pública sin conexión: el emparejamiento de Demonware se queda en "Buscando una partida" para siempre.
-- Mientras somos anfitrión de la sala se muestra el estado real de la sala (jugadores que faltan, cuenta atrás).
if not Engine.InFrontend() or Lobby == nil or Lobby.GetStatusString == nil then
	return
end

-- lobby_bots: los bots solo entran en la sala de partida pública de multijugador
if Lobby.SetPartyUIRoot ~= nil and PartyUIRoot ~= nil then
	local original_set_party_ui_root = Lobby.SetPartyUIRoot
	-- Partida pública: menú de partidas públicas (PUBLIC_MATCH, "Missions") y sala (PUBLIC_MATCH_LOBBY). El menú de
	-- partidas públicas vuelve a llamar a SetPartyUIRoot(PUBLIC_MATCH) al volver de crear clase, intendente... y la
	-- sala ya existe antes de que se abra LobbyMission: si no cuenta como pública, la cuenta atrás empieza sola.
	-- Solo las otras salas lo quitan; el menú principal y los submenús no tocan nada (al salir de la sala deja de
	-- haber party y lobby_bots.cpp ya lo detecta).
	local public_roots = {
		--[PartyUIRoot.PUBLIC_MATCH] = true,
		[PartyUIRoot.PUBLIC_MATCH_LOBBY] = true
	}
	local other_lobby_roots = {
		[PartyUIRoot.PUBLIC_MATCH] = true,
		[PartyUIRoot.PRIVATE_MATCH] = true,
		[PartyUIRoot.SYSTEM_LINK] = true,
		[PartyUIRoot.SYSTEM_LINK_LOBBY] = true,
		[PartyUIRoot.GAME_BATTLES_MATCH] = true
	}

	Lobby.SetPartyUIRoot = function ( root, ... )
		if public_roots[root] then
			Engine.SetDvarBool( "lobby_bots_public_root", not Engine.IsAliensMode() )
		elseif other_lobby_roots[root] then
			Engine.SetDvarBool( "lobby_bots_public_root", false )
		end

		-- lobby_bots_lobby_menu: los bots solo entran después de abrir la sala (LobbyMission). Volver al menú de
		-- partidas públicas desde un submenú de la sala no lo quita; el menú principal y las otras salas sí.
		if root == PartyUIRoot.PUBLIC_MATCH_LOBBY then
			Engine.SetDvarBool( "lobby_bots_lobby_menu", not Engine.IsAliensMode() )
		elseif root == PartyUIRoot.MAIN_MENU or other_lobby_roots[root] then
			Engine.SetDvarBool( "lobby_bots_lobby_menu", false )
		end
		return original_set_party_ui_root( root, ... )
	end
end

local original_get_status_string = Lobby.GetStatusString
local original_get_matchmaker_status = Lobby.GetMatchmakerStatus
local original_get_num_lobby_members = Lobby.GetNumLobbyMembers

-- Lobby.GetNumLobbyMembers devuelve 0 a ratos: el recuento estable lo da lobby_bots.cpp (lobby_bots_members)
local function stable_num_lobby_members()
	return Engine.GetDvarInt( "lobby_bots_members" )
end

local function joined_lobby_status()
	return Lobby.MatchmakerState.JOINED_LOBBY, {}
end

-- Sala pública: el estado nativo de la sala (faltan N jugadores según party_minplayers, cuenta atrás) en lugar de
-- "Buscando una partida"
-- La sala se detecta con lobby_bots_members (Lobby.IsInLobby también falla a ratos)
Lobby.GetStatusString = function ( controller )
	if Engine.GetDvarBool( "lobby_bots_public_root" ) and Engine.GetDvarInt( "lobby_bots_members" ) > 0 then
		Lobby.GetMatchmakerStatus = joined_lobby_status
		Lobby.GetNumLobbyMembers = stable_num_lobby_members
		local ok, text, spinner, state = pcall( original_get_status_string, controller )
		Lobby.GetMatchmakerStatus = original_get_matchmaker_status
		Lobby.GetNumLobbyMembers = original_get_num_lobby_members
		if ok then
			return text, spinner, state
		end
	end
	return original_get_status_string( controller )
end
