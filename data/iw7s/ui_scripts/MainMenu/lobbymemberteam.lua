-- Sala pública: equipo de cada jugador como en la partida privada al elegir equipo con L2 / R2
-- (ui_scripts/MainMenu/lobbymission.lua, lobby_bots_team_choice: 0 auto, 1 SDF, 2 UNSA).
--   * En automático la lista va en negro y tú el primero; al empezar la cuenta atrás salen los colores del juego.
--   * Con SDF o UNSA elegido, como en Black Ops 3: arriba UNSA en azul y abajo SDF en rojo, y tú pasas al grupo de tu
--     equipo. Junto a tu nombre sale un momento el equipo con L2 / R2 (MPPlayerRole, como en la partida privada).
local team_names = {
	"LUA_MENU_AUTOASSIGN_CAPS",
	"MENU_FACTION_SDF_SHORT",
	"MENU_FACTION_UNSA_SHORT"
}

-- Equipos de la sala (lobby_bots.cpp, lobby_bots_team_map): la lista solo recibe los equipos al empezar la cuenta
-- atrás, así que con equipo elegido se usan estos
local get_team_map = function ()
	local map = {}
	local host = nil
	for line in string.gmatch( Engine.GetDvarString( "lobby_bots_team_map" ) or "", "[^\n]+" ) do
		local name, team = string.match( line, "^(.-)\t(%d+)$" )
		if name then
			if string.sub( name, 1, 1 ) == "*" then
				name = string.sub( name, 2 )
				host = name
			end
			map[name] = tonumber( team )
		end
	end
	return map, host
end

-- Equipo junto a tu nombre al pulsar L2 / R2: se muestra cuando tu fila ya está en su sitio (la lista se reordena al
-- cambiar de equipo y antes salía en la fila vieja)
local pending_role = nil

local team_chosen = function ()
	return Engine.GetDvarInt( "lobby_bots_team_choice" ) > 0
end

-- Lista de la sala pública: tú el primero y, con equipo elegido, agrupada por equipos como en la partida privada
local members_path = "alwaysLoaded.activeParty.members"
local LobbyMembers_original = MenuBuilder.m_types["LobbyMembers"]
if LobbyMembers_original and GetLobbyMemberDataSourceAtIndex then
	MenuBuilder.m_types["LobbyMembers"] = function ( menu, controller )
		local self = LobbyMembers_original( menu, controller )
		if not Engine.IsMultiplayer() or Engine.IsAliensMode() or not CONDITIONS.IsPublicMatch( self ) or not self.List then
			return self
		end

		local controller_index = controller and controller.controllerIndex or self:getRootController()
		local make_member = GetLobbyMemberDataSourceAtIndex( members_path )
		local count_source = LUI.DataSourceInGlobalModel.new( members_path .. ".count" )
		local order = {}

		-- Orden de la lista, como en Black Ops 3: en automático tú siempre el primero (el juego solo te pone arriba al
		-- empezar la cuenta atrás). Con equipo elegido, arriba UNSA (azul) y abajo SDF (rojo), cada uno en el orden de
		-- llegada y tú el primero del tuyo: los colores quedan fijos y lo que cambia es tu posición
		local compute_order = function ()
			local result = {}
			local count = count_source:GetValue( controller_index ) or 0
			for i = 0, count - 1, 1 do
				result[#result + 1] = i
			end

			local map, host = get_team_map()
			local chosen = team_chosen()
			local key = {}
			for _, i in ipairs( result ) do
				local name = LUI.DataSourceInGlobalModel.new( members_path .. "." .. i .. ".gamertag" ):GetValue( controller_index )
				local team = name and map[name]
				local is_host = name and name == host
				if not chosen then
					key[i] = is_host and 0 or 1
				elseif team == 2 then
					key[i] = is_host and 0 or 1
				elseif team == 1 then
					key[i] = is_host and 2 or 3
				else
					key[i] = is_host and 4 or 5
				end
			end
			table.sort( result, function ( a, b )
				if key[a] ~= key[b] then
					return key[a] < key[b]
				end
				return a < b
			end )
			return result
		end

		local apply_order = function ()
			order = compute_order()
			local source = LUI.DataSourceFromList.new( count_source )
			source.MakeDataSourceAtIndex = function ( list, index, controller_arg )
				return make_member( list, order[index + 1] or index, controller_arg )
			end
			self.List:SetGridDataSource( source )
		end

		apply_order()
		local last_order = table.concat( order, "," )
		local timer = LUI.UITimer.new( nil, {
			interval = 100,
			event = "update_public_member_order"
		} )
		timer.id = "PublicMemberOrderTimer"
		self:addElement( timer )
		self:registerEventHandler( "update_public_member_order", function ()
			local new_order = table.concat( compute_order(), "," )
			if new_order ~= last_order then
				last_order = new_order
				apply_order()
			end

			-- Ya con tu equipo nuevo en la lista ordenada: tu fila puede mostrar el equipo
			if pending_role and not pending_role.ready then
				local map, host = get_team_map()
				if pending_role.choice == 0 or (host and map[host] == pending_role.choice) then
					pending_role.ready = true
				end
			end
		end )
		return self
	end
end

local LobbyMember_original = MenuBuilder.m_types["LobbyMember"]
if not LobbyMember_original then
	return
end

MenuBuilder.m_types["LobbyMember"] = function ( menu, controller )
	local self = LobbyMember_original( menu, controller )
	if not Engine.IsMultiplayer() or Engine.IsAliensMode() or not CONDITIONS.IsPublicMatch( self ) or self.MPPlayerRole then
		return self
	end

	local controller_index = controller and controller.controllerIndex
	if not controller_index then
		controller_index = self:getRootController()
	end

	-- Equipo junto al nombre (solo en tu fila)
	local role = MenuBuilder.BuildRegisteredType( "MPPlayerRole", {
		controllerIndex = controller_index
	} )
	role.id = "MPPlayerRole"
	role:SetDataSourceThroughElement( self, nil )
	role:SetAnchorsAndPosition( 0, 0, 0, 1, _1080p * 336, _1080p * -32, 0, _1080p * 31 )
	self:addElement( role )
	self.MPPlayerRole = role
	if role.LeftBump and role.LeftBump.setText then
		role.LeftBump:setText( Engine.Localize( "LUA_MENU_PAD_LEFT_TRIGGER_BUTTON" ), 0 )
	end
	if role.RightBump and role.RightBump.setText then
		role.RightBump:setText( Engine.Localize( "LUA_MENU_PAD_RIGHT_TRIGGER_BUTTON" ), 0 )
	end
	-- L1 / R1 cambian la misión activa en la sala pública: el selector del juego no los usa aquí
	if role.BindButton then
		role.BindButton:registerEventHandler( "button_shoulderl", function ()
			return false
		end )
		role.BindButton:registerEventHandler( "button_shoulderr", function ()
			return false
		end )
	end

	local is_local_member = function ()
		local data = self:GetDataSource()
		return data and data.controllerIndex and data.controllerIndex:GetValue( controller_index ) == controller_index
	end

	-- Colores de la fila
	local teams_visible = LUI.DataSourceInGlobalModel.new( "frontEnd.lobby.teamsVisible" )
	local recolor = function ()
		local data = self:GetDataSource()
		if not data or not self.Background then
			return
		end

		local team = data.team:GetValue( controller_index )
		if team_chosen() and data.gamertag then
			local map = get_team_map()
			local name = data.gamertag:GetValue( controller_index )
			if name and map[name] then
				team = map[name]
			end
		end
		local color = GetIntForColor( SWATCHES.genericMenu.blackBackground )
		if team and CONDITIONS.IsTeamBasedGameType() and team_chosen() then
			-- Equipo elegido: colores de cada equipo siempre, como en la partida privada (sin esperar a la cuenta atrás)
			if team == Teams.allies then
				color = GetIntForColor( SWATCHES.HUD.friendlyTeam )
			elseif team == Teams.axis then
				color = GetIntForColor( SWATCHES.HUD.enemyTeam )
			end
		elseif team and CONDITIONS.IsTeamBasedGameType() and teams_visible:GetValue( controller_index ) then
			-- Automático: los colores del juego al empezar la cuenta atrás (tu equipo azul, el otro rojo)
			local local_team = Engine.GetTeamForLocalClient()
			if local_team == Teams.allies or local_team == Teams.axis then
				color = GetIntForColor( team == local_team and SWATCHES.HUD.friendlyTeam or SWATCHES.HUD.enemyTeam )
			end
		end
		self.Background._public_color = true
		self.Background:SetRGBFromInt( color )
		self.Background._public_color = nil
	end

	if self.Background then
		-- Con equipo elegido el color lo pone solo este script: el del juego (negro hasta la cuenta atrás) hacía
		-- parpadear las filas al reordenar la lista
		local set_rgb = self.Background.SetRGBFromInt
		self.Background.SetRGBFromInt = function ( element, ... )
			if team_chosen() and not element._public_color then
				return
			end
			return set_rgb( element, ... )
		end
		self.Background:SubscribeToModel( teams_visible:GetModel( controller_index ), recolor )
		self.Background:SubscribeToModelThroughElement( self, "team", recolor )
		-- Fila nueva al reordenar la lista: color en cuanto recibe su jugador
		self.Background:SubscribeToModelThroughElement( self, "gamertag", recolor )
	end

	-- Los equipos de lobby_bots_team_map no avisan al cambiar: se repinta cada décima de segundo
	local color_timer = LUI.UITimer.new( nil, {
		interval = 100,
		event = "update_public_member_color"
	} )
	color_timer.id = "PublicMemberColorTimer"
	self:addElement( color_timer )
	local show_role = function ( choice )
		if role.RoleName then
			role.RoleName:setText( Engine.Localize( team_names[choice + 1] or team_names[1] ), 0 )
			ACTIONS.AnimateSequence( role, "Show" )
		end
	end

	self:registerEventHandler( "update_public_member_color", function ()
		recolor()
		if pending_role and pending_role.ready and is_local_member() then
			local choice = pending_role.choice
			pending_role = nil
			show_role( choice )
		end
	end )

	self:registerEventHandler( "public_team_choice", function ( element, event )
		recolor()
		if is_local_member() then
			pending_role = {
				choice = event.choice or Engine.GetDvarInt( "lobby_bots_team_choice" ),
				ready = false
			}
		end
	end )

	return self
end
