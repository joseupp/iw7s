-- Menús abiertos desde la sala pública (reglas, bots...): "MULTIJUGADOR / PARTIDA PÚBLICA" en vez de
-- "JUEGO LOCAL / MULTIJUGADOR", como en Ajustes de juego y Mapas
local GetBreadcrumb_original = CoD.GetBreadcrumb
CoD.GetBreadcrumb = function ( ... )
	if not IsPrivateMatch() and Engine.GetDvarBool( "lobby_bots_public_root" ) then
		return ToUpperCase( Engine.Localize( "LUA_MENU_BREADCRUMB_2_ITEMS", "MENU_MULTIPLAYER", "LUA_MENU_PUBLIC_MATCH_CAPS" ) )
	end
	return GetBreadcrumb_original( ... )
end

-- Elegir un modo desde la playlist descarta las reglas cambiadas en la sala pública (ranked.cpp), como en BO3, y la
-- sala recibe bots nuevos de golpe (lobby_bots.cpp)
local Playlist_DoAction_original = Playlist.DoAction
Playlist.DoAction = function ( ... )
	Engine.SetDvarBool( "lobby_rules_reset", true )
	Engine.SetDvarBool( "lobby_bots_new_lobby", true )
	return Playlist_DoAction_original( ... )
end

-- Menú "Cambiar reglas de juego" en la sala pública: sin el fondo de la partida privada
-- (PostLoadFunc llama a Lobby.SetupPrivateMatchLobbyScene), como en los menús de mapas y de bots
local GameSetupOptionsMenu_original = MenuBuilder.m_types["GameSetupOptionsMenu"]
if not GameSetupOptionsMenu_original then
	return
end

MenuBuilder.m_types["GameSetupOptionsMenu"] = function ( menu, controller )
	if not Engine.GetDvarBool( "lobby_bots_public_root" ) then
		return GameSetupOptionsMenu_original( menu, controller )
	end

	local SetupPrivateMatchLobbyScene_original = Lobby.SetupPrivateMatchLobbyScene
	Lobby.SetupPrivateMatchLobbyScene = function ()
	end

	local ok, result = pcall( GameSetupOptionsMenu_original, menu, controller )
	Lobby.SetupPrivateMatchLobbyScene = SetupPrivateMatchLobbyScene_original

	if not ok then
		error( result )
	end
	return result
end

