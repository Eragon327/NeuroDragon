#include "neurodragon/utils/Utils.h"

#include "neurodragon/Log.h"
#include "neurodragon/Test.h"

#include "ll/api/i18n/I18n.h"

#include <algorithm>
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

std::string textArgs(std::string_view key, fmt::format_args args) {
    std::string_view const pattern = ll::i18n::getInstance().get(key, {});
    // A missing key stays visible as the key itself, like text() above: the player
    // then reads "ui.toast.shotSaved" in chat instead of an empty line, which is a
    // catalogue bug that reports itself.
    return pattern.empty() ? std::string{key} : fmt::vformat(pattern, args);
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

namespace {
// The one file name the NGX core looks for next to the executable (docs/D §6.17):
// whatever build the user picks is materialised under this name.
constexpr wchar_t const* kTargetModelName = L"nvngx_dlssnr.dll";
} // namespace

std::vector<std::string> listNrModelFiles() {
    std::vector<std::string>    out;
    auto const                  current = ll::mod::NativeMod::current();
    if (!current) {
        return out;
    }
    std::error_code error;
    for (auto const& entry : std::filesystem::directory_iterator(current->getModDir(), error)) {
        if (error || !entry.is_regular_file(error)) {
            continue;
        }
        auto const name = entry.path().filename().wstring();
        if (name.rfind(L"nvngx_dlssnr", 0) != 0 || entry.path().extension() != L".dll") {
            continue;
        }
        // The canonical name is the picker's "default" entry already; listing it twice
        // would just be a duplicate row.
        if (name == kTargetModelName) {
            continue;
        }
        int const needed = ::WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(needed > 1 ? static_cast<std::size_t>(needed - 1) : 0, '\0');
        if (needed > 1) {
            ::WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, utf8.data(), needed, nullptr, nullptr);
        }
        out.push_back(std::move(utf8));
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool ensureApplicationModel(
    ll::mod::NativeMod&          mod,
    std::filesystem::path const& sourceName,
    bool                         replaceExisting
) {
    std::error_code             error;
    auto const                  modDir    = std::filesystem::absolute(mod.getModDir(), error);
    std::filesystem::path const directory = error ? mod.getModDir() : modDir;
    auto const                  source    = directory / sourceName;

    std::wstring const appDir = applicationDirectory();
    if (appDir.empty()) {
        log::write(2, "nr.error.noExeDir"_tr());
        return false;
    }
    auto const target = std::filesystem::path(appDir) / kTargetModelName;

    if (::GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!replaceExisting) {
            ND_DEBUG("NR model already in place, skipping ({} , {})", target.string(), fileSizeText(target));
            return true;
        }
        // The user picked another build in the panel: the pick wins over whatever is
        // already materialised. If the file cannot be removed, the existing one stays
        // in use instead of failing the whole mod.
        if (!::DeleteFileW(target.c_str())) {
            log::write(1, "nr.warn.modelReplaceFailed"_tr(::GetLastError(), target.string()));
            return true;
        }
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
