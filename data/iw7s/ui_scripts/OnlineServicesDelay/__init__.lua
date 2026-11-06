-- Ventana "Conectando con los servicios en línea" al pulsar Multijugador o Zombis.
-- Demonware ya está conectado cuando termina el vídeo, así que el fence "onlineServices"
-- pasa al instante y la ventana no se ve. La primera vez de cada sesión, el estado de
-- conexión (Fences.OCDSGetState) se da como "descargando" durante unos segundos para que
-- el juego muestre su ventana original; mientras tanto la ventana lleva la barra de
-- progreso (la de las descargas de parches) que se llena en ese tiempo.

local delay_ms = 1300

if not Fences or not Fences.OCDSGetState then
	return
end

local start_time = nil
-- Solo la primera vez de la sesión: el LUI se reinicia al volver de cada partida, así que se guarda en un dvar
-- (lobby_bots.cpp). Si no, la ventana salía también al volver de la partida y tapaba la pantalla de postpartida.
local done_dvar = "ui_onlineServicesDelayDone"
local done = Engine.GetDvarBool( done_dvar ) == true

local function faking()
	return start_time ~= nil and not done
end

local function progress()
	if not start_time then
		return 0
	end
	return math.min(1, (Engine.GetMilliseconds() - start_time) / delay_ms)
end

-- la ventana de conexión con barra de progreso
local wrapped = false
local function wrap_connecting_popup()
	if wrapped or not MenuBuilder or not MenuBuilder.m_types then
		return
	end

	local orig = MenuBuilder.m_types.live_dialog_text_box_with_cancel
	if not orig or not MenuBuilder.m_types.FenceDialogPopupWithProgress then
		return
	end
	wrapped = true

	MenuBuilder.m_types.live_dialog_text_box_with_cancel = function(menu, props, ...)
		if not faking() then
			return orig(menu, props, ...)
		end

		props = props or {}
		local popup = MenuBuilder.BuildRegisteredType("FenceDialogPopupWithProgress", {
			message = props.message or "",
			controllerIndex = props.controllerIndex or 0,
			onCancel = props.cancel_func
		})
		popup:SetProgress(progress())

		local function tick()
			local timer = popup:Wait(50)
			timer.onComplete = function()
				popup:SetProgress(progress())
				if progress() < 1 then
					tick()
				end
			end
		end
		tick()

		return popup
	end
end

local orig_OCDSGetState = Fences.OCDSGetState
Fences.OCDSGetState = function(...)
	local state = orig_OCDSGetState(...)
	if done or not Engine.InFrontend() then
		return state
	end

	local now = Engine.GetMilliseconds()
	if not start_time then
		start_time = now
		wrap_connecting_popup()
	end

	if now - start_time >= delay_ms + 300 then -- un momento con la barra llena
		done = true
		Engine.SetDvarBool( done_dvar, true )
		return state
	end

	local codes = FenceOnlineServices and FenceOnlineServices.CODE_STATE
	return codes and codes.downloading or 2
end
