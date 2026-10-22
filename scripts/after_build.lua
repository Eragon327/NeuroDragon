-- Package layout for bin/<modName>/.
--
-- Replaces @levibuildscript/modpacker, which only copies the DLL, its PDB and
-- manifest.json. This mod also ships lang/*.json - the catalogue ll::i18n loads
-- from <mod>/lang.
--
-- The 266 MB NR model (nvngx_dlssnr.dll) is deliberately NOT packaged: it is
-- dropped into the mod folder by hand. utils::ensureApplicationModel links it
-- next to the executable at load time, which is where the NGX core looks.

function string_formatter(str, variables)
    return str:gsub("%${(.-)}", function(var)
        return variables[var] or "${" .. var .. "}"
    end)
end

function pack_mod(target, mod_define)
    import("lib.detect.find_file")

    local manifest_path = find_file("manifest.json", os.projectdir())
    if not manifest_path then
        cprint("${bright yellow}warn: ${reset}not found manifest.json in root dir!")
        return
    end

    local outdir = path.join(os.projectdir(), "bin", mod_define.modName)
    os.mkdir(outdir)

    -- engine output
    local targetfile = target:targetfile()
    os.cp(targetfile, path.join(outdir, mod_define.modFile))
    local pdbfile = path.join(path.directory(targetfile), path.basename(targetfile) .. ".pdb")
    if os.isfile(pdbfile) then
        os.cp(pdbfile, path.join(outdir, path.basename(mod_define.modFile) .. ".pdb"))
    end

    -- manifest.json (${modName} / ${modFile} / ${modVersion} / ${modPlatform})
    -- NB: string_formatter returns (text, replacedCount); assigning first keeps
    -- the count from being passed to io.writefile as its third argument.
    local manifest  = io.readfile(manifest_path)
    local formatted = string_formatter(manifest, mod_define)
    io.writefile(path.join(outdir, "manifest.json"), formatted)

    -- i18n catalogue; <mod>/lang is where ll::i18n looks
    local langsrc = path.join(os.projectdir(), "src", "lang")
    local langdst = path.join(outdir, "lang")
    os.rm(langdst)
    if os.isdir(langsrc) then
        os.cp(langsrc, langdst)
        local count = #os.files(path.join(langdst, "*.json"))
        cprint("${bright green}[mod Packer]: ${reset}lang/ (%d files)", count)
    else
        cprint("${bright yellow}warn: ${reset}src/lang is missing, the mod will log i18n keys")
    end

    cprint("${bright green}[mod Packer]: ${reset}mod already generated to " .. outdir)
end

return {
    pack_mod = pack_mod,
    string_formatter = string_formatter,
}
