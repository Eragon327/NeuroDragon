#include "neurodragon/utils/Utils.h"

#include "neurodragon/Log.h"
#include "neurodragon/Test.h"

#include "ll/api/i18n/I18n.h"

#include <system_error>

namespace neurodragon::log {

void write(int level, std::string_view message) {
    auto& log = utils::logger();
    switch (level) {
    case 3:
        // Diagnostics never reach a release build (src/neurodragon/Test.h). The
        // call sites are already compiled out; this is the choke point that keeps
        // an accidental level-3 write out of a shipped log too.
#ifdef TEST
        log.debug("{}", message);
#endif
        break;
    case 2:
        log.error("{}", message);
        break;
    case 1:
        log.warn("{}", message);
        break;
    default:
        log.info("{}", message);
        break;
    }
}

void writeKeyArgs(int level, std::string_view key, fmt::format_args args) {
    std::string_view const pattern = ll::i18n::getInstance().get(key, {});
    if (pattern.empty()) {
        // No translation: make that obvious instead of printing nothing.
        write(level, fmt::format("missing i18n: {}", key));
        return;
    }
    write(level, fmt::vformat(pattern, args));
}

std::string text(std::string_view key) {
    std::string_view const pattern = ll::i18n::getInstance().get(key, {});
    return pattern.empty() ? std::string{key} : std::string{pattern};
}

} // namespace neurodragon::log

using namespace ll::i18n_literals;

namespace neurodragon::utils {

ll::io::Logger& logger() { return ll::mod::NativeMod::current()->getLogger(); }

// Directory of the running executable, with a trailing separator.
std::wstring applicationDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD const length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    std::size_t const slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    path.resize(slash + 1);
    return path;
}

std::string fileSizeText(std::filesystem::path const& path) {
    std::error_code error;
    auto const      size = std::filesystem::file_size(path, error);
    if (error) {
        return "?";
    }
    return std::to_string(size / (1024 * 1024)) + " MB";
}

bool ensureApplicationModel(ll::mod::NativeMod& mod, wchar_t const* fileName) {
    std::error_code             error;
    auto const                  modDir    = std::filesystem::absolute(mod.getModDir(), error);
    std::filesystem::path const directory = error ? mod.getModDir() : modDir;
    auto const                  source    = directory / fileName;

    std::wstring const appDir = applicationDirectory();
    if (appDir.empty()) {
        log::write(2, "nr.error.noExeDir"_tr());
        return false;
    }
    auto const target = std::filesystem::path(appDir) / fileName;

    if (::GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
        ND_DEBUG("NR model already in place, skipping ({} , {})", target.string(), fileSizeText(target));
        return true;
    }

    if (::GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
        log::write(2, "nr.error.modelMissing"_tr(source.string()));
        return false;
    }

    if (::CreateHardLinkW(target.c_str(), source.c_str(), nullptr)) {
        ND_DEBUG("NR model hard-linked next to the executable: {} -> {}", target.string(), source.string());
        return true;
    }
    DWORD const linkError = ::GetLastError();
    log::write(1, "nr.warn.hardlinkFailed"_tr(linkError, fileSizeText(source)));

    if (::CopyFileW(source.c_str(), target.c_str(), TRUE)) {
        log::write(0, "nr.info.modelCopied"_tr(target.string(), fileSizeText(target)));
        return true;
    }
    DWORD const copyError = ::GetLastError();
    log::write(
        2,
        "nr.error.modelUnwritable"_tr(target.string(), linkError, copyError, source.string())
    );
    return false;
}

} // namespace neurodragon::utils
