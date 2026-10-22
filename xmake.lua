add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

option("target_type")
    set_default("client")
    set_showmenu(true)
    set_values("client")
option_end()

-- add_requires("levilamina x.x.x") for a specific version
-- add_requires("levilamina develop") to use develop version
-- please note that you should add bdslibrary yourself if using dev version
add_requires("levilamina", {configs = {target_type = get_config("target_type")}})

add_requires("levibuildscript")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

target("NeuroDragon") -- Change this to your mod name.
    add_rules("@levibuildscript/linkrule")
    if is_plat("windows") then
        add_defines("NOMINMAX", "UNICODE", "_HAS_CXX23=1")
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
    add_packages("levilamina")
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
