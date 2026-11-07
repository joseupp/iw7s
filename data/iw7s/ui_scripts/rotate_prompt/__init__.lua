-- Aviso "Rotar" con el glifo del stick derecho, como en consola.
-- El juego solo lo pone con Engine.IsConsoleGame() (en PC el modelo también gira con el stick si se usa mando):
--   RigSelect, RigCustomization y WeaponSelect: al abrir el menú
--   LoadoutSelect (crear clase): solo mientras se ve el arma (ClientWeapon.SetWeaponVisible( 0, ... ))
if not Engine.InFrontend() or Engine.IsConsoleGame() then
	return
end

local BUTTON = "button_stickr_updown_noswap"

-- Los servicios en línea (killswitch_character_rotation) desactivan el giro del personaje: en RigSelect y
-- RigCustomization no se creaba la zona de giro (CharacterRotator) ni el aviso. Sin servidores no hace falta.
local get_dvar_bool = Engine.GetDvarBool
Engine.GetDvarBool = function ( name, ... )
	if name == "killswitch_character_rotation" then
		return false
	end
	return get_dvar_bool( name, ... )
end

local function use_gamepad()
	return Engine.IsGamepadEnabled() == 1
end

local function rotate_helper()
	return {
		helper_text = Engine.Localize( "LUA_MENU_ROTATE" ),
		button_ref = BUTTON,
		priority = 10,
		side = "left",
		clickable = false
	}
end

local function add_to_menu( menu )
	if menu and menu.AddButtonHelperTextToElement then
		menu:AddButtonHelperTextToElement( menu, rotate_helper() )
	elseif menu and menu.AddButtonHelperText then
		menu:AddButtonHelperText( rotate_helper() )
	end
end

local function wrap( name, builder )
	return function ( ... )
		local menu = builder( ... )
		if menu then
			local add = function ()
				if use_gamepad() then
					pcall( add_to_menu, menu )
				end
			end
			add()
			-- Al abrirlo, el menú de crear clase de debajo oculta el arma y quita su aviso (la barra de botones es la
			-- misma): se vuelve a poner un momento después y cada vez que el menú recupera el foco
			pcall( function ()
				menu:Wait( 250 ).onComplete = add
			end )
			pcall( function ()
				menu:addEventHandler( "gain_focus", function ( element, event )
					add()
				end )
			end )
		end
		return menu
	end
end

-- Crear clase: el aviso va y viene con el arma. SetWeaponVisible se llama también mientras se construye el menú
-- (antes de tener la instancia nueva), así que se guarda el estado y se aplica al terminar de construirlo.
local loadout_menu = nil
local weapon_visible = false

local function apply_loadout()
	local menu = loadout_menu
	if not menu or not use_gamepad() or not menu.AddButtonHelperText or not menu:getParent() then
		return
	end
	pcall( function ()
		if weapon_visible then
			menu:AddButtonHelperText( rotate_helper() )
		else
			menu:RemoveButtonHelperText( BUTTON, "left" )
		end
	end )
end

local function wrap_loadout( builder )
	return function ( ... )
		loadout_menu = nil
		local menu = builder( ... )
		loadout_menu = menu
		if menu then
			pcall( function ()
				menu:Wait( 1 ).onComplete = apply_loadout
			end )
		end
		return menu
	end
end

if ClientWeapon and ClientWeapon.SetWeaponVisible then
	local set_visible = ClientWeapon.SetWeaponVisible
	ClientWeapon.SetWeaponVisible = function ( index, visible, ... )
		local result = set_visible( index, visible, ... )
		if index == 0 then
			weapon_visible = visible and true or false
			apply_loadout()
		end
		return result
	end
end

-- pairs() no recorre tablas en este entorno (tampoco CRM.supportedSmallImages en motd): lista con ipairs
local names = {
	"RigSelect",
	"RigCustomization",
	"WeaponSelect",
	"LoadoutSelect"
}
local menus = {}
for _, name in ipairs( names ) do
	menus[name] = true
end

local function patch( name, builder )
	if type( builder ) ~= "function" then
		return builder
	end
	if name == "LoadoutSelect" then
		return wrap_loadout( builder )
	end
	return wrap( name, builder )
end

-- Estos menús se vuelven a registrar cada vez que se abren (MenuBuilder.registerType escribe en m_types; la tabla
-- MenuBuilder está bloqueada, así que no se puede sustituir registerType). Para que cada registro pase por aquí, sus
-- entradas no se guardan en m_types sino en "shadow": m_types las sigue encontrando por __index.
local types = MenuBuilder.m_types
local shadow = {}
for _, name in ipairs( names ) do
	local builder = rawget( types, name )
	if builder then
		shadow[name] = patch( name, builder )
		rawset( types, name, nil )
	end
end

local meta = getmetatable( types ) or {}
local old_index = meta.__index
local old_newindex = meta.__newindex

meta.__index = function ( t, name )
	if menus[name] then
		return shadow[name]
	end
	if type( old_index ) == "function" then
		return old_index( t, name )
	elseif type( old_index ) == "table" then
		return old_index[name]
	end
	return nil
end

meta.__newindex = function ( t, name, builder )
	if menus[name] then
		shadow[name] = patch( name, builder )
		return
	end
	if type( old_newindex ) == "function" then
		return old_newindex( t, name, builder )
	elseif type( old_newindex ) == "table" then
		old_newindex[name] = builder
		return
	end
	rawset( t, name, builder )
end
setmetatable( types, meta )
