gitVersioningCommand = "git describe --tags --dirty --always"
gitCurrentBranchCommand = "git symbolic-ref -q --short HEAD"

-- Quote the given string input as a C string
function cstrquote(value)
    if value == nil then
        return "\"\""
    end
    result = value:gsub("\\", "\\\\")
    result = result:gsub("\"", "\\\"")
    result = result:gsub("\n", "\\n")
    result = result:gsub("\t", "\\t")
    result = result:gsub("\r", "\\r")
    result = result:gsub("\a", "\\a")
    result = result:gsub("\b", "\\b")
    result = "\"" .. result .. "\""
    return result
end

-- Converts tags in "vX.X.X" format and given revision number Y to an array of numbers {X,X,X,Y}.
-- In the case where the format does not work fall back to padding with zeroes and just ending with the revision number.
-- partscount can be either 3 or 4.
function vertonumarr(value, vernumber, partscount)
    vernum = {}
    for num in string.gmatch(value or "", "%d+") do
        if #vernum < 3 then
            table.insert(vernum, tonumber(num))
        end
    end
    while #vernum < 3 do
        table.insert(vernum, 0)
    end
    if #vernum < partscount then
        table.insert(vernum, tonumber(vernumber))
    end
    return vernum
end

-- Número de revisión propio (r1000, r1001...): se lee de revision.txt y se sube a mano en cada cambio.
-- Las letras detrás del número son la fase (b = beta): "1000b" -> r1000b
function readrevision()
    local file = io.open(path.join(_MAIN_SCRIPT_DIR, "revision.txt"), "r")
    if file == nil then
        return 1000, ""
    end
    local content = file:read("*a") or ""
    file:close()
    local number, suffix = string.match(content, "(%d+)(%a*)")
    return tonumber(number) or 1000, suffix or ""
end

dependencies = {
    basePath = "./deps"
}

function dependencies.load()
    dir = path.join(dependencies.basePath, "premake/*.lua")
    deps = os.matchfiles(dir)

    for i, dep in pairs(deps) do
        dep = dep:gsub(".lua", "")
        require(dep)
    end
end

function dependencies.imports()
    for i, proj in pairs(dependencies) do
        if type(i) == 'number' then
            proj.import()
        end
    end
end

function dependencies.projects()
    for i, proj in pairs(dependencies) do
        if type(i) == 'number' then
            proj.project()
        end
    end
end

newoption {
    trigger = "copy-to",
    description = "Optional, copy the EXE to a custom folder after build, define the path here if wanted.",
    value = "PATH"
}

newoption {
    trigger = "steam-overlay",
    description = "Enable the Steam Overlay."
}

newoption {
    trigger = "inject-host-as-lib",
    description = "Decide whether to load the game as lib or to inject it"
}

newaction {
    trigger = "version",
    description = "Returns the version string for the current commit of the source code.",
    onWorkspace = function(wks)
        -- get current version via git
        local proc = assert(io.popen(gitVersioningCommand, "r"))
        local gitDescribeOutput = assert(proc:read('*a')):gsub("%s+", "")
        proc:close()
        local version = gitDescribeOutput

        proc = assert(io.popen(gitCurrentBranchCommand, "r"))
        local gitCurrentBranchOutput = assert(proc:read('*a')):gsub("%s+", "")
        local gitCurrentBranchSuccess = proc:close()
        if gitCurrentBranchSuccess then
            -- We got a branch name, check if it is a feature branch
            if gitCurrentBranchOutput ~= "develop" and gitCurrentBranchOutput ~= "master" then
                version = version .. "-" .. gitCurrentBranchOutput
            end
        end

        print(version)
        os.exit(0)
    end
}

newaction {
    trigger = "generate-buildinfo",
    description = "Sets up build information file like version.h.",
    onWorkspace = function(wks)
        -- get old version number from version.hpp if any
        local oldVersion = "(none)"
        local oldRevision = "(none)"
        local oldVersionHeader = io.open(wks.location .. "/src/version.h", "r")
        if oldVersionHeader ~= nil then
            local oldVersionHeaderContent = assert(oldVersionHeader:read('*l'))
            while oldVersionHeaderContent do
                m = string.match(oldVersionHeaderContent, "#define GIT_DESCRIBE (.+)%s*$")
                if m ~= nil then
                    oldVersion = m
                end

                m = string.match(oldVersionHeaderContent, "#define REVISION_STRING \"(.-)\"")
                if m ~= nil then
                    oldRevision = m
                end

                oldVersionHeaderContent = oldVersionHeader:read('*l')
            end
        end

        -- get current version via git
        local proc = assert(io.popen(gitVersioningCommand, "r"))
        local gitDescribeOutput = assert(proc:read('*a')):gsub("%s+", "")
        proc:close()

        -- generate version.hpp with a revision number if not equal
        gitDescribeOutputQuoted = cstrquote(gitDescribeOutput)
        local revision, revisionSuffix = readrevision()
        local revisionString = "r" .. revision .. revisionSuffix
        if oldVersion ~= gitDescribeOutputQuoted or oldRevision ~= revisionString then
            -- get current git hash
            local proc = assert(io.popen("git rev-parse HEAD", "r"))
            local gitCommitHash = assert(proc:read('*a')):gsub("%s+", "")
            proc:close()

            -- get whether this is a clean revision (no uncommitted changes)
            proc = assert(io.popen("git status --porcelain", "r"))
            local revDirty = (assert(proc:read('*a')) ~= "")
            if revDirty then
                revDirty = 1
            else
                revDirty = 0
            end
            proc:close()

            -- get current tag name
            proc = assert(io.popen("git describe --tags --abbrev=0"))
            local tagName = proc:read('*l')

            -- get current branch name
            proc = assert(io.popen("git branch --show-current"))
            local branchName = proc:read('*l')

            -- branch for ci
            if branchName == nil or branchName == '' then
                proc = assert(io.popen("git show -s --pretty=%d HEAD"))
                local branchInfo = proc:read('*l')
                m = string.match(branchInfo, ".+,.+, ([^)]+)")
                if m ~= nil then
                    branchName = m
                end
            end

            if branchName == nil then
                branchName = "develop"
            end

            print("Detected branch: " .. branchName)

            -- get revision number via git
            local proc = assert(io.popen("git rev-list --count HEAD", "r"))
            local revNumber = assert(proc:read('*a')):gsub("%s+", "")

            print("Update " .. oldVersion .. " -> " .. gitDescribeOutputQuoted .. " (" .. revisionString .. ")")

            -- write version header
            local versionHeader = assert(io.open(wks.location .. "/src/version.h", "w"))
            versionHeader:write("/*\n")
            versionHeader:write(" * Automatically generated by premake5.\n")
            versionHeader:write(" * Do not touch!\n")
            versionHeader:write(" */\n")
            versionHeader:write("\n")
            versionHeader:write("#define GIT_DESCRIBE " .. gitDescribeOutputQuoted .. "\n")
            versionHeader:write("#define GIT_DIRTY " .. revDirty .. "\n")
            versionHeader:write("#define GIT_HASH " .. cstrquote(gitCommitHash) .. "\n")
            versionHeader:write("#define GIT_TAG " .. cstrquote(tagName) .. "\n")
            versionHeader:write("#define GIT_BRANCH " .. cstrquote(branchName) .. "\n")
            versionHeader:write("\n")
            versionHeader:write("// Revision number (revision.txt)\n")
            versionHeader:write("#define REVISION " .. revision .. "\n")
            versionHeader:write("#define REVISION_STRING " .. cstrquote(revisionString) .. "\n")
            versionHeader:write("\n")
            versionHeader:write("// Version transformed for RC files\n")
            versionHeader:write("#define VERSION_PRODUCT_RC " .. revision .. ",0,0,0\n")
            versionHeader:write("#define VERSION_PRODUCT REVISION_STRING\n")
            versionHeader:write("#define VERSION_FILE_RC " .. revision .. ",0,0,0\n")
            versionHeader:write("#define VERSION_FILE REVISION_STRING\n")
            versionHeader:write("\n")
            versionHeader:write("// Alias definitions\n")
            versionHeader:write("#define VERSION REVISION_STRING\n")
            versionHeader:write("#define SHORTVERSION REVISION_STRING\n")
            versionHeader:close()
            local versionHeader = assert(io.open(wks.location .. "/src/version.hpp", "w"))
            versionHeader:write("/*\n")
            versionHeader:write(" * Automatically generated by premake5.\n")
            versionHeader:write(" * Do not touch!\n")
            versionHeader:write(" *\n")
            versionHeader:write(" * This file exists for reasons of complying with our coding standards.\n")
            versionHeader:write(" *\n")
            versionHeader:write(
                " * The Resource Compiler will ignore any content from C++ header files if they're not from STDInclude.hpp.\n")
            versionHeader:write(" * That's the reason why we now place all version info in version.h instead.\n")
            versionHeader:write(" */\n")
            versionHeader:write("\n")
            versionHeader:write("#include \".\\version.h\"\n")
            versionHeader:close()
        end
    end
}

dependencies.load()

workspace "iw7s"
startproject "client"
location "./build"
objdir "%{wks.location}/obj"
targetdir "%{wks.location}/bin/%{cfg.platform}/%{cfg.buildcfg}"

configurations {"Debug", "Release"}

language "C++"
cppdialect "C++20"

architecture "x86_64"
platforms "x64"

systemversion "latest"
symbols "On"
staticruntime "On"
editandcontinue "Off"
warnings "Extra"
characterset "ASCII"

defines {'BUILD_NAME="IW7"'}

if _OPTIONS["steam-overlay"] then
    defines "LOAD_STEAM_OVERLAY"
end

if os.getenv("CI") then
    defines {"CI"}
end

flags {"NoIncrementalLink", "NoMinimalRebuild", "MultiProcessorCompile", "No64BitChecks"}

filter "platforms:x64"
defines {"_WINDOWS", "WIN32"}
filter {}

filter "configurations:Release"
optimize "Size"
buildoptions {"/GL"}
linkoptions {"/IGNORE:4702", "/LTCG"}
defines {"NDEBUG"}
flags {"FatalCompileWarnings"}
filter {}

filter "configurations:Debug"
optimize "Debug"
buildoptions {"/bigobj"}
defines {"DEBUG", "_DEBUG"}
filter {}

project "common"
kind "StaticLib"
language "C++"

files {"./src/common/**.hpp", "./src/common/**.cpp"}

includedirs {"./src/common", "%{prj.location}/src"}

resincludedirs {"$(ProjectDir)src"}

dependencies.imports()

project "client"
-- Aplicación de ventana: sin la ventana de consola de Windows al arrancar (cliente offline, sin servidor dedicado)
kind "WindowedApp"
entrypoint "mainCRTStartup"
language "C++"

targetname "iw7s"

pchheader "std_include.hpp"
pchsource "src/client/std_include.cpp"

linkoptions {"/IGNORE:4254", "/DYNAMICBASE:NO", "/SAFESEH:NO", "/LARGEADDRESSAWARE", "/LAST:.main", "/PDBCompress"}

files {"./src/client/**.rc", "./src/client/**.hpp", "./src/client/**.cpp", "./src/client/resources/**.*"}

includedirs {"./src/client", "./src/common", "%{prj.location}/src"}

resincludedirs {"$(ProjectDir)src"}

dependson {"tlsdll"}

links {"common"}

prebuildcommands {"pushd %{_MAIN_SCRIPT_DIR}", "tools\\premake5 generate-buildinfo", "popd"}

local COMPUTER_NAME = os.getenv('COMPUTERNAME')
if COMPUTER_NAME == "JOEL-PC" then
    debugdir "D:\\Games\\PC\\IW7"
    debugcommand "D:\\Games\\PC\\IW7\\$(TargetName)$(TargetExt)"
    postbuildcommands {"copy /y \"$(OutDir)$(TargetName)$(TargetExt)\" \"D:\\Games\\PC\\IW7\\$(TargetName)$(TargetExt)\""}
elseif COMPUTER_NAME == "DESKTOP-P7PCR6I" or COMPUTER_NAME == "mikey" then
    debugdir "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Call of Duty - Infinite Warfare"
    debugcommand "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Call of Duty - Infinite Warfare\\$(TargetName)$(TargetExt)"
    postbuildcommands {"copy /y \"$(OutDir)$(TargetName)$(TargetExt)\" \"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Call of Duty - Infinite Warfare\\$(TargetName)$(TargetExt)\""}
end

if _OPTIONS["copy-to"] then
    postbuildcommands {"copy /y \"$(TargetPath)\" \"" .. _OPTIONS["copy-to"] .. "\""}
end

if os.getenv("AURORAIW7_GAME_PATH") then
    debugdir "$(AURORAIW7_GAME_PATH)"
    debugcommand "$(AURORAIW7_GAME_PATH)\\$(TargetName)$(TargetExt)"
    postbuildcommands {"echo Copying to Aurora iw7s game path...",
                       "copy /y \"$(OutDir)$(TargetName)$(TargetExt)\" \"$(AURORAIW7_GAME_PATH)\\$(TargetName)$(TargetExt)\""}
end

if _OPTIONS["debug-dir"] then
    debugdir(_OPTIONS["debug-dir"])
end

dependencies.imports()

project "tlsdll"
kind "SharedLib"
language "C++"

files {"./src/tlsdll/**.rc", "./src/tlsdll/**.hpp", "./src/tlsdll/**.cpp", "./src/tlsdll/resources/**.*"}

includedirs {"./src/tlsdll", "%{prj.location}/src"}

links {"common"}

resincludedirs {"$(ProjectDir)src"}

-- Herramienta: PNG -> DDS con el formato de las texturas de IW7 (para iw7s\images, images.cpp)
project "png2dds"
kind "ConsoleApp"
language "C++"

files {"./src/png2dds/**.cpp"}

directxtex.import()

group "Dependencies"
dependencies.projects()
