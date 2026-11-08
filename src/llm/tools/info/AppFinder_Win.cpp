#include "AppFinder_Internal.hpp"

#if defined(_WIN32)

#include <windows.h>
#include <appmodel.h>
#include <propidl.h>
#include <propkey.h>
#include <shlobj.h>
#include <objbase.h>
#include <shobjidl.h>
#include <winioctl.h>

#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>
#include "fs/path_sanitizer.hpp"

#ifndef IO_REPARSE_TAG_APPEXECLINK
#define IO_REPARSE_TAG_APPEXECLINK (0x8000001BL)
#endif

// build_keys is defined in AppFinder.cpp; forward-declare here so the scanner
// functions can call it without a circular include.
namespace media::llm::info {
void build_keys(AppEntry& e);
}

namespace media::llm::info {
namespace {

namespace fs = std::filesystem;

// ─── String conversion ────────────────────────────────────────────────────

static std::string wide_to_utf8(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                        s.data(), len, nullptr, nullptr);
    return s;
}

[[maybe_unused]]
static std::wstring utf8_to_wide(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                  nullptr, 0);
    if (len <= 0) return {};
    std::wstring ws(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        ws.data(), len);
    return ws;
}

// ─── Registry helpers ─────────────────────────────────────────────────────

static std::string reg_read_string(HKEY hkey, const wchar_t* value_name)
{
    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(hkey, value_name, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS)
        return {};
    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return {};
    if (cb < 2) return {};

    std::wstring buf(cb / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(hkey, value_name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf.data()), &cb) != ERROR_SUCCESS)
        return {};
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();

    if (type == REG_EXPAND_SZ) {
        wchar_t expanded[MAX_PATH * 2]{};
        DWORD elen = ExpandEnvironmentStringsW(buf.c_str(), expanded,
                                               static_cast<DWORD>(std::size(expanded)));
        if (elen > 0 && elen < std::size(expanded))
            buf = expanded;
    }
    return wide_to_utf8(buf);
}

static std::string extract_exe_from_string(const std::string& s)
{
    if (s.empty()) return {};
    std::string path = s;
    if (path.front() == '"') {
        auto end = path.find('"', 1);
        if (end != std::string::npos)
            path = path.substr(1, end - 1);
    } else {
        const auto pos = media::strutil::lower_ascii(path).find(".exe");
        if (pos != std::string::npos)
            path = path.substr(0, pos + 4);
    }
    if (!af_is_native_ext(media::path::path_to_utf8(media::path::path_from_utf8(path).extension())))
        return {};
    if (af_is_installer_cache_path(path))
        return {};
    return path;
}

static std::string pick_exe_in_dir(const fs::path& dir, const std::string& display_name)
{
    const std::string want = af_normalize(display_name);
    std::string best;
    int best_score = -1;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file())
            continue;
        if (!af_is_native_ext(media::path::path_to_utf8(entry.path().extension())))
            continue;
        const std::string stem = media::strutil::lower_ascii(media::path::path_to_utf8(entry.path().stem()));
        if (af_is_uninstaller_name(stem) || af_is_satellite_name(stem))
            continue;
        const std::string path = media::path::path_to_utf8(entry.path());
        if (af_is_installer_cache_path(path))
            continue;
        const std::string stem_n = af_normalize(stem);
        int score = 10;
        if (stem_n == want)
            score = 1000;
        else if (af_abbrev_match(stem_n, want))
            score = 800;
        else if (!stem_n.empty() && (want.find(stem_n) != std::string::npos
                                     || stem_n.find(want) != std::string::npos))
            score = 600;
        if (score > best_score) {
            best_score = score;
            best = path;
        }
    }
    return best;
}

// ─── §1. Registry Uninstall entries ──────────────────────────────────────

static void scan_registry_uninstall(std::vector<AppEntry>& out)
{
    static const struct { HKEY root; const wchar_t* path; } k_keys[] = {
        {HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
        {HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
        {HKEY_CURRENT_USER,  L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
    };

    for (const auto& [root, path] : k_keys) {
        HKEY hparent = nullptr;
        if (RegOpenKeyExW(root, path, 0, KEY_READ, &hparent) != ERROR_SUCCESS)
            continue;

        wchar_t subkey_name[512]{};
        for (DWORD idx = 0; ; ++idx) {
            DWORD name_len = static_cast<DWORD>(std::size(subkey_name));
            if (RegEnumKeyExW(hparent, idx, subkey_name, &name_len,
                              nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;

            HKEY hsub = nullptr;
            if (RegOpenKeyExW(hparent, subkey_name, 0, KEY_READ, &hsub) != ERROR_SUCCESS)
                continue;

            std::string display_name = reg_read_string(hsub, L"DisplayName");
            if (display_name.empty()) { RegCloseKey(hsub); continue; }

            // Skip Windows updates (KB patches)
            if (display_name.find("KB") != std::string::npos) {
                auto kb = display_name.find("KB");
                if (kb + 2 < display_name.size() &&
                    std::isdigit(static_cast<unsigned char>(display_name[kb + 2])))
                {
                    RegCloseKey(hsub); continue;
                }
            }

            std::string exe_path;
            std::string install_loc   = reg_read_string(hsub, L"InstallLocation");
            std::string display_icon  = reg_read_string(hsub, L"DisplayIcon");
            std::string uninstall_str = reg_read_string(hsub, L"UninstallString");

            if (af_is_installer_cache_path(install_loc))
                install_loc.clear();

            // Prefer DisplayIcon only when it is a real product exe, not the
            // MSI icon cache under Windows\Installer.
            if (!display_icon.empty()) {
                auto comma = display_icon.find(',');
                if (comma != std::string::npos)
                    display_icon = display_icon.substr(0, comma);
                std::string candidate = extract_exe_from_string(display_icon);
                if (!candidate.empty() && af_exe_exists(candidate))
                    exe_path = candidate;
            }

            if (!install_loc.empty()) {
                std::error_code ec;
                if (fs::is_directory(media::path::path_from_utf8(install_loc), ec)) {
                    std::string from_dir = pick_exe_in_dir(media::path::path_from_utf8(install_loc), display_name);
                    if (!from_dir.empty())
                        exe_path = std::move(from_dir);
                }
            }

            // Fall back to UninstallString
            if (exe_path.empty() && !uninstall_str.empty()) {
                std::string candidate = extract_exe_from_string(uninstall_str);
                if (!candidate.empty() && af_exe_exists(candidate) &&
                    !af_is_uninstaller_name(media::strutil::lower_ascii(media::path::path_to_utf8(media::path::path_from_utf8(candidate).stem()))))
                    exe_path = candidate;
            }

            RegCloseKey(hsub);
            if (exe_path.empty() || af_is_installer_cache_path(exe_path)) continue;

            AppEntry e;
            e.display_name = display_name;
            e.exe_path     = exe_path;
            e.install_root = install_loc;
            e.source       = "registry_uninstall";
            build_keys(e);
            out.push_back(std::move(e));
        }
        RegCloseKey(hparent);
    }
}

// ─── §2. App Paths ────────────────────────────────────────────────────────

static void scan_app_paths(std::vector<AppEntry>& out)
{
    static const struct { HKEY root; const wchar_t* path; } k_keys[] = {
        {HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths"},
        {HKEY_CURRENT_USER,  L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths"},
    };

    for (const auto& [root, path] : k_keys) {
        HKEY hparent = nullptr;
        if (RegOpenKeyExW(root, path, 0, KEY_READ, &hparent) != ERROR_SUCCESS)
            continue;

        wchar_t subkey_name[512]{};
        for (DWORD idx = 0; ; ++idx) {
            DWORD name_len = static_cast<DWORD>(std::size(subkey_name));
            if (RegEnumKeyExW(hparent, idx, subkey_name, &name_len,
                              nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;

            HKEY hsub = nullptr;
            if (RegOpenKeyExW(hparent, subkey_name, 0, KEY_READ, &hsub) != ERROR_SUCCESS)
                continue;

            std::string exe_path = reg_read_string(hsub, nullptr); // (Default)
            std::string app_path = reg_read_string(hsub, L"Path");
            RegCloseKey(hsub);

            if (exe_path.empty()) continue;
            exe_path = extract_exe_from_string(exe_path);
            if (exe_path.empty() || !af_exe_exists(exe_path)) continue;

            std::string label = wide_to_utf8(subkey_name);
            auto dot = label.rfind('.');
            if (dot != std::string::npos) label = label.substr(0, dot);

            AppEntry e;
            e.display_name  = label;
            e.exe_path      = exe_path;
            e.app_paths_env = app_path;
            e.source        = "app_paths";
            build_keys(e);
            out.push_back(std::move(e));
        }
        RegCloseKey(hparent);
    }
}

// ─── §3. Start Menu shortcuts ─────────────────────────────────────────────

static void scan_start_menu(std::vector<AppEntry>& out)
{
    auto resolve_lnk = [](const fs::path& lnk_path, std::string& target, std::string& work_dir) -> bool {
        IShellLinkW* psl = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IShellLinkW, reinterpret_cast<void**>(&psl));
        if (FAILED(hr) || !psl) return false;

        IPersistFile* ppf = nullptr;
        hr = psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&ppf));
        if (FAILED(hr) || !ppf) { psl->Release(); return false; }

        hr = ppf->Load(lnk_path.c_str(), STGM_READ);
        if (FAILED(hr)) { ppf->Release(); psl->Release(); return false; }

        // Advertised MSI shortcuts need Resolve before GetPath, otherwise the
        // target is the Windows\Installer icon stub (i386_SldWorks.exe, …).
        (void)psl->Resolve(nullptr, SLR_NO_UI | SLR_NOUPDATE | SLR_NOTRACK);

        wchar_t buf[MAX_PATH]{};
        WIN32_FIND_DATAW wfd{};
        hr = psl->GetPath(buf, MAX_PATH, &wfd, 0);
        if (FAILED(hr) || !buf[0])
            hr = psl->GetPath(buf, MAX_PATH, nullptr, SLGP_RAWPATH);
        if (SUCCEEDED(hr) && buf[0])
            target = wide_to_utf8(buf);
        if (af_is_installer_cache_path(target))
            target.clear();

        wchar_t wdir[MAX_PATH]{};
        hr = psl->GetWorkingDirectory(wdir, MAX_PATH);
        if (SUCCEEDED(hr) && wdir[0])
            work_dir = wide_to_utf8(wdir);

        ppf->Release();
        psl->Release();
        return !target.empty();
    };

    const KNOWNFOLDERID folders[] = {FOLDERID_CommonPrograms, FOLDERID_Programs};
    for (const auto& fid : folders) {
        PWSTR path_tmp = nullptr;
        if (FAILED(SHGetKnownFolderPath(fid, 0, nullptr, &path_tmp)))
            continue;
        fs::path root(path_tmp);
        CoTaskMemFree(path_tmp);

        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(
                root, fs::directory_options::skip_permission_denied, ec)) {
            if (!entry.is_regular_file()) continue;
            if (media::strutil::lower_ascii(media::path::path_to_utf8(entry.path().extension())) != ".lnk") continue;

            std::string target, work_dir;
            if (!resolve_lnk(entry.path(), target, work_dir)) continue;
            if (!af_is_native_ext(media::path::path_to_utf8(media::path::path_from_utf8(target).extension()))) continue;
            if (af_is_installer_cache_path(target)) continue;
            if (!af_exe_exists(target)) continue;

            std::string label = media::path::path_to_utf8(entry.path().stem());
            std::string label_lower = media::strutil::lower_ascii(label);
            if (af_is_uninstaller_name(label_lower)) continue;
            if (label_lower.find("readme") != std::string::npos) continue;
            if (label_lower.find("help") == 0) continue;

            AppEntry e;
            e.display_name       = label;
            e.exe_path           = target;
            e.working_directory  = work_dir;
            e.source             = "start_menu";
            build_keys(e);
            out.push_back(std::move(e));
        }
    }
}

// ─── §4. Known install directories (shallow) ─────────────────────────────

static void scan_known_dirs(std::vector<AppEntry>& out)
{
    std::vector<fs::path> roots;
    auto add_env = [&](const char* var) {
        const char* val = std::getenv(var);
        if (val && val[0]) roots.emplace_back(val);
    };
    add_env("ProgramFiles");
    add_env("ProgramFiles(x86)");
    {
        const char* local = std::getenv("LocalAppData");
        if (local && local[0]) roots.emplace_back(media::path::path_from_utf8(local) / "Programs");
    }

    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        for (const auto& dir_entry : fs::directory_iterator(root, ec)) {
            if (!dir_entry.is_directory()) continue;
            auto add_exes = [&](const fs::path& dir, const std::string& label) {
                std::error_code ec2;
                for (const auto& file : fs::directory_iterator(dir, ec2)) {
                    if (!file.is_regular_file()) continue;
                    if (!af_is_native_ext(media::path::path_to_utf8(file.path().extension()))) continue;
                    if (af_is_uninstaller_name(media::strutil::lower_ascii(media::path::path_to_utf8(file.path().stem())))) continue;
                    if (af_is_installer_cache_path(media::path::path_to_utf8(file.path()))) continue;

                    AppEntry e;
                    e.display_name = label;
                    e.exe_path     = media::path::path_to_utf8(file.path());
                    e.install_root = media::path::path_to_utf8(dir);
                    e.source       = "known_dir";
                    build_keys(e);
                    out.push_back(std::move(e));
                }
            };

            const fs::path vendor = dir_entry.path();
            const std::size_t before = out.size();
            add_exes(vendor, media::path::path_to_utf8(vendor.filename()));
            // SOLIDWORKS Corp\SOLIDWORKS\SLDWORKS.exe — one extra level only
            // when the vendor folder itself has no product exe.
            if (out.size() == before) {
                std::error_code ec3;
                for (const auto& sub : fs::directory_iterator(vendor, ec3)) {
                    if (!sub.is_directory()) continue;
                    add_exes(sub.path(), media::path::path_to_utf8(sub.path().filename()));
                }
            }
        }
    }
}

// ─── §5. Store / MSIX packages (user aliases + AUMID, skip inbox) ─────────

static std::wstring wide_lower(std::wstring s)
{
    for (wchar_t& c : s)
        c = static_cast<wchar_t>(::towlower(c));
    return s;
}

static bool is_appexeclink_path(const fs::path& p)
{
    WIN32_FIND_DATAW fd{};
    const HANDLE h = ::FindFirstFileW(p.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    ::FindClose(h);
    return (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        && fd.dwReserved0 == IO_REPARSE_TAG_APPEXECLINK;
}

static std::wstring aumid_from_reparse(const fs::path& p)
{
    const HANDLE h = ::CreateFileW(
        p.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};

    alignas(8) unsigned char raw[16 * 1024]{};
    DWORD got = 0;
    const BOOL ok = ::DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0,
                                      raw, sizeof(raw), &got, nullptr);
    ::CloseHandle(h);
    if (!ok || got < 16)
        return {};

    struct Hdr {
        ULONG  tag;
        USHORT data_len;
        USHORT reserved;
        ULONG  version;
    };
    const auto* hdr = reinterpret_cast<const Hdr*>(raw);
    if (hdr->tag != IO_REPARSE_TAG_APPEXECLINK)
        return {};

    const wchar_t* cur = reinterpret_cast<const wchar_t*>(raw + sizeof(Hdr));
    const wchar_t* end = reinterpret_cast<const wchar_t*>(raw + got);
    // Version 3: [0] package id, [1] entry point / AUMID, [2] exe, [3] type
    for (int i = 0; i < 2 && cur < end; ++i) {
        const wchar_t* next = cur;
        while (next < end && *next)
            ++next;
        if (i == 1)
            return std::wstring(cur, next);
        cur = (next < end) ? next + 1 : end;
    }
    return {};
}

static std::string display_name_for_aumid(const std::wstring& aumid)
{
    if (aumid.empty())
        return {};
    const std::wstring parse = L"shell:AppsFolder\\" + aumid;
    IShellItem* item = nullptr;
    if (FAILED(::SHCreateItemFromParsingName(parse.c_str(), nullptr, IID_PPV_ARGS(&item))) || !item)
        return {};
    PWSTR name = nullptr;
    std::string out;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name) {
        out = wide_to_utf8(name);
        CoTaskMemFree(name);
    }
    item->Release();
    return out;
}

static bool family_is_system(const std::wstring& family_l)
{
    static const wchar_t* k_pref[] = {
        L"windows.",
        L"microsoft.windows.shellexperiencehost",
        L"microsoft.windows.startmenuexperiencehost",
        L"microsoft.windows.search",
        L"microsoft.windows.cloudexperiencehost",
        L"microsoft.windows.contentdeliverymanager",
        L"microsoft.windows.sechealthui",
        L"microsoft.windows.assignedaccesslockapp",
        L"microsoft.windows.apprep.",
        L"microsoft.windows.oobe",
        L"microsoft.windows.parentalcontrols",
        L"microsoft.windows.peopleexperiencehost",
        L"microsoft.windows.pinningconfirmationdialog",
        L"microsoft.windows.printqueueactioncenter",
        L"microsoft.windows.capturepicker",
        L"microsoft.windows.narrator",
        L"microsoft.windows.callingshellapp",
        L"microsoft.windows.internal.",
        L"microsoft.aad.",
        L"microsoft.accountscontrol",
        L"microsoft.lockapp",
        L"microsoft.bioenrollment",
        L"microsoft.creddialoghost",
        L"microsoft.ecapp",
        L"microsoft.win32webviewhost",
        L"microsoft.asynctextservice",
        L"microsoft.xboxgamecallableui",
        L"microsoft.vclibs",
        L"microsoft.net.",
        L"microsoft.ui.xaml",
        L"microsoft.windowsappruntime",
        L"microsoft.services.store.engagement",
        L"microsoft.microsoftedgedevtoolsclient",
    };
    for (const wchar_t* p : k_pref) {
        const std::size_t n = std::wcslen(p);
        if (family_l.size() >= n && family_l.compare(0, n, p) == 0)
            return true;
    }
    return false;
}

static LONG package_origin(PCWSTR full_name, PackageOrigin* origin)
{
    using Fn = LONG (WINAPI*)(PCWSTR, PackageOrigin*);
    static const Fn fn = reinterpret_cast<Fn>(
        ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "GetStagedPackageOrigin"));
    if (!fn || !origin)
        return ERROR_NOT_SUPPORTED;
    return fn(full_name, origin);
}

static bool is_system_package(const std::wstring& aumid)
{
    if (aumid.empty())
        return true;

    UINT32 family_len = 0;
    UINT32 praid_len = 0;
    LONG rc = ::ParseApplicationUserModelId(aumid.c_str(), &family_len, nullptr,
                                            &praid_len, nullptr);
    if (rc != ERROR_INSUFFICIENT_BUFFER || family_len == 0)
        return true;

    std::wstring family(family_len, L'\0');
    std::wstring praid(praid_len, L'\0');
    rc = ::ParseApplicationUserModelId(aumid.c_str(), &family_len, family.data(),
                                       &praid_len, praid.data());
    if (rc != ERROR_SUCCESS)
        return true;
    if (!family.empty() && family.back() == L'\0')
        family.pop_back();

    if (family_is_system(wide_lower(family)))
        return true;

    UINT32 count = 0;
    UINT32 buf_len = 0;
    rc = ::FindPackagesByPackageFamily(family.c_str(), PACKAGE_FILTER_HEAD,
                                       &count, nullptr, &buf_len, nullptr, nullptr);
    if (rc == ERROR_SUCCESS)
        return count == 0;
    if (rc != ERROR_INSUFFICIENT_BUFFER || count == 0)
        return true;

    std::vector<PWSTR> names(count, nullptr);
    std::vector<wchar_t> buf(buf_len, L'\0');
    std::vector<UINT32> props(count, 0);
    rc = ::FindPackagesByPackageFamily(family.c_str(), PACKAGE_FILTER_HEAD,
                                       &count, names.data(), &buf_len, buf.data(),
                                       props.data());
    if (rc != ERROR_SUCCESS)
        return true;

    bool any_user = false;
    for (UINT32 i = 0; i < count; ++i) {
        if (props[i] & PACKAGE_PROPERTY_FRAMEWORK)
            continue;
        PackageOrigin origin = PackageOrigin_Unknown;
        if (package_origin(names[i], &origin) == ERROR_SUCCESS
            && origin == PackageOrigin_Inbox)
            return true;
        any_user = true;
    }
    return !any_user;
}

static void scan_app_packages(std::vector<AppEntry>& out)
{
    PWSTR local = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        return;
    const fs::path root = std::filesystem::path(local) / L"Microsoft" / L"WindowsApps";
    CoTaskMemFree(local);

    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return;

    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec)
            break;
        if (media::strutil::lower_ascii(media::path::path_to_utf8(entry.path().extension())) != ".exe")
            continue;
        if (!is_appexeclink_path(entry.path()))
            continue;

        const std::wstring aumid = aumid_from_reparse(entry.path());
        if (is_system_package(aumid))
            continue;

        const std::string stem = media::strutil::lower_ascii(media::path::path_to_utf8(entry.path().stem()));
        if (stem == "systemsettings" || stem == "searchhost"
            || stem == "applicationframehost")
            continue;

        // Shell display names marshal onto the UI thread and freeze the window
        // the first time this cache is built. The exe stem is enough to match.
        std::string label = media::path::path_to_utf8(entry.path().stem());

        AppEntry e;
        e.display_name = std::move(label);
        e.exe_path     = media::path::path_to_utf8(entry.path());
        e.source       = "app_package";
        build_keys(e);
        out.push_back(std::move(e));
    }
}

static void scan_apps_folder(std::vector<AppEntry>& out)
{
    IShellItem* folder = nullptr;
    if (FAILED(::SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr, IID_PPV_ARGS(&folder)))
        || !folder)
        return;

    IEnumShellItems* en = nullptr;
    const HRESULT bind = folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en));
    folder->Release();
    if (FAILED(bind) || !en)
        return;

    IShellItem* item = nullptr;
    while (en->Next(1, &item, nullptr) == S_OK && item) {
        IShellItem2* item2 = nullptr;
        if (FAILED(item->QueryInterface(IID_PPV_ARGS(&item2))) || !item2) {
            item->Release();
            continue;
        }

        PWSTR aumid_raw = nullptr;
        const HRESULT id_hr = item2->GetString(PKEY_AppUserModel_ID, &aumid_raw);
        item2->Release();
        if (FAILED(id_hr) || !aumid_raw) {
            item->Release();
            continue;
        }
        const std::wstring aumid(aumid_raw);
        CoTaskMemFree(aumid_raw);
        if (is_system_package(aumid)) {
            item->Release();
            continue;
        }

        PWSTR fspath = nullptr;
        std::string exe;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &fspath)) && fspath) {
            exe = wide_to_utf8(fspath);
            CoTaskMemFree(fspath);
        }
        PWSTR name = nullptr;
        std::string label;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name) {
            label = wide_to_utf8(name);
            CoTaskMemFree(name);
        }
        item->Release();

        if (exe.empty() || !af_is_native_ext(media::path::path_to_utf8(media::path::path_from_utf8(exe).extension()))
            || !af_exe_exists(exe))
            continue;
        if (label.empty())
            label = media::path::path_to_utf8(media::path::path_from_utf8(exe).stem());

        AppEntry e;
        e.display_name = std::move(label);
        e.exe_path     = std::move(exe);
        e.source       = "app_package";
        build_keys(e);
        out.push_back(std::move(e));
    }
    en->Release();
}

} // namespace (anon)

// ─── Platform hooks ───────────────────────────────────────────────────────

std::filesystem::path af_platform_config_dir()
{
    PWSTR path_tmp = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path_tmp)))
        return {};
    std::filesystem::path p(path_tmp);
    CoTaskMemFree(path_tmp);
    p /= "PolyMech";
    p /= "pm-image";
    return p;
}

void af_scan_platform(std::vector<AppEntry>& out)
{
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    scan_app_paths(out);
    scan_registry_uninstall(out);
    scan_start_menu(out);
    scan_known_dirs(out);
    scan_app_packages(out);
    // shell:AppsFolder enumeration is a UI-thread COM call. Skip it so the
    // first search can run on a worker without freezing the window.

    if (SUCCEEDED(co)) CoUninitialize();
}

} // namespace media::llm::info

#endif // _WIN32
