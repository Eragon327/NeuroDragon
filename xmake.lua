add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

option("target_type")
    set_default("client")
    set_showmenu(true)
    set_values("client")
option_end()

-- Diagnostics build (src/neurodragon/Test.h): defines TEST, which compiles in
-- the BMP colour dumps, the NGX parameter trace (nr.paramLog), the resource/eval
-- diagnostics and every debug-level line. Off by default, so a release DLL
-- contains none of that code.
option("test")
    set_default(false)
    set_showmenu(true)
    set_description("Compile in diagnostic dumps and debug logging (TEST)")
option_end()

-- add_requires("levilamina x.x.x") for a specific version
-- add_requires("levilamina develop") to use develop version
-- please note that you should add bdslibrary yourself if using dev version
--
-- Pinned on purpose (2026-10-29, docs/specs/vv.md §17.2): the three test instances run
-- the 26.51.7 loader, and this mod compiles against that package's headers. An
-- unpinned requirement may resolve to a newer 26.51.x on the next reconfigure, which
-- would compile against one ABI and load under another. Bump the headers and the
-- instance loaders together, never one of them.
add_requires("levilamina 26.51.7", {configs = {target_type = get_config("target_type")}})

add_requires("levibuildscript")

-- UI layer: Dear ImGui (Win32 + DX11 backend) + MinHook - the same stack Mangrove
-- and LHolo already ship for their overlays. A client mod can only draw a resident
-- panel above the game's HUD by hooking DXGI Present and rendering on the
-- D3D11On12-wrapped back buffer, which is what those two projects prove out.
add_requires("imgui v1.91.9", {configs = {shared = false, win32 = true, dx11 = true, no_demo_windows = true}})
add_requires("minhook", {configs = {shared = false}})

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

target("NeuroDragon") -- Change this to your mod name.
    add_rules("@levibuildscript/linkrule")
    if is_plat("windows") then
        add_defines("NOMINMAX", "UNICODE", "_HAS_CXX23=1")
        if get_config("test") then
            add_defines("TEST")
        end
        set_exceptions("none") -- To avoid conflicts with /EHa.
        add_cxflags( "/EHa", "/utf-8", "/W4", "/w44265", "/w44289", "/w44296", "/w45263", "/w44738", "/w45204")
        add_cxflags(
            "/EHs",
            "-Wno-microsoft-cast",
            "-Wno-invalid-offsetof",
            "-Wno-c++2b-extensions",
            "-Wno-microsoft-include",
            "-Wno-overloaded-virtual",
            "-Wno-ignored-qualifiers",
            "-Wno-missing-field-initializers",
            "-Wno-potentially-evaluated-expression",
            "-Wno-pragma-system-header-outside-header",
            {tools = {"clang_cl"}}
        )
        set_toolchains("clang-cl")
    end
    add_packages("levilamina", "imgui", "minhook")
    -- windowscodecs: PNG encoder for the screenshot key (ui/Screenshot.cpp);
    -- ole32: CoInitializeEx / CoCreateInstance for the WIC factory.
    add_syslinks("user32", "d3d11", "d3d12", "dxgi", "windowscodecs", "ole32")
    set_kind("shared")
    set_languages("c++20")
    set_symbols("debug")
    add_headerfiles("src/**.h")
    add_files("src/**.cpp")
    add_includedirs("src")
    if is_config("target_type", "server") then
    --  add_includedirs("src-server")
    --  add_files("src-server/**.cpp")
    else
    --  add_includedirs("src-client")
    --  add_files("src-client/**.cpp")
    end
    -- Pre-build gate (scripts/agentflow/flow.py, protocol in AGENTS.md).
    -- It fails the build on the mistakes that are cheaper to catch here than in a
    -- game session: an NGX parameter name the runtime does not know, a shader that
    -- does not compile, a DLSS evaluate that stopped setting a required parameter.
    -- There is deliberately no switch to skip it - a build that cannot pass the
    -- gate must not be handed to a human as "try this".
    before_build(function (target)
        import("lib.detect.find_program")
        local python = find_program("python") or find_program("python3")
        if not python then
            raise("agentflow: python is required for the pre-build gate (see AGENTS.md)")
        end
        os.execv(python, {
            path.join(os.projectdir(), "scripts", "agentflow", "flow.py"),
            "gate", "build",
        })
    end)
    -- Replaces @levibuildscript/modpacker: that rule only copies the DLL, its
    -- PDB and manifest.json, while this mod also ships lang/ and the NR model.
    after_build(function (target)
        local mod_packer = import("scripts.after_build")

        local tag = os.iorun("git describe --tags --abbrev=0 --always")
        local major, minor, patch = tag:match("v(%d+)%.(%d+)%.(%d+)")
        if not major then
            print("Failed to parse version tag, using 0.0.0")
            major, minor, patch = 0, 0, 0
        end

        mod_packer.pack_mod(target, {
            modName = target:name(),
            modFile = path.filename(target:targetfile()),
            modVersion = major .. "." .. minor .. "." .. patch,
            modPlatform = get_config("target_type"),
        })
    end)
