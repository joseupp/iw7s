-- Mensaje del día (CRM) sin conexión.
-- Sin servidores, Engine.CRMGetMessageContent no devuelve nada y el widget CRMMain del menú multijugador (imagen
-- pequeña + "pulsa para abrir") se oculta. Aquí se sirven mensajes locales leídos de iw7s/dw/crm_motd.txt.
--
-- Formato del archivo (clave=valor, una por línea; en "long" se puede usar \n para saltos de línea):
--   title=Título
--   short=Texto corto bajo la imagen
--   long=Texto largo de la ventana emergente (sin él, el mensaje no se puede abrir)
--   image=image_motd_mp_dlc4_keyart_v1     (la pequeña es <image>_small si el juego la tiene)
--   layout=1                               (1 = ventana completa, 0 = ventana dividida)
--   popup=1                                (1 = abrir el mensaje principal al entrar al menú cuando cambie)
--   todas=1                                (1 = las imágenes de eventos del modo entran en el sorteo del carrusel)
--   rotar=5                                (cuántos mensajes salen en el carrusel detrás del del día)
--
-- Lo que va antes de cualquier apartado es el mensaje principal de multijugador. [zombis] empieza el mensaje
-- principal de Zombis (mismas claves). Cada [mensaje] añade otro mensaje al carrusel del modo en el que está
-- (multijugador antes de [zombis], Zombis después). El mensaje del día va siempre primero; detrás salen "rotar"
-- mensajes elegidos al azar al iniciar el juego entre los [mensaje] y las imágenes de eventos. El menú los va pasando
-- cada 10 s.
if not Engine.InFrontend() or Engine.CRMGetMessageContent == nil then
	return
end

local file_path = "iw7s/dw/crm_motd.txt"

local MP_MOTD = 1
local MP_MSGS = 2
local CP_MOTD = 11
local CP_MSGS = 12

-- Con más mensajes que esto no caben los puntos de abajo: se ocultan
local MAX_PIPS = 8

local function new_main( image )
	return {
		title = "IW7S",
		short = "Bienvenido a ^3IW7S",
		long = "Bienvenido a ^3IW7S^7.\n\nCliente sin conexión de Infinite Warfare.",
		image = image,
		layout = "1",
		popup = "1",
		todas = "0",
		rotar = "5"
	}
end

-- Imágenes de eventos de cada modo (las de CRM.supportedSmallImages de ui/utils/crmutils; en el juego esa tabla
-- llega vacía, así que van aquí)
local mp_images = {
		"image_motd_mp_22xp_centered",
		"image_motd_mp_2wxp_centered",
		"image_motd_mp_2xk_centered",
		"image_motd_mp_2xp_centered",
		"image_motd_mp_2xp_tokens",
		"image_motd_mp_announcer003",
		"image_motd_mp_announcer004",
		"image_motd_mp_announcer1",
		"image_motd_mp_announcer1_v2",
		"image_motd_mp_bloodanvil_v2",
		"image_motd_mp_bo4_dde",
		"image_motd_mp_bo4_dde_plus",
		"image_motd_mp_bo4_shield",
		"image_motd_mp_carnage2xp",
		"image_motd_mp_cwl_generic",
		"image_motd_mp_cwl_loot",
		"image_motd_mp_cwlpack",
		"image_motd_mp_cysp",
		"image_motd_mp_demolition",
		"image_motd_mp_dlc1_keyart",
		"image_motd_mp_dlc2_keyart",
		"image_motd_mp_dlc2_turista",
		"image_motd_mp_dlc3_bermuda",
		"image_motd_mp_dlc3_ember",
		"image_motd_mp_dlc3_fore",
		"image_motd_mp_dlc3_keyart_v1",
		"image_motd_mp_dlc3_keyart_v2",
		"image_motd_mp_dlc3_permafrost",
		"image_motd_mp_dlc4_altitude_v1",
		"image_motd_mp_dlc4_altitude_v2",
		"image_motd_mp_dlc4_carnage_v1",
		"image_motd_mp_dlc4_carnage_v2",
		"image_motd_mp_dlc4_depot22_v1",
		"image_motd_mp_dlc4_depot22_v2",
		"image_motd_mp_dlc4_heartland_v1",
		"image_motd_mp_dlc4_heartland_v2",
		"image_motd_mp_dlc4_keyart_v1",
		"image_motd_mp_dlc4_keyart_v2",
		"image_motd_mp_dos_generic",
		"image_motd_mp_dos_keyart",
		"image_motd_mp_dos_showcase",
		"image_motd_mp_dropzone",
		"image_motd_mp_epicgungame",
		"image_motd_mp_feast_v1",
		"image_motd_mp_feast_v2",
		"image_motd_mp_ftlrig",
		"image_motd_mp_generic01",
		"image_motd_mp_generic1",
		"image_motd_mp_generic2",
		"image_motd_mp_hack_generic",
		"image_motd_mp_halloween_bossbattles_v1",
		"image_motd_mp_halloween_bossbattles_v2",
		"image_motd_mp_halloween_bossbattles_v3",
		"image_motd_mp_halloween_gesturegame",
		"image_motd_mp_halloween_hacks_w1",
		"image_motd_mp_halloween_hacks_w2",
		"image_motd_mp_halloween_hacks_w3",
		"image_motd_mp_halloween_halloween",
		"image_motd_mp_halloween_quadfeed",
		"image_motd_mp_halloween_triple_play_mp",
		"image_motd_mp_hypertdm",
		"image_motd_mp_luck_v1",
		"image_motd_mp_luck_v2",
		"image_motd_mp_mercrig",
		"image_motd_mp_missionteams_all_v2",
		"image_motd_mp_mkii_v2",
		"image_motd_mp_mlg_gamebattles",
		"image_motd_mp_mtx12_epicweaponkit",
		"image_motd_mp_mtx12_hyperxp",
		"image_motd_mp_mtx12_playercards",
		"image_motd_mp_mtx12_showcase",
		"image_motd_mp_mtx12_taunts",
		"image_motd_mp_mtx12_weapons",
		"image_motd_mp_mwr_keyart",
		"image_motd_mp_mwrkeyart",
		"image_motd_mp_nuke_v1",
		"image_motd_mp_nuke_v2",
		"image_motd_mp_phantomrig",
		"image_motd_mp_quartermaster4",
		"image_motd_mp_quartermaster5",
		"image_motd_mp_reload_v1",
		"image_motd_mp_reload_v2",
		"image_motd_mp_s2_dde",
		"image_motd_mp_s2_generic",
		"image_motd_mp_sd_bundle",
		"image_motd_mp_sd_bundle20_centered",
		"image_motd_mp_sd_bundle25_centered",
		"image_motd_mp_sd_sale25_centered",
		"image_motd_mp_sdsale_centered",
		"image_motd_mp_sdsale_v2",
		"image_motd_mp_seasonpass",
		"image_motd_mp_silent_v1",
		"image_motd_mp_silent_v2",
		"image_motd_mp_sp_v2",
		"image_motd_mp_stats_sdf",
		"image_motd_mp_stats_unsa",
		"image_motd_mp_strykerrig",
		"image_motd_mp_synapticrig",
		"image_motd_mp_tacticaltdm",
		"image_motd_mp_tripleplay",
		"image_motd_mp_vobundle",
		"image_motd_mp_warfighterrig",
		"image_motd_mp_wreck_v1",
		"image_motd_mp_wreck_v2",
}

local cp_images = {
		"image_motd_mp_halloween_triple_play_zm",
		"image_motd_mp_zm",
		"image_motd_cp_dlc3",
		"image_motd_cp_dlc4_keyart_v1",
		"image_motd_cp_dlc4_keyart_v2",
		"image_motd_zm_bombstoppers",
		"image_motd_zm_candymachines",
		"image_motd_zm_fateandfortune",
		"image_motd_zm_portal",
		"image_motd_zm_supplydrop",
		"image_motd_zm_ziskeyart",
}

local modes = {
	mp = {
		main = new_main( "image_motd_main" ),
		extra = {},
		images = mp_images
	},
	cp = {
		main = new_main( "image_motd_main" ),
		extra = {},
		images = cp_images
	}
}

local function read_config()
	local mode = modes.mp
	local current = mode.main
	local cp_todas = nil
	local cp_rotar = nil

	if io and io.fileexists and io.fileexists( file_path ) then
		local ok, data = pcall( io.readfile, file_path )
		if ok and type( data ) == "string" then
			-- quita el BOM UTF-8
			data = data:gsub( "^\239\187\191", "" )
			for line in data:gmatch( "[^\r\n]+" ) do
				if line:match( "^%s*%[zombis%]%s*$" ) then
					mode = modes.cp
					current = mode.main
				elseif line:match( "^%s*%[mensaje%]%s*$" ) then
					current = {
						layout = "1"
					}
					table.insert( mode.extra, current )
				else
					local key, value = line:match( "^%s*([%w_]+)%s*=%s*(.-)%s*$" )
					if key and value then
						current[key] = value:gsub( "\\n", "\n" )
						if key == "todas" and current == modes.cp.main then
							cp_todas = current[key]
						end
						if key == "rotar" and current == modes.cp.main then
							cp_rotar = current[key]
						end
					end
				end
			end
		end
	end

	-- Sin "todas" en [zombis], Zombis usa el de multijugador
	if not cp_todas then
		modes.cp.main.todas = modes.mp.main.todas
	end
	if not cp_rotar then
		modes.cp.main.rotar = modes.mp.main.rotar
	end
end
read_config()

-- Carrusel: entre los [mensaje] y (con todas=1) las imágenes de eventos del modo, se eligen "rotar" al azar la
-- primera vez que se piden los mensajes. Las imágenes sin [mensaje] propio llevan el texto del mensaje del día.
local function fill_all( mode )
	if mode.filled then
		return
	end
	mode.filled = true

	local pool = {}
	local used = {
		[mode.main.image or ""] = true
	}
	for _, msg in ipairs( mode.extra ) do
		table.insert( pool, msg )
		if msg.image then
			used[msg.image] = true
		end
	end

	if mode.main.todas == "1" then
		for _, image in ipairs( mode.images ) do
			if not used[image] then
				table.insert( pool, {
					title = mode.main.title,
					short = mode.main.short,
					long = mode.main.long,
					image = image,
					layout = mode.main.layout or "1"
				} )
			end
		end
	end

	if Engine.GetMilliseconds then
		math.randomseed( Engine.GetMilliseconds() )
	end

	local count = math.min( tonumber( mode.main.rotar ) or 5, #pool )
	mode.extra = {}
	for i = 1, count do
		table.insert( mode.extra, table.remove( pool, math.random( 1, #pool ) ) )
	end
end

local function mode_of( location )
	local mode = nil
	if location == MP_MOTD or location == MP_MSGS then
		mode = modes.mp
	elseif location == CP_MOTD or location == CP_MSGS then
		mode = modes.cp
	end
	if mode then
		fill_all( mode )
	end
	return mode
end

-- checksum numérico del contenido: si cambia el mensaje, la ventana vuelve a abrirse sola una vez
local function checksum( text )
	local h = 5381
	for i = 1, #text do
		h = (h * 33 + text:byte( i )) % 2147483647
	end
	return h
end

local function make_message( msg, location, index )
	local title = msg.title or ""
	local short = msg.short or ""
	local long = msg.long or ""
	local image = msg.image or "image_motd_main"
	return {
		title = title,
		content_short = short,
		content_long = long,
		image = image,
		layout_type = msg.layout or "1",
		location_id = location,
		message_index = index,
		checksum = checksum( title .. short .. long .. image .. index )
	}
end

local orig_get_content = Engine.CRMGetMessageContent
Engine.CRMGetMessageContent = function ( controller, location, index )
	index = index or 0
	local mode = mode_of( location )
	if mode then
		local msg = nil
		if location == MP_MOTD or location == CP_MOTD then
			msg = index == 0 and mode.main or nil
		else
			msg = mode.extra[index + 1]
		end
		if msg then
			return {
				message = make_message( msg, location, index )
			}
		end
		return nil
	end
	local ok, result = pcall( orig_get_content, controller, location, index )
	if ok then
		return result
	end
	return nil
end

if Engine.CRMGetValidMessageCount then
	local orig_get_count = Engine.CRMGetValidMessageCount
	Engine.CRMGetValidMessageCount = function ( controller, location )
		if location == MP_MSGS or location == CP_MSGS then
			return #mode_of( location ).extra
		end
		local ok, result = pcall( orig_get_count, controller, location )
		if ok and type( result ) == "number" then
			return result
		end
		return 0
	end
end

local our_force_open = nil
local our_sources = nil

local function patch_crm()
	if not CRM then
		return
	end

	-- Versión pequeña de cada imagen en el cuadro del menú (la tabla del juego llega vacía)
	if type( CRM.supportedSmallImages ) ~= "table" then
		CRM.supportedSmallImages = {}
	end
	for _, list in ipairs( { mp_images, cp_images } ) do
		for _, image in ipairs( list ) do
			CRM.supportedSmallImages[image .. "_small"] = true
		end
	end

	-- Cada función por separado: CRM puede existir antes de que crmutils las defina (o crmutils las vuelve a definir)
	-- apertura automática al entrar al menú (CRM.TryForceOpenCRMWindow); popup=0 la desactiva
	if CRM.TryForceOpenCRMWindow and CRM.TryForceOpenCRMWindow ~= our_force_open then
		local orig_force_open = CRM.TryForceOpenCRMWindow
		our_force_open = function ( controller, location, index )
			local mode = mode_of( location )
			if (location == MP_MOTD or location == CP_MOTD) and mode and mode.main.popup ~= "1" then
				return false
			end
			return orig_force_open( controller, location, index )
		end
		CRM.TryForceOpenCRMWindow = our_force_open
	end

	-- El original pone hasta 4 mensajes y el del día al final: aquí el del día primero y todos los demás
	if CRM.GetMenuMessageDataSources and CRM.GetMessageDataSources and CRM.GetMenuMessageDataSources ~= our_sources then
		our_sources = function ( path, controller, popup_only )
			local list = {}
			local function add( location, index )
				local content = CRM.GetMessageContent( controller, location, index )
				if content and (not popup_only or CRM.DoesMessageOpenPopup( content )) then
					local sources = CRM.GetMessageDataSources( path, controller, location, index )
					if sources then
						sources.index = #list
						table.insert( list, sources )
					end
				end
			end

			fill_all( modes.mp )
			fill_all( modes.cp )
			if Engine.IsCoreMode() then
				add( MP_MOTD, 0 )
				for i = 0, #modes.mp.extra - 1 do
					add( MP_MSGS, i )
				end
			else
				add( CP_MOTD, 0 )
				for i = 0, #modes.cp.extra - 1 do
					add( CP_MSGS, i )
				end
			end
			return list
		end
		CRM.GetMenuMessageDataSources = our_sources
	end
end
patch_crm()

-- Con muchos mensajes, los puntos de abajo no caben: se ocultan
local function wrap_main( builder )
	return function ( ... )
		patch_crm()
		local widget = builder( ... )
		local mode = Engine.IsCoreMode() and modes.mp or modes.cp
		fill_all( mode )
		if widget and widget.Pips and #mode.extra + 1 > MAX_PIPS then
			widget.Pips:SetAlpha( 0, 0 )
			widget.Pips.SetAlpha = function () end
		end
		return widget
	end
end

if MenuBuilder.m_types["CRMMain"] then
	MenuBuilder.m_types["CRMMain"] = wrap_main( MenuBuilder.m_types["CRMMain"] )
else
	local register_type = MenuBuilder.registerType
	MenuBuilder.registerType = function ( name, builder, ... )
		if name == "CRMMain" then
			builder = wrap_main( builder )
		end
		return register_type( name, builder, ... )
	end
end

-- los comandos de lectura/clic del CRM esperan datos del servidor que no existen
if Engine.Exec then
	local orig_exec = Engine.Exec
	Engine.Exec = function ( cmd, ... )
		if type( cmd ) == "string" then
			patch_crm()
			if cmd:find( "^markCRMMessageRead" ) or cmd:find( "^clickCRMMessageAction" ) then
				return
			end
		end
		return orig_exec( cmd, ... )
	end
end
