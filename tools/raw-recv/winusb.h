/* winusb.h - v0.7.24: the Windows viewer installs the Switch's USB driver
 * itself, so nobody needs Zadig.
 *
 * Windows 8 and later carry WinUSB with a signed generic INF (winusb.inf,
 * "WinUsb Device", USB\MS_COMP_WINUSB). Device Manager can put it on any USB
 * device: Update driver -> Let me pick -> Universal Serial Bus devices ->
 * WinUsb Device. That is done here with the same SetupAPI calls, so no
 * certificate is made or trusted (Zadig signs a driver package of its own).
 * The Switch cannot ask for WinUSB by itself: usb:ds takes no BOS platform
 * capability (MS OS 2.0) and answers no vendor request (MS OS 1.0).
 *
 * Installing needs administrator rights: the button starts this .exe again
 * through "runas" (Windows' permission prompt) with --install-driver DIR,
 * which does only drv_install_main() and writes DIR\driver-install.log. The
 * viewer waits for it on a thread and copies that log into viewer.log. */
#ifdef _WIN32
#include <setupapi.h>
#include <newdev.h>
#include <objbase.h>

enum { DRV_IDLE = 0, DRV_RUNNING, DRV_DONE, DRV_FAILED };
static SDL_atomic_t g_drv_state;
static char g_drv_msg[200] = "";

/* ---- the elevated side ---- */

static FILE *g_drv_log;
static void drv_log(const char *fmt, ...)
{
    if (!g_drv_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_drv_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_drv_log);
    fflush(g_drv_log);
}

/* USB\VID_1209&PID_5F1E or USB\VID_1209&PID_5F1E&REV_xxxx: the device itself */
static int drv_is_switch(const wchar_t *ids)
{
    static const wchar_t want[] = L"USB\\VID_1209&PID_5F1E";
    const size_t n = sizeof(want) / sizeof(want[0]) - 1;
    for (const wchar_t *p = ids; *p; p += wcslen(p) + 1)
        if (!_wcsnicmp(p, want, n) && (p[n] == 0 || !_wcsnicmp(p + n, L"&REV_", 5))) return 1;
    return 0;
}

static int drv_find(HDEVINFO *set_out, SP_DEVINFO_DATA *dev_out, wchar_t *inst, DWORD inst_n)
{
    HDEVINFO set = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) { drv_log("SetupDiGetClassDevs: error %lu", GetLastError()); return 0; }
    for (DWORD i = 0;; i++) {
        SP_DEVINFO_DATA d = { .cbSize = sizeof(d) };
        if (!SetupDiEnumDeviceInfo(set, i, &d)) break;
        wchar_t ids[1024] = { 0 };
        if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, NULL, (BYTE *)ids, sizeof(ids) - 2 * sizeof(wchar_t), NULL)) continue;
        if (!drv_is_switch(ids)) continue;
        if (!SetupDiGetDeviceInstanceIdW(set, &d, inst, inst_n, NULL)) inst[0] = 0;
        *set_out = set;
        *dev_out = d;
        return 1;
    }
    SetupDiDestroyDeviceInfoList(set);
    return 0;
}

static void drv_log_prop(HDEVINFO set, SP_DEVINFO_DATA *dev, DWORD prop, const char *name)
{
    wchar_t v[512] = { 0 };
    if (SetupDiGetDeviceRegistryPropertyW(set, dev, prop, NULL, (BYTE *)v, sizeof(v) - sizeof(wchar_t), NULL)) drv_log("  %s: %ls", name, v);
    else drv_log("  %s: (none, %lu)", name, GetLastError());
}

/* the "WinUsb Device" entry of winusb.inf, in the device's class driver list */
static int drv_pick(HDEVINFO set, SP_DEVINFO_DATA *dev, const wchar_t *inf, SP_DRVINFO_DATA_W *out)
{
    SP_DEVINSTALL_PARAMS_W p = { .cbSize = sizeof(p) };
    if (!SetupDiGetDeviceInstallParamsW(set, dev, &p)) { drv_log("SetupDiGetDeviceInstallParams: error %lu", GetLastError()); return 0; }
    p.Flags |= DI_ENUMSINGLEINF;
    p.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    wcsncpy(p.DriverPath, inf, MAX_PATH - 1);
    p.DriverPath[MAX_PATH - 1] = 0;
    if (!SetupDiSetDeviceInstallParamsW(set, dev, &p)) { drv_log("SetupDiSetDeviceInstallParams: error %lu", GetLastError()); return 0; }
    if (!SetupDiBuildDriverInfoList(set, dev, SPDIT_CLASSDRIVER)) { drv_log("SetupDiBuildDriverInfoList: error %lu", GetLastError()); return 0; }
    int found = 0;
    for (DWORD i = 0; i < 64; i++) {
        SP_DRVINFO_DATA_W d = { .cbSize = sizeof(d) };
        if (!SetupDiEnumDriverInfoW(set, dev, SPDIT_CLASSDRIVER, i, &d)) break;
        union { SP_DRVINFO_DETAIL_DATA_W det; BYTE b[8192]; } u;
        memset(&u, 0, sizeof(u));
        u.det.cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);
        DWORD need = 0;
        SetupDiGetDriverInfoDetailW(set, dev, &d, &u.det, sizeof(u) - 2 * sizeof(wchar_t), &need);   /* a short buffer still holds the first id */
        drv_log("  driver %lu: %ls (%ls) id %ls", (unsigned long)i, d.Description, d.ProviderName, u.det.HardwareID);
        if (!found && !_wcsicmp(u.det.HardwareID, L"USB\\MS_COMP_WINUSB")) { *out = d; found = 1; }
    }
    return found;
}

static int drv_install_main(void)
{
    /* the log folder, from the wide command line (argv is in the ANSI code page) */
    int argn = 0;
    wchar_t **argw = CommandLineToArgvW(GetCommandLineW(), &argn);
    wchar_t logp[MAX_PATH + 64] = L"";
    for (int i = 1; argw && i + 1 < argn; i++)
        if (!wcscmp(argw[i], L"--install-driver")) { _snwprintf(logp, MAX_PATH + 32, L"%ls\\driver-install.log", argw[i + 1]); break; }
    if (logp[0]) g_drv_log = _wfopen(logp, L"w");
    if (argw) LocalFree(argw);
    drv_log("Switch Frame Tap " SFT_VERSION " - installing WinUSB for the Switch (1209:5F1E)");

    HDEVINFO set = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA dev = { .cbSize = sizeof(dev) };
    wchar_t inst[512] = L"";
    if (!drv_find(&set, &dev, inst, 512)) { drv_log("result: the Switch is not on the bus"); return 2; }
    drv_log("device: %ls", inst);
    drv_log_prop(set, &dev, SPDRP_CLASSGUID, "class");
    drv_log_prop(set, &dev, SPDRP_SERVICE, "driver");

    wchar_t inf[MAX_PATH];
    UINT wn = GetWindowsDirectoryW(inf, MAX_PATH - 20);
    if (!wn || wn >= MAX_PATH - 20) { drv_log("result: no Windows folder"); return 3; }
    wcscat(inf, L"\\INF\\winusb.inf");
    if (GetFileAttributesW(inf) == INVALID_FILE_ATTRIBUTES) { drv_log("result: %ls is missing (Windows 7?)", inf); return 3; }

    /* the class driver list only holds INFs of the device's own class: an
     * unknown device, or one with another driver (libusbK, libusb0), is moved
     * to Universal Serial Bus devices first, as Device Manager does */
    static const GUID usbdevice = { 0x88bae032, 0x5a81, 0x49f0, { 0xbc, 0x3d, 0xa4, 0xff, 0x13, 0x82, 0x16, 0xd6 } };
    SP_DRVINFO_DATA_W drv = { .cbSize = sizeof(drv) };
    int found = drv_pick(set, &dev, inf, &drv);
    if (!found && !IsEqualGUID(&dev.ClassGuid, &usbdevice)) {
        static const wchar_t g[] = L"{88BAE032-5A81-49F0-BC3D-A4FF138216D6}";
        SetupDiDestroyDriverInfoList(set, &dev, SPDIT_CLASSDRIVER);
        if (!SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_CLASSGUID, (const BYTE *)g, sizeof(g)))
            drv_log("set class USBDevice: error %lu", GetLastError());
        else
            drv_log("class set to USBDevice");
        SetupDiDestroyDeviceInfoList(set);
        set = SetupDiCreateDeviceInfoList(NULL, NULL);
        dev = (SP_DEVINFO_DATA){ .cbSize = sizeof(dev) };
        if (set == INVALID_HANDLE_VALUE || !SetupDiOpenDeviceInfoW(set, inst, NULL, 0, &dev)) {
            drv_log("result: the device could not be opened again (error %lu)", GetLastError());
            return 4;
        }
        found = drv_pick(set, &dev, inf, &drv);
    }
    if (!found) { drv_log("result: Windows did not offer winusb.inf's WinUsb Device for it"); SetupDiDestroyDeviceInfoList(set); return 7; }

    /* as Zadig's driver does: an interface GUID for programs that look the
     * Switch up by one (libusb finds it either way) */
    HKEY k = SetupDiCreateDevRegKeyW(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, NULL, NULL);
    if (k != INVALID_HANDLE_VALUE) {
        static const wchar_t ig[] = L"{6E3A3F35-5C1B-4F0E-9A1D-5F1E12095F1E}\0";
        RegSetValueExW(k, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ, (const BYTE *)ig, sizeof(ig));
        RegCloseKey(k);
    }

    if (!SetupDiSetSelectedDriverW(set, &dev, &drv)) drv_log("SetupDiSetSelectedDriver: error %lu", GetLastError());
    typedef BOOL (WINAPI *install_t)(HWND, HDEVINFO, PSP_DEVINFO_DATA, PSP_DRVINFO_DATA_W, DWORD, PBOOL);
    HMODULE nd = LoadLibraryW(L"newdev.dll");
    install_t install = nd ? (install_t)(void *)GetProcAddress(nd, "DiInstallDevice") : NULL;
    BOOL reboot = FALSE, ok = FALSE;
    if (install) {
        ok = install(NULL, set, &dev, &drv, DIIDFLAG_NOFINISHINSTALLUI, &reboot);
        if (!ok) drv_log("DiInstallDevice: error 0x%lx", GetLastError());
    }
    if (!ok) {
        ok = SetupDiCallClassInstaller(DIF_INSTALLDEVICE, set, &dev);
        if (!ok) drv_log("DIF_INSTALLDEVICE: error 0x%lx", GetLastError());
    }
    if (ok) {
        static const wchar_t fn[] = L"Switch Frame Tap";
        SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_FRIENDLYNAME, (const BYTE *)fn, sizeof(fn));
        drv_log_prop(set, &dev, SPDRP_SERVICE, "driver now");
    }
    SetupDiDestroyDeviceInfoList(set);
    drv_log("result: %s%s", ok ? "installed" : "failed", reboot ? " - Windows wants a restart" : "");
    return ok ? (reboot ? 5 : 0) : 4;
}

/* ---- the viewer's side ---- */

static wchar_t g_drv_dir[MAX_PATH] = L"";

static int SDLCALL drv_thread(void *unused)
{
    (void)unused;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    wchar_t exe[MAX_PATH], params[MAX_PATH + 32];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    _snwprintf(params, MAX_PATH + 30, L"--install-driver \"%ls\"", g_drv_dir);
    params[MAX_PATH + 30] = 0;
    SHELLEXECUTEINFOW sei = { .cbSize = sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = params;
    sei.nShow = SW_HIDE;
    int state = DRV_FAILED;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        const DWORD e = GetLastError();
        if (e == ERROR_CANCELLED) snprintf(g_drv_msg, sizeof(g_drv_msg), "Windows' permission prompt was answered No.");
        else snprintf(g_drv_msg, sizeof(g_drv_msg), "The installer did not start (error %lu).", e);
        fprintf(stderr, "usb driver: %s\n", g_drv_msg);
    } else {
        DWORD code = 1;
        if (WaitForSingleObject(sei.hProcess, 120000) != WAIT_OBJECT_0) code = 6;
        else GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        switch (code) {
        case 0: snprintf(g_drv_msg, sizeof(g_drv_msg), "Driver installed - connecting to the Switch..."); state = DRV_DONE; break;
        case 5: snprintf(g_drv_msg, sizeof(g_drv_msg), "Driver installed - Windows wants a restart first."); state = DRV_DONE; break;
        case 2: snprintf(g_drv_msg, sizeof(g_drv_msg), "The Switch was not found - is the cable in?"); break;
        case 3: snprintf(g_drv_msg, sizeof(g_drv_msg), "This Windows has no generic WinUSB driver (Windows 7?)."); break;
        case 6: snprintf(g_drv_msg, sizeof(g_drv_msg), "The installer took too long."); break;
        case 7: snprintf(g_drv_msg, sizeof(g_drv_msg), "Windows did not offer its WinUSB driver for the Switch."); break;
        default: snprintf(g_drv_msg, sizeof(g_drv_msg), "Windows did not take the driver (code %lu).", code); break;
        }
        /* the installer's own log, into viewer.log */
        wchar_t lp[MAX_PATH + 32];
        _snwprintf(lp, MAX_PATH + 30, L"%ls\\driver-install.log", g_drv_dir);
        lp[MAX_PATH + 30] = 0;
        FILE *f = _wfopen(lp, L"r");
        fprintf(stderr, "usb driver: installer exit %lu\n", code);
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) fprintf(stderr, "  %s", line);
            fclose(f);
        }
    }
    CoUninitialize();
    SDL_AtomicSet(&g_drv_state, state);
    return 0;
}

/* the button: one install at a time */
static void drv_start(void)
{
    if (SDL_AtomicGet(&g_drv_state) == DRV_RUNNING) return;
    char *dir = SDL_GetPrefPath("switch-frame-tap", "viewer");
    if (!dir) return;
    size_t n = strlen(dir);
    while (n && (dir[n - 1] == '\\' || dir[n - 1] == '/')) dir[--n] = 0;   /* "C:\...\viewer\" would escape the closing quote */
    MultiByteToWideChar(CP_UTF8, 0, dir, -1, g_drv_dir, MAX_PATH);
    SDL_free(dir);
    snprintf(g_drv_msg, sizeof(g_drv_msg), "Installing - answer Yes in Windows' permission prompt.");
    SDL_AtomicSet(&g_drv_state, DRV_RUNNING);
    fprintf(stderr, "usb driver: installing WinUSB for the Switch\n");
    SDL_Thread *t = SDL_CreateThread(drv_thread, "sft-driver", NULL);
    if (t) SDL_DetachThread(t);
    else { snprintf(g_drv_msg, sizeof(g_drv_msg), "The installer did not start."); SDL_AtomicSet(&g_drv_state, DRV_FAILED); }
}

static int drv_state_for_menu(void) { return SDL_AtomicGet(&g_drv_state); }
static const char *drv_msg_for_menu(void) { return g_drv_msg; }
#endif
