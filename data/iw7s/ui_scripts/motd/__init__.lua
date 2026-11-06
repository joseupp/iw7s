-- Mensaje del día (CRM) sin conexión.
-- Sin servidores, Engine.CRMGetMessageContent no devuelve nada y el widget CRMMain del menú multijugador (imagen
-- pequeña + "pulsa para abrir") se oculta. Aquí se sirve un mensaje local leído de iw7s/dw/crm_motd.txt.
-- Formato del archivo (clave=valor, una por línea; en "long" se puede usar \n para saltos de línea):
--   title=Título
--   short=Texto corto bajo la imagen
--   long=Texto largo de la ventana emergente
--   image=image_motd_mp_dlc4_keyart_v1     (la pequeña es <image>_small si existe)
--   layout=1                               (1 = ventana completa, 0 = ventana dividida)
--   popup=1                                (1 = abrir la ventana al entrar al menú cuando cambie el mensaje)
if not Engine.InFrontend() or Engine.CRMGetMessageContent == nil then
	return
end

local file_path = "iw7s/dw/crm_motd.txt"

local defaults = {
	title = "IW7S",
	short = "Bienvenido a ^3IW7S",
	long = "Bienvenido a ^3IW7S^7.\n\nCliente sin conexión de Infinite Warfare.",
	image = "image_motd_mp_dlc4_keyart_v1",
	layout = "1",
	popup = "1"
}

local MP_MOTD = 1
local CP_MOTD = 11

local function read_config()
	local cfg = {}
	for k, v in pairs( defaults ) do
		cfg[k] = v
	end

	if io and io.fileexists and io.fileexists( file_path ) then
		local ok, data = pcall( io.readfile, file_path )
		if ok and type( data ) == "string" then
			-- quita el BOM UTF-8
			data = data:gsub( "^\239\187\191", "" )
			for line in data:gmatch( "[^\r\n]+" ) do
				local key, value = line:match( "^%s*([%w_]+)%s*=%s*(.-)%s*$" )
				if key and value then
					cfg[key] = value:gsub( "\\n", "\n" )
				end
			end
		end
	end
	return cfg
end

local cfg = read_config()

-- checksum numérico del contenido: si cambia el mensaje, la ventana vuelve a abrirse sola una vez
local function checksum( text )
	local h = 5381
	for i = 1, #text do
		h = (h * 33 + text:byte( i )) % 2147483647
	end
	return h
end

local message_checksum = checksum( cfg.title .. cfg.short .. cfg.long .. cfg.image )

local function make_message( location )
	return {
		title = cfg.title,
		content_short = cfg.short,
		content_long = cfg.long,
		image = cfg.image,
		layout_type = cfg.layout,
		location_id = location,
		message_index = 0,
		checksum = message_checksum
	}
end

local orig_get_content = Engine.CRMGetMessageContent
Engine.CRMGetMessageContent = function ( controller, location, index )
	if (location == MP_MOTD or location == CP_MOTD) and (index == nil or index == 0) then
		return {
			message = make_message( location )
		}
	end
	local ok, result = pcall( orig_get_content, controller, location, index )
	if ok then
		return result
	end
	return nil
end

-- apertura automática al entrar al menú (CRM.TryForceOpenCRMWindow); popup=0 la desactiva
local function wrap_force_open()
	if not CRM or not CRM.TryForceOpenCRMWindow or CRM.__iw7s_motd then
		return
	end
	CRM.__iw7s_motd = true
	local orig_force_open = CRM.TryForceOpenCRMWindow
	CRM.TryForceOpenCRMWindow = function ( controller, location, index )
		if (location == MP_MOTD or location == CP_MOTD) and cfg.popup ~= "1" then
			return false
		end
		return orig_force_open( controller, location, index )
	end
end
wrap_force_open()

-- los comandos de lectura/clic del CRM esperan datos del servidor que no existen
if Engine.Exec then
	local orig_exec = Engine.Exec
	Engine.Exec = function ( cmd, ... )
		if type( cmd ) == "string" then
			wrap_force_open()
			if cmd:find( "^markCRMMessageRead" ) or cmd:find( "^clickCRMMessageAction" ) then
				return
			end
		end
		return orig_exec( cmd, ... )
	end
end
