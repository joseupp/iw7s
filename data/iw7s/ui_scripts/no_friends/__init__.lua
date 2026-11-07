-- Sin botón de amigos (Triángulo): en un cliente sin conexión no hay lista de amigos.
-- El aviso lo pone friendswidget con ButtonHelperText.CommonEvents.addFriendsButton en la barra de botones de los menús
-- (menú principal, menú de pausa de la partida, etc.), junto al contador de amigos (online_friends_widget). Triángulo
-- queda libre (en la sala pública cambia de equipo).

local function friends_text()
	local ok, text = pcall( Engine.Localize, "@LUA_MENU_FRIENDS" )
	return ok and text or nil
end

local function is_friends_button( event )
	if type( event ) ~= "table" or event.button_ref ~= "button_alt2" then
		return false
	end
	local text = friends_text()
	return text ~= nil and event.helper_text == text
end

-- El evento común: sin texto, la barra no añade nada
local function patch_common_events()
	if ButtonHelperText and ButtonHelperText.CommonEvents and ButtonHelperText.CommonEvents.addFriendsButton then
		pcall( function ()
			ButtonHelperText.CommonEvents.addFriendsButton.helper_text = ""
		end )
	end
end
patch_common_events()

-- Por si algún menú lo añade con su propia tabla: se filtra en las dos listas de la barra
local function filter_list( list )
	if not list or not ButtonHelperText or not ButtonHelperText.AddHelperTextObject then
		return
	end
	local add = ButtonHelperText.AddHelperTextObject
	list:registerEventHandler( "add_button_helper_text", function ( element, event )
		if is_friends_button( event ) then
			return
		end
		return add( element, event )
	end )
end

local function find_child( element, id )
	local child = element:getFirstChild()
	while child do
		if child.id == id then
			return child
		end
		child = child:getNextSibling()
	end
	return nil
end

local function wrap( builder )
	return function ( ... )
		patch_common_events()
		local self = builder( ... )
		pcall( function ()
			filter_list( find_child( self, "buttonHelperLeftList" ) )
			filter_list( find_child( self, "buttonHelperRightList" ) )
		end )
		return self
	end
end

if MenuBuilder.m_types["button_helper_text_main"] then
	MenuBuilder.m_types["button_helper_text_main"] = wrap( MenuBuilder.m_types["button_helper_text_main"] )
else
	local register_type = MenuBuilder.registerType
	MenuBuilder.registerType = function ( name, builder, ... )
		if name == "button_helper_text_main" then
			builder = wrap( builder )
		end
		return register_type( name, builder, ... )
	end
end

-- El contador de amigos (flecha + número) junto a la barra de botones: un elemento vacío en su lugar
local function empty_widget()
	local self = LUI.UIElement.new()
	self.id = "online_friends_widget"
	return self
end

if MenuBuilder.m_types["online_friends_widget"] then
	MenuBuilder.m_types["online_friends_widget"] = empty_widget
else
	local register_type_friends = MenuBuilder.registerType
	MenuBuilder.registerType = function ( name, builder, ... )
		if name == "online_friends_widget" then
			builder = empty_widget
		end
		return register_type_friends( name, builder, ... )
	end
end

-- Triángulo ya no abre la lista de amigos
local flow = LUI and LUI.FlowManager
if flow then
	local request_popup = flow.RequestPopupMenu
	flow.RequestPopupMenu = function ( element, menu, ... )
		if menu == "FriendsList" then
			return
		end
		return request_popup( element, menu, ... )
	end

	local request_add = flow.RequestAddMenu
	flow.RequestAddMenu = function ( menu, ... )
		if menu == "FriendsList" then
			return
		end
		return request_add( menu, ... )
	end
end
