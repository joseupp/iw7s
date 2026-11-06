-- Número de compilación también durante la partida, como en consola.
-- LUI.UIRoot.init (ui/luiroot.lua) solo añade "BuildNumberText" en el menú principal;
-- en partida se añade aquí la primera vez que se actualiza la raíz.
local orig_HandleErrors = LUI.UIRoot.HandleErrors
LUI.UIRoot.HandleErrors = function(root, ...)
	if not root.ingameBuildNumber and not Engine.InFrontend() then
		local text = MenuBuilder.BuildRegisteredType("BuildNumberText")
		text.id = "ingameBuildNumberText"
		text:animateToState("visible")
		root:addElement(text)
		root.ingameBuildNumber = text
	end

	return orig_HandleErrors(root, ...)
end
