/*
 * winemenu_bridge.cpp — Universal Context Menu → JSON Bridge (v3)
 * ================================================================
 *
 * Purely dynamic extraction of right-click context menu items and their
 * underlying CLI commands.  Zero hardcoded string-to-command mappings.
 *
 * Architecture (three phases):
 *
 * Phase 1 — COM Menu Discovery
 * For each shellex\ContextMenuHandler, instantiate the COM object,
 * initialize via IShellExtInit with a synthetic dummy file, call
 * IContextMenu::QueryContextMenu to populate a real HMENU, then
 * walk the menu tree with GetMenuItemInfoW.  This yields ONLY the
 * actual UI strings the user would see (no STRINGTABLE garbage).
 *
 * Phase 2 — Dynamic Command Interception
 * Install 12-byte absolute-JMP inline detours on CreateProcessW,
 * CreateProcessA, and ShellExecuteExW within the current process.
 * For each menu item discovered in Phase 1, call InvokeCommand with
 * CMIC_MASK_FLAG_NO_UI | SW_HIDE.  The detour captures the exact
 * lpCommandLine / lpFile+lpParameters and returns FALSE to block
 * actual execution.
 *
 * Phase 3 — Extension Grouping
 * Many extensions (.zip, .rar, .7z) yield identical action lists.
 * Using ActionInfo::operator==, group extensions that share the
 * same action vector into a single JSON object to avoid redundancy.
 *
 * Compile (MinGW-w64 on Linux):
 * x86_64-w64-mingw32-g++ -static winemenu_bridge.cpp -o winemenu_bridge.exe \
 * -lole32 -luuid -lshell32
 *
 * Run:
 * wine winemenu_bridge.exe WinRAR.ZIP
 * wine winemenu_bridge.exe 7-Zip.7z -o ctx.json -v
 */

#define UNICODE
#define _UNICODE
#define _WIN32_WINNT  0x0600
#define NTDDI_VERSION 0x06000000

#include <initguid.h>
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <utility>

// ===================================================================
//  CONSTANTS
// ===================================================================

#define MENU_ID_FIRST   1
#define MENU_ID_LAST    0x7FFF

#ifndef CMIC_MASK_FLAG_NO_UI
#define CMIC_MASK_FLAG_NO_UI  0x00000400
#endif

static bool g_verbose = false;

#define LOG_V(fmt, ...) \
    do { if (g_verbose) fprintf(stderr, "[*] " fmt "\n", ##__VA_ARGS__); } while (0)
#define LOG_E(fmt, ...) fprintf(stderr, "[!] " fmt "\n", ##__VA_ARGS__)
#define LOG_I(fmt, ...) fprintf(stderr, "[+] " fmt "\n", ##__VA_ARGS__)

// ===================================================================
//  DATA STRUCTURES
// ===================================================================

struct ActionInfo {
    std::wstring uiName;       // The exact menu button text
    std::wstring rawCommand;   // Intercepted CLI command (empty if no process created)
    std::wstring source;       // "static" | "dynamic_hook"
    std::wstring verb;         // Shell verb or command offset ID
    std::wstring handler;      // Handler name (dynamic only)

    // Grouping comparison: two actions are equivalent if they present the
    // same button text, invoke the same command, and come from the same layer.
    bool operator==(const ActionInfo& o) const {
        return uiName    == o.uiName
            && rawCommand == o.rawCommand
            && source     == o.source;
    }
    bool operator!=(const ActionInfo& o) const { return !(*this == o); }
};

struct ExtensionGroup {
    std::vector<std::wstring> extensions;
    std::vector<ActionInfo>   actions;
};

struct HandlerEntry {
    std::wstring name;
    CLSID        clsid;
};

struct MenuItem {
    UINT                    id;
    std::wstring            text;
    bool                    isSeparator;
    std::vector<MenuItem>   children;
};

// ===================================================================
//  STRING UTILITIES
// ===================================================================

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        &s[0], n, nullptr, nullptr);
    return s;
}

static std::string JsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b,sizeof(b),"\\u%04x",c); o += b; }
                else          { o += (char)c; }
        }
    }
    return o;
}

static std::string JStr(const std::wstring& ws) {
    return "\"" + JsonEscape(WideToUtf8(ws)) + "\"";
}

static bool WcsContainsI(const std::wstring& hay, const std::wstring& ndl) {
    if (ndl.empty()) return true;
    std::wstring h = hay, n = ndl;
    std::transform(h.begin(), h.end(), h.begin(), ::towlower);
    std::transform(n.begin(), n.end(), n.begin(), ::towlower);
    return h.find(n) != std::wstring::npos;
}

static std::wstring WcsToLower(const std::wstring& s) {
    std::wstring o = s;
    std::transform(o.begin(), o.end(), o.begin(), ::towlower);
    return o;
}

// Strip Win32 accelerator markers (&X), keyboard shortcut tabs (\t...),
// and trailing whitespace from menu text.
static std::wstring CleanMenuText(const std::wstring& raw) {
    std::wstring o;
    o.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == L'\t') break;                   // tab = shortcut hint
        if (raw[i] == L'&') {
            if (i + 1 < raw.size() && raw[i + 1] == L'&')
                { o += L'&'; ++i; }                   // escaped &&
            // else: skip single & (accelerator)
        } else {
            o += raw[i];
        }
    }
    while (!o.empty() && (o.back() == L' ' || o.back() == L'\r'))
        o.pop_back();
    return o;
}

// ===================================================================
//  x86_64 INLINE HOOK ENGINE
//
//  Overwrites the target function's prologue with:
//      48 B8  [8-byte address]    ; mov rax, &detour
//      FF E0                       ; jmp rax
//  Total: 12 bytes.  No trampoline needed — we intentionally BLOCK
//  the original call (return FALSE) after capturing the arguments.
//
//  g_hookActive:         master switch, true only during InvokeCommand
//  g_commandIntercepted: set on first capture, prevents overwrite from
//                        internal call chaining (ShellExecuteExW → CreateProcessW)
// ===================================================================

static bool         g_hookActive         = false;
static std::wstring g_interceptedCommand;
static bool         g_commandIntercepted = false;

struct InlineHook {
    BYTE* pTarget;
    BYTE   saved[14];
    DWORD  patchLen;
    bool   active;

    InlineHook() : pTarget(nullptr), patchLen(12), active(false) {
        memset(saved, 0xCC, sizeof(saved));
    }

    bool Install(void* target, void* detour) {
        pTarget = reinterpret_cast<BYTE*>(target);
        DWORD oldProt;
        if (!VirtualProtect(pTarget, patchLen,
                            PAGE_EXECUTE_READWRITE, &oldProt)) {
            LOG_E("VirtualProtect(%p) failed: %lu", pTarget, GetLastError());
            return false;
        }
        memcpy(saved, pTarget, patchLen);
        // mov rax, imm64
        pTarget[0]  = 0x48;
        pTarget[1]  = 0xB8;
        *reinterpret_cast<UINT64*>(pTarget + 2) =
            static_cast<UINT64>(reinterpret_cast<uintptr_t>(detour));
        // jmp rax
        pTarget[10] = 0xFF;
        pTarget[11] = 0xE0;
        FlushInstructionCache(GetCurrentProcess(), pTarget, patchLen);
        DWORD tmp;
        VirtualProtect(pTarget, patchLen, oldProt, &tmp);
        active = true;
        return true;
    }

    void Remove() {
        if (!active || !pTarget) return;
        DWORD oldProt;
        if (VirtualProtect(pTarget, patchLen,
                           PAGE_EXECUTE_READWRITE, &oldProt)) {
            memcpy(pTarget, saved, patchLen);
            FlushInstructionCache(GetCurrentProcess(), pTarget, patchLen);
            DWORD tmp;
            VirtualProtect(pTarget, patchLen, oldProt, &tmp);
        }
        active = false;
    }
};

static InlineHook g_hkCreateProcessW;
static InlineHook g_hkCreateProcessA;
static InlineHook g_hkShellExecuteExW;

// ===================================================================
//  API DETOUR IMPLEMENTATIONS
//
//  Each detour:
//    1. Checks g_hookActive && !g_commandIntercepted
//    2. Records the command string
//    3. Returns FALSE to block actual execution
// ===================================================================

static BOOL WINAPI Det_CreateProcessW(
    LPCWSTR lpApp, LPWSTR lpCmd,
    LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCWSTR,
    LPSTARTUPINFOW, LPPROCESS_INFORMATION pi)
{
    if (g_hookActive && !g_commandIntercepted) {
        g_commandIntercepted = true;
        if      (lpCmd && lpCmd[0]) g_interceptedCommand = lpCmd;
        else if (lpApp && lpApp[0]) g_interceptedCommand = lpApp;
        LOG_V("  [hook] CreateProcessW: %ls", g_interceptedCommand.c_str());
    }
    if (pi) memset(pi, 0, sizeof(*pi));
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

static BOOL WINAPI Det_CreateProcessA(
    LPCSTR lpApp, LPSTR lpCmd,
    LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCSTR,
    LPSTARTUPINFOA, LPPROCESS_INFORMATION pi)
{
    if (g_hookActive && !g_commandIntercepted) {
        g_commandIntercepted = true;
        const char* src = (lpCmd && lpCmd[0]) ? lpCmd
                        : (lpApp && lpApp[0]) ? lpApp : nullptr;
        if (src) {
            int wn = MultiByteToWideChar(CP_ACP, 0, src, -1, nullptr, 0);
            if (wn > 1) {
                g_interceptedCommand.resize(wn - 1);
                MultiByteToWideChar(CP_ACP, 0, src, -1,
                                    &g_interceptedCommand[0], wn);
            }
        }
        LOG_V("  [hook] CreateProcessA: %ls", g_interceptedCommand.c_str());
    }
    if (pi) memset(pi, 0, sizeof(*pi));
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

static BOOL WINAPI Det_ShellExecuteExW(SHELLEXECUTEINFOW* sei) {
    if (g_hookActive && !g_commandIntercepted && sei) {
        g_commandIntercepted = true;
        std::wstring cmd;
        if (sei->lpFile)       cmd  = sei->lpFile;
        if (sei->lpParameters) { cmd += L" "; cmd += sei->lpParameters; }
        g_interceptedCommand = cmd;
        sei->hInstApp = reinterpret_cast<HINSTANCE>(static_cast<uintptr_t>(42));
        LOG_V("  [hook] ShellExecuteExW: %ls", g_interceptedCommand.c_str());
    }
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

static bool InstallAllHooks() {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE s32 = GetModuleHandleW(L"shell32.dll");
    bool ok = true;
    void* p;
    if (k32) {
        if ((p = (void*)GetProcAddress(k32, "CreateProcessW")))
            ok &= g_hkCreateProcessW.Install(p, (void*)Det_CreateProcessW);
        if ((p = (void*)GetProcAddress(k32, "CreateProcessA")))
            ok &= g_hkCreateProcessA.Install(p, (void*)Det_CreateProcessA);
    }
    if (s32) {
        if ((p = (void*)GetProcAddress(s32, "ShellExecuteExW")))
            ok &= g_hkShellExecuteExW.Install(p, (void*)Det_ShellExecuteExW);
    }
    if (ok) LOG_I("API hooks armed (CreateProcessW/A, ShellExecuteExW)");
    else    LOG_E("Hook installation partially failed");
    return ok;
}

static void RemoveAllHooks() {
    g_hkCreateProcessW.Remove();
    g_hkCreateProcessA.Remove();
    g_hkShellExecuteExW.Remove();
    LOG_V("API hooks removed");
}

// ===================================================================
//  MINIMAL IDataObject — CF_HDROP PROVIDER
//
//  Wraps a single file path in a DROPFILES structure.  Handed to
//  IShellExtInit::Initialize so the handler knows what file type
//  to build its context menu for.
// ===================================================================

class DropDataObject : public IDataObject {
    LONG    m_ref;
    HGLOBAL m_hDrop;

public:
    explicit DropDataObject(const std::wstring& path) : m_ref(1), m_hDrop(nullptr) {
        DWORD pathBytes = (DWORD)((path.size() + 2) * sizeof(WCHAR));
        DWORD total     = sizeof(DROPFILES) + pathBytes;
        m_hDrop = GlobalAlloc(GHND, total);
        if (m_hDrop) {
            BYTE* p = (BYTE*)GlobalLock(m_hDrop);
            DROPFILES* df = (DROPFILES*)p;
            df->pFiles = sizeof(DROPFILES);
            df->fWide  = TRUE;
            WCHAR* dst = (WCHAR*)(p + sizeof(DROPFILES));
            wcscpy(dst, path.c_str());
            dst[path.size() + 1] = L'\0';  // double-null termination
            GlobalUnlock(m_hDrop);
        }
    }
    ~DropDataObject() { if (m_hDrop) GlobalFree(m_hDrop); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDataObject)) {
            *ppv = static_cast<IDataObject*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fe, STGMEDIUM* med) override {
        if (!fe || !med) return E_INVALIDARG;
        if (fe->cfFormat == CF_HDROP && (fe->tymed & TYMED_HGLOBAL)) {
            SIZE_T sz = GlobalSize(m_hDrop);
            med->tymed = TYMED_HGLOBAL;
            med->hGlobal = GlobalAlloc(GHND, sz);
            if (!med->hGlobal) return E_OUTOFMEMORY;
            memcpy(GlobalLock(med->hGlobal), GlobalLock(m_hDrop), sz);
            GlobalUnlock(med->hGlobal); GlobalUnlock(m_hDrop);
            med->pUnkForRelease = nullptr;
            return S_OK;
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fe) override {
        return (fe && fe->cfFormat == CF_HDROP && (fe->tymed & TYMED_HGLOBAL))
               ? S_OK : DV_E_FORMATETC;
    }
    // Stubs
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*)        override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* o) override
        { if (o) o->ptd = nullptr; return DATA_S_SAMEFORMATETC; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL)      override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**)     override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
        { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD)          override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};

// ===================================================================
//  REGISTRY HELPERS
// ===================================================================

static std::wstring RegGetStr(HKEY hk, const wchar_t* name = nullptr) {
    DWORD type = 0, sz = 0;
    if (RegQueryValueExW(hk, name, nullptr, &type, nullptr, &sz) != ERROR_SUCCESS)
        return {};
    if (type != REG_SZ && type != REG_EXPAND_SZ) return {};
    std::wstring val(sz / sizeof(WCHAR), L'\0');
    RegQueryValueExW(hk, name, nullptr, &type, (BYTE*)&val[0], &sz);
    while (!val.empty() && val.back() == L'\0') val.pop_back();
    if (type == REG_EXPAND_SZ) {
        DWORD expLen = ExpandEnvironmentStringsW(val.c_str(), nullptr, 0);
        if (expLen > 0) {
            std::wstring exp(expLen, L'\0');
            ExpandEnvironmentStringsW(val.c_str(), &exp[0], expLen);
            while (!exp.empty() && exp.back() == L'\0') exp.pop_back();
            return exp;
        }
    }
    return val;
}

// Search HKCR for ProgIDs matching query (exact match first, then substring)
static std::vector<std::wstring> FindProgIds(const std::wstring& query) {
    std::vector<std::wstring> out;
    HKEY hTest;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, query.c_str(), 0,
                      KEY_READ, &hTest) == ERROR_SUCCESS) {
        RegCloseKey(hTest);
        out.push_back(query);
        return out;
    }
    HKEY hkcr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, nullptr, 0, KEY_READ, &hkcr) != ERROR_SUCCESS)
        return out;
    WCHAR name[512];
    for (DWORD i = 0; ; ++i) {
        DWORD nlen = 512;
        if (RegEnumKeyExW(hkcr, i, name, &nlen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        if (name[0] == L'.' || name[0] == L'{') continue;
        if (!WcsContainsI(name, query)) continue;
        // Only include ProgIDs that have shell or shellex children
        HKEY hc;
        bool useful = false;
        std::wstring sp = std::wstring(name) + L"\\shell";
        if (RegOpenKeyExW(hkcr, sp.c_str(), 0, KEY_READ, &hc) == ERROR_SUCCESS)
            { useful = true; RegCloseKey(hc); }
        sp = std::wstring(name) + L"\\shellex";
        if (!useful && RegOpenKeyExW(hkcr, sp.c_str(), 0, KEY_READ, &hc) == ERROR_SUCCESS)
            { useful = true; RegCloseKey(hc); }
        if (useful) out.push_back(name);
    }
    RegCloseKey(hkcr);
    return out;
}

// Find .ext keys whose default value or OpenWithProgids matches a ProgID
static std::vector<std::wstring> FindExtensions(const std::wstring& progId) {
    std::vector<std::wstring> exts;
    HKEY hkcr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, nullptr, 0, KEY_READ, &hkcr) != ERROR_SUCCESS)
        return exts;
    WCHAR name[256];
    for (DWORD i = 0; ; ++i) {
        DWORD nlen = 256;
        if (RegEnumKeyExW(hkcr, i, name, &nlen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        if (name[0] != L'.') continue;
        HKEY hExt;
        if (RegOpenKeyExW(hkcr, name, 0, KEY_READ, &hExt) != ERROR_SUCCESS) continue;
        bool match = (_wcsicmp(RegGetStr(hExt).c_str(), progId.c_str()) == 0);
        if (!match) {
            HKEY hOwp;
            if (RegOpenKeyExW(hExt, L"OpenWithProgids", 0, KEY_READ, &hOwp) == ERROR_SUCCESS) {
                match = (RegQueryValueExW(hOwp, progId.c_str(), nullptr,
                                          nullptr, nullptr, nullptr) == ERROR_SUCCESS);
                RegCloseKey(hOwp);
            }
        }
        if (match) exts.push_back(name);
        RegCloseKey(hExt);
    }
    RegCloseKey(hkcr);
    return exts;
}

// Extract static shell\<verb>\command entries from a ProgID
static std::vector<ActionInfo> ExtractStaticVerbs(const std::wstring& progId) {
    std::vector<ActionInfo> out;
    HKEY hShell;
    std::wstring path = progId + L"\\shell";
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path.c_str(), 0, KEY_READ, &hShell) != ERROR_SUCCESS)
        return out;

    WCHAR vname[256];
    for (DWORD i = 0; ; ++i) {
        DWORD vlen = 256;
        if (RegEnumKeyExW(hShell, i, vname, &vlen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        HKEY hCmd;
        std::wstring cp = std::wstring(vname) + L"\\command";
        if (RegOpenKeyExW(hShell, cp.c_str(), 0, KEY_READ, &hCmd) != ERROR_SUCCESS)
            continue;
        std::wstring command = RegGetStr(hCmd);
        RegCloseKey(hCmd);
        if (command.empty()) continue;

        ActionInfo ai;
        ai.rawCommand = command;
        ai.source     = L"static";
        ai.verb       = vname;

        // Display name: MUIVerb > default value > verb key name
        HKEY hVerb;
        if (RegOpenKeyExW(hShell, vname, 0, KEY_READ, &hVerb) == ERROR_SUCCESS) {
            std::wstring mui = RegGetStr(hVerb, L"MUIVerb");
            if (!mui.empty())        ai.uiName = CleanMenuText(mui);
            else {
                std::wstring def = RegGetStr(hVerb);
                ai.uiName = CleanMenuText(def.empty() ? std::wstring(vname) : def);
            }
            RegCloseKey(hVerb);
        } else {
            ai.uiName = vname;
        }

        out.push_back(ai);
        LOG_I("  Static verb: \"%ls\" -> %ls", ai.uiName.c_str(), ai.rawCommand.c_str());
    }
    RegCloseKey(hShell);
    return out;
}

// Enumerate ContextMenuHandler CLSIDs under a registry prefix
static std::vector<HandlerEntry> FindHandlers(const std::wstring& prefix) {
    std::vector<HandlerEntry> out;
    std::wstring p = prefix + L"\\shellex\\ContextMenuHandlers";
    HKEY hCmh;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, p.c_str(), 0, KEY_READ, &hCmh) != ERROR_SUCCESS)
        return out;
    WCHAR name[256];
    for (DWORD i = 0; ; ++i) {
        DWORD nlen = 256;
        if (RegEnumKeyExW(hCmh, i, name, &nlen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        HKEY hH;
        if (RegOpenKeyExW(hCmh, name, 0, KEY_READ, &hH) != ERROR_SUCCESS) continue;
        std::wstring cs = RegGetStr(hH);
        RegCloseKey(hH);
        CLSID clsid;
        if (!cs.empty() && CLSIDFromString(cs.c_str(), &clsid) == S_OK)
            out.push_back({name, clsid});
    }
    RegCloseKey(hCmh);
    return out;
}

// ===================================================================
//  DUMMY FILE CREATION
//
//  Create a temporary file with the given extension so that
//  IShellExtInit::Initialize has a real path for CF_HDROP.
//  We write minimal magic-byte signatures for common archive formats
//  to maximize handler compatibility; unknown types get an empty file.
// ===================================================================

static std::wstring CreateDummyFile(const std::wstring& ext) {
    WCHAR tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring fpath = std::wstring(tmp) + L"wmbr_probe" + ext;
    HANDLE hf = CreateFileW(fpath.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return fpath;
    DWORD wr;
    // Minimal file signatures — aids handlers that check magic bytes
    // before populating menus.  NOT a heuristic for command mapping.
    if      (_wcsicmp(ext.c_str(), L".zip") == 0 || _wcsicmp(ext.c_str(), L".jar") == 0)
        { BYTE s[]={'P','K',3,4,0x14,0,0,0,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
          WriteFile(hf,s,sizeof(s),&wr,nullptr); }
    else if (_wcsicmp(ext.c_str(), L".rar") == 0)
        { BYTE s[]={'R','a','r','!',0x1A,7,1,0}; WriteFile(hf,s,sizeof(s),&wr,nullptr); }
    else if (_wcsicmp(ext.c_str(), L".7z") == 0)
        { BYTE s[]={'7','z',0xBC,0xAF,0x27,0x1C}; WriteFile(hf,s,sizeof(s),&wr,nullptr); }
    else if (_wcsicmp(ext.c_str(), L".gz") == 0 || _wcsicmp(ext.c_str(), L".tgz") == 0)
        { BYTE s[]={0x1F,0x8B}; WriteFile(hf,s,sizeof(s),&wr,nullptr); }
    else if (_wcsicmp(ext.c_str(), L".bz2") == 0)
        { BYTE s[]={'B','Z','h'}; WriteFile(hf,s,sizeof(s),&wr,nullptr); }
    else if (_wcsicmp(ext.c_str(), L".xz") == 0)
        { BYTE s[]={0xFD,'7','z','X','Z',0}; WriteFile(hf,s,sizeof(s),&wr,nullptr); }
    else if (_wcsicmp(ext.c_str(), L".tar") == 0)
        { BYTE s[512]={}; memcpy(s+257,"ustar",5); WriteFile(hf,s,sizeof(s),&wr,nullptr); }
    // else: empty file — handler must cope with extension alone
    CloseHandle(hf);
    return fpath;
}

// ===================================================================
//  HMENU TREE WALKING
//
//  After QueryContextMenu populates an HMENU, we recursively extract
//  all menu items including submenus.  FlattenMenu collapses the tree
//  into (id, "Parent → Child") pairs for sequential InvokeCommand calls.
// ===================================================================

static std::vector<MenuItem> WalkMenu(HMENU hm) {
    std::vector<MenuItem> items;
    int cnt = GetMenuItemCount(hm);
    if (cnt <= 0) return items;

    WCHAR buf[512];
    for (int i = 0; i < cnt; ++i) {
        MenuItem mi;
        mi.isSeparator = false;
        mi.id = 0;

        MENUITEMINFOW info = {};
        info.cbSize     = sizeof(info);
        info.fMask      = MIIM_ID | MIIM_TYPE | MIIM_SUBMENU | MIIM_STRING;
        info.dwTypeData = buf;
        info.cch        = 512;
        buf[0] = L'\0';

        if (!GetMenuItemInfoW(hm, i, TRUE, &info)) continue;

        if (info.fType & MFT_SEPARATOR) {
            mi.isSeparator = true;
        } else {
            mi.id   = info.wID;
            mi.text = CleanMenuText(buf);
            if (info.hSubMenu)
                mi.children = WalkMenu(info.hSubMenu);
        }
        items.push_back(mi);
    }
    return items;
}

static void FlattenMenu(const std::vector<MenuItem>& items,
                         std::vector<std::pair<UINT, std::wstring>>& out,
                         const std::wstring& prefix = L"")
{
    for (const auto& m : items) {
        if (m.isSeparator) continue;
        std::wstring label = prefix.empty()
            ? m.text
            : (prefix + L" \x2192 " + m.text);    // → separator for submenus
        if (m.children.empty()) {
            if (m.id >= MENU_ID_FIRST)
                out.push_back(std::make_pair(m.id, label));
        } else {
            FlattenMenu(m.children, out, label);
        }
    }
}

// ===================================================================
//  COM HANDLER PROBING — PHASE 1 + PHASE 2 COMBINED
//
//  For a single handler + dummy file path:
//    1. CoCreateInstance → IShellExtInit::Initialize → IContextMenu
//    2. QueryContextMenu into a fresh HMENU
//    3. Walk the HMENU to discover menu item IDs and labels
//    4. For each item: gate hooks → InvokeCommand → capture → ungate
//
//  Returns one ActionInfo per menu item (may have empty rawCommand if
//  the handler didn't call CreateProcess/ShellExecute for that item).
// ===================================================================

static std::vector<ActionInfo> ProbeHandler(
    const HandlerEntry& handler,
    const std::wstring& dummyPath)
{
    std::vector<ActionInfo> actions;
    HRESULT hr;

    // Step 1: Instantiate COM object
    IUnknown* pUnk = nullptr;
    hr = CoCreateInstance(handler.clsid, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IUnknown, (void**)&pUnk);
    if (FAILED(hr)) {
        LOG_E("  CoCreateInstance('%ls') failed: 0x%08lx",
              handler.name.c_str(), (unsigned long)hr);
        return actions;
    }

    // Step 2: Initialize with our dummy file via IShellExtInit
    IShellExtInit* pInit = nullptr;
    hr = pUnk->QueryInterface(IID_IShellExtInit, (void**)&pInit);
    if (FAILED(hr)) {
        LOG_E("  QI(IShellExtInit) failed for '%ls'", handler.name.c_str());
        pUnk->Release();
        return actions;
    }

    DropDataObject* pData = new DropDataObject(dummyPath);

    // Provide folder PIDL for handlers that need it
    PIDLIST_ABSOLUTE pidlFolder = nullptr;
    {
        std::wstring dir = dummyPath;
        size_t sl = dir.find_last_of(L"\\/");
        if (sl != std::wstring::npos) dir.resize(sl);
        SHParseDisplayName(dir.c_str(), nullptr, &pidlFolder, 0, nullptr);
    }

    hr = pInit->Initialize(pidlFolder, pData, nullptr);
    if (pidlFolder) CoTaskMemFree(pidlFolder);
    pData->Release();

    if (FAILED(hr)) {
        LOG_E("  IShellExtInit::Initialize('%ls') failed: 0x%08lx",
              handler.name.c_str(), (unsigned long)hr);
        pInit->Release(); pUnk->Release();
        return actions;
    }

    // Step 3: Get IContextMenu and populate an HMENU
    IContextMenu* pCtx = nullptr;
    hr = pUnk->QueryInterface(IID_IContextMenu, (void**)&pCtx);
    if (FAILED(hr)) {
        LOG_E("  QI(IContextMenu) failed for '%ls'", handler.name.c_str());
        pInit->Release(); pUnk->Release();
        return actions;
    }

    HMENU hMenu = CreatePopupMenu();
    hr = pCtx->QueryContextMenu(hMenu, 0, MENU_ID_FIRST, MENU_ID_LAST, CMF_NORMAL);
    if (FAILED(hr)) {
        LOG_E("  QueryContextMenu('%ls') failed: 0x%08lx",
              handler.name.c_str(), (unsigned long)hr);
        DestroyMenu(hMenu); pCtx->Release(); pInit->Release(); pUnk->Release();
        return actions;
    }

    UINT maxCmdId = MENU_ID_FIRST + HRESULT_CODE(hr);

    // Step 4: Walk the HMENU tree
    std::vector<std::pair<UINT, std::wstring>> flat;
    {
        auto tree = WalkMenu(hMenu);
        FlattenMenu(tree, flat);
    }

    LOG_I("  Handler '%ls': %zu menu items (id range %u..%u)",
          handler.name.c_str(), flat.size(),
          (unsigned)MENU_ID_FIRST, (unsigned)(maxCmdId - 1));

    // Step 5: Invoke each item with hooks gated — Phase 2
    std::wstring dir = dummyPath;
    { size_t sl = dir.find_last_of(L"\\/");
      if (sl != std::wstring::npos) dir.resize(sl); }
    std::string dirA = WideToUtf8(dir);

    for (size_t idx = 0; idx < flat.size(); ++idx) {
        UINT id = flat[idx].first;
        const std::wstring& label = flat[idx].second;
        if (id < MENU_ID_FIRST || id >= maxCmdId) continue;
        if (label.empty()) continue;

        // Reset capture state for this invocation
        g_interceptedCommand.clear();
        g_commandIntercepted = false;
        g_hookActive = true;

        CMINVOKECOMMANDINFO ici = {};
        ici.cbSize      = sizeof(ici);
        ici.fMask       = CMIC_MASK_FLAG_NO_UI;
        ici.hwnd        = nullptr;
        ici.lpVerb      = MAKEINTRESOURCEA(id - MENU_ID_FIRST);
        ici.nShow       = SW_HIDE;
        ici.lpDirectory  = dirA.c_str();

        hr = pCtx->InvokeCommand(&ici);

        g_hookActive = false;

        ActionInfo ai;
        ai.uiName  = label;
        ai.verb    = L"cmd_" + std::to_wstring(id - MENU_ID_FIRST);
        ai.handler = handler.name;

        if (g_commandIntercepted && !g_interceptedCommand.empty()) {
            ai.rawCommand = g_interceptedCommand;
            ai.source     = L"dynamic_hook";
            LOG_I("    [%zu/%zu] \"%ls\" -> %ls",
                  idx+1, flat.size(), label.c_str(), ai.rawCommand.c_str());
        } else {
            ai.rawCommand = L"";
            ai.source     = L"dynamic_hook";
            LOG_V("    [%zu/%zu] \"%ls\" -> (no process creation, hr=0x%08lx)",
                  idx+1, flat.size(), label.c_str(), (unsigned long)hr);
        }

        actions.push_back(ai);
    }

    DestroyMenu(hMenu);
    pCtx->Release();
    pInit->Release();
    pUnk->Release();

    return actions;
}

// ===================================================================
//  PER-EXTENSION ACTION COLLECTION
//
//  For a single extension:
//    1. Extract static verbs from the owning ProgID
//    2. Collect handlers (ProgID-level + extension-level)
//    3. Create a dummy file with this extension
//    4. Probe each unique handler via COM + hooks
//    5. Return the combined action list
// ===================================================================

static std::vector<ActionInfo> CollectActionsForExtension(
    const std::wstring& ext,
    const std::wstring& progId,
    bool hooksInstalled,
    const std::wstring& wSearch)
{
    std::vector<ActionInfo> allActions;

    // Layer 1: static verbs from the ProgID
    auto statics = ExtractStaticVerbs(progId);
    allActions.insert(allActions.end(), statics.begin(), statics.end());

    if (!hooksInstalled) return allActions;

    // Collect handlers from ProgID and extension, deduplicate by CLSID
    std::set<std::wstring> seenClsids;
    std::vector<HandlerEntry> handlers;

    auto addHandlers = [&](const std::vector<HandlerEntry>& hs) {
        for (const auto& h : hs) {
            WCHAR cs[64];
            StringFromGUID2(h.clsid, cs, 64);
            std::wstring key = cs;
            if (seenClsids.count(key)) continue;
            seenClsids.insert(key);
            handlers.push_back(h);
        }
    };

    addHandlers(FindHandlers(progId));
    addHandlers(FindHandlers(ext));

    // [الإصلاح الحاسم]: البحث في الـ Wildcard (HKCR\*) لجميع الملفات
    auto wildcardHandlers = FindHandlers(L"*");
    std::vector<HandlerEntry> matchedWildcards;
    for (const auto& h : wildcardHandlers) {
        // نأخذ فقط الـ Handlers التي يطابق اسمها بحثنا (مثل WinRAR)
        if (WcsContainsI(h.name, wSearch)) {
            matchedWildcards.push_back(h);
        }
    }
    addHandlers(matchedWildcards);

    if (handlers.empty()) return allActions;

    // Create dummy file for COM initialization
    std::wstring dummyPath = CreateDummyFile(ext);
    LOG_I("  Extension %ls: probing %zu handler(s) with %ls",
          ext.c_str(), handlers.size(), dummyPath.c_str());

    // Layer 2: probe each handler via COM (Phase 1 + Phase 2)
    for (const auto& h : handlers) {
        auto dynamic = ProbeHandler(h, dummyPath);
        allActions.insert(allActions.end(), dynamic.begin(), dynamic.end());
    }

    DeleteFileW(dummyPath.c_str());
    return allActions;
}

// ===================================================================
//  PHASE 3: EXTENSION GROUPING
//
//  Uses ActionInfo::operator== and std::vector::operator== to identify
//  extensions with identical action lists.  Produces a compact
//  representation where each group shares a single actions array.
// ===================================================================

static std::vector<ExtensionGroup> GroupExtensions(
    const std::map<std::wstring, std::vector<ActionInfo>>& perExt)
{
    std::vector<ExtensionGroup> groups;
    std::set<std::wstring> assigned;

    for (auto it = perExt.begin(); it != perExt.end(); ++it) {
        if (assigned.count(it->first)) continue;

        ExtensionGroup g;
        g.extensions.push_back(it->first);
        g.actions = it->second;
        assigned.insert(it->first);

        // Find all other extensions with identical action vectors
        for (auto jt = it; jt != perExt.end(); ++jt) {
            if (jt == it) continue;
            if (assigned.count(jt->first)) continue;
            if (jt->second == g.actions) {
                g.extensions.push_back(jt->first);
                assigned.insert(jt->first);
            }
        }

        std::sort(g.extensions.begin(), g.extensions.end());
        groups.push_back(g);
    }

    // Sort groups by first extension for deterministic output
    std::sort(groups.begin(), groups.end(),
              [](const ExtensionGroup& a, const ExtensionGroup& b) {
                  return a.extensions.front() < b.extensions.front();
              });

    return groups;
}

// ===================================================================
//  JSON EMITTER
// ===================================================================

static void EmitJson(FILE* fp,
                     const std::wstring& appName,
                     const std::vector<ExtensionGroup>& groups)
{
    fprintf(fp, "{\n");
    fprintf(fp, "  \"application\": %s,\n", JStr(appName).c_str());
    fprintf(fp, "  \"extension_groups\": [\n");

    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const auto& g = groups[gi];
        fprintf(fp, "    {\n");

        // Extensions array
        fprintf(fp, "      \"extensions\": [");
        for (size_t ei = 0; ei < g.extensions.size(); ++ei)
            fprintf(fp, "%s%s", (ei ? ", " : ""), JStr(g.extensions[ei]).c_str());
        fprintf(fp, "],\n");

        // Actions array
        fprintf(fp, "      \"actions\": [\n");
        for (size_t ai = 0; ai < g.actions.size(); ++ai) {
            const ActionInfo& a = g.actions[ai];
            fprintf(fp, "        {\n");
            fprintf(fp, "          \"ui_name\": %s,\n",    JStr(a.uiName).c_str());
            fprintf(fp, "          \"raw_command\": %s,\n", JStr(a.rawCommand).c_str());
            fprintf(fp, "          \"source\": %s",         JStr(a.source).c_str());
            if (!a.verb.empty())
                fprintf(fp, ",\n          \"verb\": %s", JStr(a.verb).c_str());
            if (!a.handler.empty())
                fprintf(fp, ",\n          \"handler\": %s", JStr(a.handler).c_str());
            fprintf(fp, "\n        }%s\n", (ai + 1 < g.actions.size() ? "," : ""));
        }
        fprintf(fp, "      ]\n");
        fprintf(fp, "    }%s\n", (gi + 1 < groups.size() ? "," : ""));
    }

    fprintf(fp, "  ]\n");
    fprintf(fp, "}\n");
}

// ===================================================================
//  ENTRY POINT
// ===================================================================

static void PrintUsage(const char* exe) {
    fprintf(stderr,
        "winemenu_bridge v3 — Universal Context Menu to JSON Bridge\n"
        "\n"
        "Usage: %s <ProgID|AppName> [options]\n"
        "\n"
        "Options:\n"
        "  -o, --output <file>   Write JSON to file (default: stdout)\n"
        "  -v, --verbose         Enable diagnostic output to stderr\n"
        "  -h, --help            Show this message\n"
        "\n"
        "Examples:\n"
        "  wine %s WinRAR.ZIP\n"
        "  wine %s 7-Zip.7z -o ctx.json -v\n",
        exe, exe, exe);
}

int main(int argc, char* argv[]) {
    if (argc < 2) { PrintUsage(argv[0]); return 1; }

    std::string searchTerm, outputFile;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
            g_verbose = true;
        else if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) && i + 1 < argc)
            outputFile = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
            { PrintUsage(argv[0]); return 0; }
        else if (searchTerm.empty())
            searchTerm = argv[i];
    }

    if (searchTerm.empty()) {
        LOG_E("Missing ProgID or application name.");
        return 1;
    }

    // Convert search term to wide string
    int wn = MultiByteToWideChar(CP_ACP, 0, searchTerm.c_str(), -1, nullptr, 0);
    std::wstring wSearch(wn - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, searchTerm.c_str(), -1, &wSearch[0], wn);

    // COM init (STA required for in-proc shell extensions)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        LOG_E("CoInitializeEx failed: 0x%08lx", (unsigned long)hr);
        return 1;
    }

    LOG_I("Query: \"%ls\"", wSearch.c_str());

    // ================================================================
    //  Step 1: Resolve ProgIDs and extensions
    // ================================================================
    std::vector<std::wstring> progIds = FindProgIds(wSearch);
    if (progIds.empty()) {
        LOG_E("No ProgIDs found matching \"%ls\".", wSearch.c_str());
        CoUninitialize();
        return 1;
    }

    // Build extension → ProgID ownership map
    std::map<std::wstring, std::wstring> extToProgId;
    for (const auto& pid : progIds) {
        LOG_I("  ProgID: %ls", pid.c_str());
        auto exts = FindExtensions(pid);
        for (const auto& e : exts)
            extToProgId[WcsToLower(e)] = pid;  // last ProgID wins on overlap
    }

    if (extToProgId.empty()) {
        LOG_E("No file extensions found for the matched ProgIDs.");
        CoUninitialize();
        return 1;
    }

    LOG_I("Total extensions: %zu", extToProgId.size());

    // ================================================================
    //  Step 2: Install API hooks
    // ================================================================
    bool hooksOk = InstallAllHooks();
    if (!hooksOk) {
        LOG_E("API hooks failed — dynamic command interception unavailable.");
        LOG_E("Static registry verbs will still be extracted.");
    }

    // ================================================================
    //  Step 3: Collect actions per extension
    // ================================================================
    std::map<std::wstring, std::vector<ActionInfo>> perExtActions;

    for (auto it = extToProgId.begin(); it != extToProgId.end(); ++it) {
        const std::wstring& ext   = it->first;
        const std::wstring& pid   = it->second;

        LOG_I("Processing extension: %ls (ProgID: %ls)", ext.c_str(), pid.c_str());

        auto actions = CollectActionsForExtension(ext, pid, hooksOk, wSearch);
        if (!actions.empty())
            perExtActions[ext] = actions;
    }

    if (hooksOk) RemoveAllHooks();

    // ================================================================
    //  Step 4: Group extensions with identical action sets (Phase 3)
    // ================================================================
    auto groups = GroupExtensions(perExtActions);

    // ================================================================
    //  Step 5: Emit JSON
    // ================================================================
    size_t totalActions = 0;
    for (const auto& g : groups)
        totalActions += g.actions.size();

    LOG_I("Result: %zu extension group(s), %zu total unique action(s)",
          groups.size(), totalActions);

    FILE* out = stdout;
    if (!outputFile.empty()) {
        out = fopen(outputFile.c_str(), "wb");
        if (!out) {
            LOG_E("Cannot open \"%s\" for writing", outputFile.c_str());
            out = stdout;
        }
    }

    EmitJson(out, wSearch, groups);

    if (out != stdout) {
        fclose(out);
        LOG_I("JSON written to: %s", outputFile.c_str());
    }

    CoUninitialize();
    return 0;
}