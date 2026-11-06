local f0_local0 = module
local f0_local1, f0_local2 = ...
f0_local0( f0_local1, package.seeall )
f0_local0 = function ( f1_arg0, f1_arg1, f1_arg2 )
	f1_arg0.MenuTitle.MenuBreadcrumbs:setText( CoD.GetBreadcrumb(), 0 )
end

-- Botones del menú de bots de la sala pública (lobby_bots.cpp): cantidad de bots aliados / enemigos y su dificultad.
-- "Automático" (-1) deja la sala como hasta ahora (se llena hasta party_maxplayers, bot_autoconnectdefault de la playlist).
local MaxBots = 17

local CountLabels = function ()
	local labels = { "AUTOMÁTICO" }
	for i = 0, MaxBots, 1 do
		table.insert( labels, tostring( i ) )
	end
	return labels
end

local DifficultyLabels = function ()
	return {
		Engine.Localize( "LUA_MENU_BOTS_MIXED" ),
		Engine.Localize( "LUA_MENU_BOTS_RECRUIT" ),
		Engine.Localize( "LUA_MENU_BOTS_REGULAR" ),
		Engine.Localize( "LUA_MENU_BOTS_HARDENED" ),
		Engine.Localize( "LUA_MENU_BOTS_VETERAN" )
	}
end

-- valor del dvar (-1 = automático / mixta) <-> posición de la flecha (1 = automático / mixta)
local GetIndex = function ( dvar )
	return Engine.GetDvarInt( dvar ) + 2
end

local SetValue = function ( dvar, index )
	Engine.SetDvarInt( dvar, index - 2 )
end

local SetButtonIndex = function ( button, index )
	if button then
		button.currentValue = index
		button:UpdateContent()
	end
end

local AddCountLogic = function ( self, controller, button, dvar, otherButton, otherDvar )
	LUI.AddUIArrowTextButtonLogic( button, controller, {
		labels = CountLabels(),
		action = function ( index )
			SetValue( dvar, index )
			local value = index - 2
			if otherDvar then
				local other = Engine.GetDvarInt( otherDvar )
				if value < 0 then
					-- automático en los dos equipos
					if other >= 0 then
						Engine.SetDvarInt( otherDvar, -1 )
						SetButtonIndex( otherButton, 1 )
					end
				elseif other < 0 then
					-- al elegir una cantidad, el otro equipo empieza en 0
					Engine.SetDvarInt( otherDvar, 0 )
					SetButtonIndex( otherButton, 2 )
				elseif value + other > MaxBots then
					Engine.SetDvarInt( otherDvar, MaxBots - value )
					SetButtonIndex( otherButton, MaxBots - value + 2 )
				end
			end
		end,
		defaultValue = GetIndex( dvar ),
		wrapAround = false
	} )
end

local AddDifficultyLogic = function ( self, controller, button, dvar )
	LUI.AddUIArrowTextButtonLogic( button, controller, {
		labels = DifficultyLabels(),
		action = function ( index )
			SetValue( dvar, index )
		end,
		defaultValue = GetIndex( dvar ),
		wrapAround = true
	} )
end

local AddArrowButton = function ( self, controller, id, title, desc, top )
	local button = MenuBuilder.BuildRegisteredType( "GenericArrowButton", {
		controllerIndex = controller
	} )
	button.id = id
	button.buttonDescription = desc
	button.Title:setText( ToUpperCase( title ), 0 )
	button.Text:setText( "", 0 )
	button:SetAnchorsAndPosition( 0, 1, 0, 1, 0, _1080p * 600, _1080p * top, _1080p * (top + 30) )
	self:addElement( button )
	self[id] = button
	return button
end

function LobbyBotSetupButtons( menu, controller )
	local self = LUI.UIVerticalList.new()
	self:SetAnchorsAndPosition( 0, 1, 0, 1, 0, 600 * _1080p, 0, 280 * _1080p )
	self.id = "LobbyBotSetupButtons"
	local f1_local1 = controller and controller.controllerIndex
	if not f1_local1 and not Engine.InFrontend() then
		f1_local1 = self:getRootController()
	end
	assert( f1_local1 )
	self:SetSpacing( 10 * _1080p )

	if CONDITIONS.IsTeamBasedGameType( self ) then
		local friendly = AddArrowButton( self, f1_local1, "FriendlyBots", Engine.Localize( "LUA_MENU_FRIENDLY_BOTS_CAPS" ),
			Engine.Localize( "LUA_MENU_FRIENDLY_BOTS_DESC" ), 0 )
		local friendlyDifficulty = AddArrowButton( self, f1_local1, "FriendlyBotsDifficulty", Engine.Localize( "LUA_MENU_BOTDIFFICULTY_CAPS" ),
			Engine.Localize( "LUA_MENU_FRIENDLY_BOTDIFFICULTY_DESC" ), 40 )
		local enemy = AddArrowButton( self, f1_local1, "EnemyBots", Engine.Localize( "LUA_MENU_ENEMY_BOTS_CAPS" ),
			Engine.Localize( "LUA_MENU_ENEMY_BOTS_DESC" ), 100 )
		local enemyDifficulty = AddArrowButton( self, f1_local1, "EnemyBotsDifficulty", Engine.Localize( "LUA_MENU_BOTDIFFICULTY_CAPS" ),
			Engine.Localize( "LUA_MENU_ENEMY_BOTDIFFICULTY_DESC" ), 140 )

		AddCountLogic( self, f1_local1, friendly, "lobby_bots_allies", enemy, "lobby_bots_enemies" )
		AddCountLogic( self, f1_local1, enemy, "lobby_bots_enemies", friendly, "lobby_bots_allies" )
		AddDifficultyLogic( self, f1_local1, friendlyDifficulty, "lobby_bots_difficulty_allies" )
		AddDifficultyLogic( self, f1_local1, enemyDifficulty, "lobby_bots_difficulty_enemies" )
	else
		-- sin equipos: todos los bots van en lobby_bots_enemies
		local bots = AddArrowButton( self, f1_local1, "FFABots", Engine.Localize( "LUA_MENU_BOTS_CAPS" ),
			Engine.Localize( "LUA_MENU_BOTS_DESC" ), 0 )
		local botsDifficulty = AddArrowButton( self, f1_local1, "FFABotsDifficulty", Engine.Localize( "LUA_MENU_BOTDIFFICULTY_CAPS" ),
			Engine.Localize( "LUA_MENU_BOTDIFFICULTY_DESC" ), 40 )

		AddCountLogic( self, f1_local1, bots, "lobby_bots_enemies", nil, nil )
		AddDifficultyLogic( self, f1_local1, botsDifficulty, "lobby_bots_difficulty_enemies" )
	end

	local ButtonDescriptionText = MenuBuilder.BuildRegisteredType( "ButtonDescriptionText", {
		controllerIndex = f1_local1
	} )
	ButtonDescriptionText.id = "ButtonDescriptionText"
	ButtonDescriptionText:SetAnchorsAndPosition( 0, 1, 0, 1, 0, _1080p * 420, _1080p * 190, _1080p * 222 )
	self:addElement( ButtonDescriptionText )
	self.ButtonDescriptionText = ButtonDescriptionText

	return self
end

MenuBuilder.registerType( "LobbyBotSetupButtons", LobbyBotSetupButtons )

function GameSetupBots2( menu, controller )
	local self = LUI.UIVerticalNavigator.new()
	self.id = "GameSetupBots2"
	local f2_local1 = controller and controller.controllerIndex
	if not f2_local1 and not Engine.InFrontend() then
		f2_local1 = self:getRootController()
	end
	assert( f2_local1 )
	self:playSound( "menu_open" )
	local f2_local2 = self
	local ButtonHelperBar = nil
	
	ButtonHelperBar = MenuBuilder.BuildRegisteredType( "ButtonHelperBar", {
		controllerIndex = f2_local1
	} )
	ButtonHelperBar.id = "ButtonHelperBar"
	ButtonHelperBar:SetAnchorsAndPosition( 0, 0, 1, 0, 0, 0, _1080p * -85, 0 )
	self:addElement( ButtonHelperBar )
	self.ButtonHelperBar = ButtonHelperBar
	
	local MenuTitle = nil
	
	MenuTitle = MenuBuilder.BuildRegisteredType( "MenuTitle", {
		controllerIndex = f2_local1
	} )
	MenuTitle.id = "MenuTitle"
	MenuTitle.MenuTitle:setText( ToUpperCase( Engine.Localize( "LUA_MENU_BOT_SETUP_CAPS" ) ), 0 )
	MenuTitle.MenuBreadcrumbs:setText( ToUpperCase( Engine.Localize( "EXE_LOCAL_PLAY" ) ), 0 )
	MenuTitle.Icon:SetTop( _1080p * -28.5, 0 )
	MenuTitle.Icon:SetBottom( _1080p * 61.5, 0 )
	MenuTitle:SetAnchorsAndPosition( 0, 1, 0, 1, _1080p * 96, _1080p * 1056, _1080p * 54, _1080p * 134 )
	self:addElement( MenuTitle )
	self.MenuTitle = MenuTitle
	
	local FriendsElement = nil
	
	FriendsElement = MenuBuilder.BuildRegisteredType( "online_friends_widget", {
		controllerIndex = f2_local1
	} )
	FriendsElement.id = "FriendsElement"
	FriendsElement:SetFont( FONTS.GetFont( FONTS.MainCondensed.File ) )
	FriendsElement:SetAlignment( LUI.Alignment.Left )
	FriendsElement:SetAnchorsAndPosition( 0, 1, 1, 0, _1080p * 100, _1080p * 600, _1080p * -60, _1080p * -15 )
	self:addElement( FriendsElement )
	self.FriendsElement = FriendsElement
	
	local GameSetupButtons = nil
	
	GameSetupButtons = MenuBuilder.BuildRegisteredType( "LobbyBotSetupButtons", {
		controllerIndex = f2_local1
	} )
	GameSetupButtons.id = "GameSetupButtons"
	GameSetupButtons:SetAnchorsAndPosition( 0, 1, 0, 1, _1080p * 130, _1080p * 730, _1080p * 216, _1080p * 941 )
	self:addElement( GameSetupButtons )
	self.GameSetupButtons = GameSetupButtons
	
	local SocialFeed = nil
	
	SocialFeed = MenuBuilder.BuildRegisteredType( "SocialFeed", {
		controllerIndex = f2_local1
	} )
	SocialFeed.id = "SocialFeed"
	SocialFeed:SetAnchorsAndPosition( 0, 0, 1, 0, 0, 0, _1080p * -115, _1080p * -85 )
	self:addElement( SocialFeed )
	self.SocialFeed = SocialFeed
	
	self.addButtonHelperFunction = function ( f3_arg0, f3_arg1 )
		f3_arg0:AddButtonHelperText( {
			helper_text = Engine.Localize( "MENU_BACK" ),
			button_ref = "button_secondary",
			side = "left",
			priority = 10,
			clickable = true
		} )
		f3_arg0:AddButtonHelperText( {
			helper_text = Engine.Localize( "LUA_MENU_SELECT" ),
			button_ref = "button_primary",
			side = "left",
			priority = -10,
			clickable = true
		} )
        if Engine.IsCoreMode() then
			local self = LUI.UIElement.new( {
				worldBlur = 5
			} )
			self:setupWorldBlur()
			self.id = "blur"
			f3_arg0:addElement( self )
		end
	end
	
	self:addEventHandler( "menu_create", self.addButtonHelperFunction )
	
	local bindButton = LUI.UIBindButton.new()
	bindButton.id = "selfBindButton"
	self:addElement( bindButton )
	self.bindButton = bindButton
	
	self.bindButton:addEventHandler( "button_secondary", function ( f4_arg0, f4_arg1 )
		local f4_local0 = f4_arg1.controller or f2_local1
		ACTIONS.LeaveMenu( self )
	end )
	f0_local0( self, f2_local1, controller )
	return self
end

MenuBuilder.registerType( "GameSetupBots2", GameSetupBots2 )
LockTable( _M )
