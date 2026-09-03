#include "resource.h"

#include "pk2/archive.h"
#include "pk2/path.h"
#include "pk2/server_config.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <fstream>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

HINSTANCE gInstance = nullptr;
HWND gMain = nullptr;
HWND gTree = nullptr;
HWND gList = nullptr;
HWND gStatus = nullptr;
HWND gProgress = nullptr;
HWND gSearch = nullptr;
HWND gSearchButton = nullptr;
DWORD gUiThreadId = 0;

pk2::Pk2Archive gArchive;
bool gLoaded = false;
bool gBusy = false;
std::string gCurrentFolder;
int gTreeWidth = 260;
bool gDraggingSplitter = false;
WNDPROC gOriginalSearchProc = nullptr;

constexpr UINT WM_APP_OPEN_COMPLETE = WM_APP + 1;
constexpr UINT WM_APP_TASK_PROGRESS = WM_APP + 2;
constexpr UINT WM_APP_TASK_COMPLETE = WM_APP + 3;

struct OpenResult {
    bool ok{};
    pk2::Pk2Archive archive;
    std::wstring message;
};

struct TaskProgress {
    int percent{};
    std::wstring message;
};

struct TaskResult {
    bool ok{};
    bool archiveChanged{};
    std::string refreshFolder;
    std::wstring message;
};

HGLOBAL createDropEffectGlobal(DWORD effect) {
    auto handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (handle == nullptr) {
        return nullptr;
    }
    auto* value = static_cast<DWORD*>(GlobalLock(handle));
    if (value == nullptr) {
        GlobalFree(handle);
        return nullptr;
    }
    *value = effect;
    GlobalUnlock(handle);
    return handle;
}

constexpr const wchar_t* kAppTitle = L"PK2 Workbench PRO - by kahme247";
constexpr const wchar_t* kAppTitlePrefix = L"PK2 Workbench PRO";
constexpr const wchar_t* kAppCredit = L"by kahme247";
constexpr const wchar_t* kAppVersion = L"0.3.1";

std::wstring absolutePathWide(const fs::path& path) {
    const auto input = path.wstring();
    const auto required = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        throw pk2::Pk2Error("Could not resolve path: " + pk2::pathUtf8(path));
    }

    std::wstring result(required, L'\0');
    const auto written = GetFullPathNameW(input.c_str(),
                                          required,
                                          result.data(),
                                          nullptr);
    if (written == 0 || written >= required) {
        throw pk2::Pk2Error("Could not resolve path: " + pk2::pathUtf8(path));
    }
    result.resize(written);
    return result;
}

std::string filesystemErrorText(const std::string& action,
                                const fs::path& path,
                                const std::error_code& error) {
    return action + ": " + pk2::pathUtf8(path) + " (" + error.message() + ")";
}

void createDirectoriesChecked(const fs::path& path) {
    std::error_code error;
    fs::create_directories(path, error);
    if (error) {
        throw pk2::Pk2Error(filesystemErrorText("Could not create directory", path, error));
    }
}

bool existsChecked(const fs::path& path, const std::string& action) {
    std::error_code error;
    const auto exists = fs::exists(path, error);
    if (error) {
        throw pk2::Pk2Error(filesystemErrorText(action, path, error));
    }
    return exists;
}

bool isDirectoryChecked(const fs::path& path, const std::string& action) {
    std::error_code error;
    const auto isDirectory = fs::is_directory(path, error);
    if (error) {
        throw pk2::Pk2Error(filesystemErrorText(action, path, error));
    }
    return isDirectory;
}

bool isRegularFileChecked(const fs::path& path, const std::string& action) {
    std::error_code error;
    const auto isRegular = fs::is_regular_file(path, error);
    if (error) {
        throw pk2::Pk2Error(filesystemErrorText(action, path, error));
    }
    return isRegular;
}

HGLOBAL createHDropGlobal(const std::vector<fs::path>& paths) {
    std::vector<std::wstring> nativePaths;
    nativePaths.reserve(paths.size());
    std::size_t pathChars = 1;
    for (const auto& path : paths) {
        auto native = absolutePathWide(path);
        pathChars += native.size() + 1;
        nativePaths.push_back(std::move(native));
    }

    const auto byteCount = sizeof(DROPFILES) + pathChars * sizeof(wchar_t);
    auto handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, byteCount);
    if (handle == nullptr) {
        return nullptr;
    }

    auto* drop = static_cast<DROPFILES*>(GlobalLock(handle));
    if (drop == nullptr) {
        GlobalFree(handle);
        return nullptr;
    }

    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    auto* cursor = reinterpret_cast<wchar_t*>(
        reinterpret_cast<std::uint8_t*>(drop) + sizeof(DROPFILES));
    for (const auto& path : nativePaths) {
        const auto chars = path.size() + 1;
        CopyMemory(cursor, path.c_str(), chars * sizeof(wchar_t));
        cursor += chars;
    }
    *cursor = L'\0';
    GlobalUnlock(handle);
    return handle;
}

FORMATETC makeFormatEtc(CLIPFORMAT format) {
    FORMATETC result{};
    result.cfFormat = format;
    result.dwAspect = DVASPECT_CONTENT;
    result.lindex = -1;
    result.tymed = TYMED_HGLOBAL;
    return result;
}

class ShellDropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (iid == IID_IUnknown || iid == IID_IDropSource) {
            *object = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const auto count = InterlockedDecrement(&refCount_);
        if (count == 0) {
            delete this;
        }
        return static_cast<ULONG>(count);
    }

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override {
        if (escapePressed) {
            return DRAGDROP_S_CANCEL;
        }
        if ((keyState & MK_LBUTTON) == 0) {
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    ~ShellDropSource() = default;

    LONG refCount_{1};
};

class HdropDataObject final : public IDataObject {
public:
    explicit HdropDataObject(std::vector<fs::path> paths)
        : paths_(std::move(paths)),
          preferredDropEffect_(static_cast<CLIPFORMAT>(
              RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT))) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (iid == IID_IUnknown || iid == IID_IDataObject) {
            *object = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const auto count = InterlockedDecrement(&refCount_);
        if (count == 0) {
            delete this;
        }
        return static_cast<ULONG>(count);
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (format == nullptr || medium == nullptr) {
            return E_POINTER;
        }
        ZeroMemory(medium, sizeof(*medium));
        if (QueryGetData(format) != S_OK) {
            return DATA_E_FORMATETC;
        }

        HGLOBAL handle = nullptr;
        if (format->cfFormat == CF_HDROP) {
            handle = createHDropGlobal(paths_);
        } else if (format->cfFormat == preferredDropEffect_) {
            handle = createDropEffectGlobal(DROPEFFECT_COPY);
        }
        if (handle == nullptr) {
            return STG_E_MEDIUMFULL;
        }

        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = handle;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
        return DATA_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (format == nullptr) {
            return E_POINTER;
        }
        if ((format->tymed & TYMED_HGLOBAL) == 0 || format->dwAspect != DVASPECT_CONTENT) {
            return DATA_E_FORMATETC;
        }
        if (format->cfFormat == CF_HDROP || format->cfFormat == preferredDropEffect_) {
            return S_OK;
        }
        return DATA_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* formatOut) override {
        if (formatOut != nullptr) {
            formatOut->ptd = nullptr;
        }
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** enumFormats) override {
        if (enumFormats == nullptr) {
            return E_POINTER;
        }
        *enumFormats = nullptr;
        if (direction != DATADIR_GET) {
            return E_NOTIMPL;
        }

        FORMATETC formats[] = {
            makeFormatEtc(CF_HDROP),
            makeFormatEtc(preferredDropEffect_),
        };
        return SHCreateStdEnumFmtEtc(_countof(formats), formats, enumFormats);
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    ~HdropDataObject() = default;

    LONG refCount_{1};
    std::vector<fs::path> paths_;
    CLIPFORMAT preferredDropEffect_{};
};

std::wstring toWide(const std::string& text) {
    try {
        return pk2::widenUtf8(text);
    } catch (...) {
        if (text.empty()) {
            return {};
        }
        const int size = MultiByteToWideChar(CP_ACP,
                                             0,
                                             text.data(),
                                             static_cast<int>(text.size()),
                                             nullptr,
                                             0);
        if (size <= 0) {
            return L"An error occurred, but Windows could not decode the message text.";
        }
        std::wstring result(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_ACP,
                            0,
                            text.data(),
                            static_cast<int>(text.size()),
                            result.data(),
                            size);
        return result;
    }
}

std::string toUtf8(const std::wstring& text) {
    return pk2::narrowUtf8(text);
}

std::wstring safePathText(const fs::path& path) {
    try {
        return toWide(pk2::pathUtf8(path));
    } catch (...) {
        try {
            return path.wstring();
        } catch (...) {
            return L"<path unavailable>";
        }
    }
}

std::wstring exceptionMessage(const std::exception& ex) {
    if (const auto* filesystemError = dynamic_cast<const fs::filesystem_error*>(&ex)) {
        std::wstring message = L"Filesystem error";
        if (filesystemError->code()) {
            message += L": ";
            message += toWide(filesystemError->code().message());
        }
        if (!filesystemError->path1().empty()) {
            message += L"\r\nPath: ";
            message += safePathText(filesystemError->path1());
        }
        if (!filesystemError->path2().empty()) {
            message += L"\r\nTarget: ";
            message += safePathText(filesystemError->path2());
        }
        return message;
    }
    if (const auto* systemError = dynamic_cast<const std::system_error*>(&ex)) {
        std::wstring message = L"System error";
        if (systemError->code()) {
            message += L" ";
            message += std::to_wstring(systemError->code().value());
            message += L" (";
            message += toWide(systemError->code().category().name());
            message += L"): ";
            message += toWide(systemError->code().message());
        } else {
            message += L": ";
            message += toWide(ex.what());
        }
        return message;
    }
    return toWide(ex.what());
}

void setStatus(const std::wstring& text) {
    SendMessageW(gStatus, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

void showError(const std::exception& ex) {
    const auto message = exceptionMessage(ex);
    MessageBoxW(gMain, message.c_str(), L"PK2 Tool Error", MB_ICONERROR | MB_OK);
}

std::wstring formatSize(std::uint64_t bytes) {
    static constexpr const wchar_t* kUnits[] = {L"B", L"KB", L"MB", L"GB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }

    std::wostringstream out;
    if (unit == 0) {
        out << bytes << L" " << kUnits[unit];
    } else if (value >= 100.0) {
        out << std::fixed << std::setprecision(0) << value << L" " << kUnits[unit];
    } else if (value >= 10.0) {
        out << std::fixed << std::setprecision(1) << value << L" " << kUnits[unit];
    } else {
        out << std::fixed << std::setprecision(2) << value << L" " << kUnits[unit];
    }
    return out.str();
}

void setProgressVisible(bool visible) {
    if (gProgress != nullptr) {
        ShowWindow(gProgress, visible ? SW_SHOW : SW_HIDE);
        if (!visible) {
            SendMessageW(gProgress, PBM_SETPOS, 0, 0);
        }
    }
}

void setProgress(int percent) {
    percent = std::clamp(percent, 0, 100);
    if (gProgress != nullptr) {
        SendMessageW(gProgress, PBM_SETPOS, percent, 0);
    }
}

void setBusy(bool busy, const std::wstring& statusText) {
    gBusy = busy;
    EnableWindow(gTree, busy ? FALSE : TRUE);
    EnableWindow(gList, busy ? FALSE : TRUE);
    EnableWindow(gSearch, busy ? FALSE : TRUE);
    EnableWindow(gSearchButton, busy ? FALSE : TRUE);
    DragAcceptFiles(gMain, busy ? FALSE : TRUE);
    setProgressVisible(busy);
    if (busy) {
        setProgress(0);
    }

    const auto menu = GetMenu(gMain);
    if (menu != nullptr) {
        const auto enabled = busy ? MF_GRAYED : MF_ENABLED;
        EnableMenuItem(menu, IDM_FILE_OPEN, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_FILE_CLOSE, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_FILE_SAVE, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_FILE_SAVE_AS, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_FILE_EXIT, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_EXTRACT_SELECTED, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_EXTRACT_SHOWN, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_IMPORT_FILE, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_IMPORT_FOLDER, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_DELETE_ENTRY, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_TOOLS_SERVER_CONFIG, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_TOOLS_DEFRAGMENT, MF_BYCOMMAND | enabled);
        DrawMenuBar(gMain);
    }

    if (!statusText.empty()) {
        setStatus(statusText);
    }
}

void postProgress(HWND targetWindow,
                  std::uint64_t completed,
                  std::uint64_t total,
                  std::string_view currentPath,
                  const std::wstring& verb,
                  int& lastPercent,
                  std::chrono::steady_clock::time_point& lastPost) {
    int percent = 100;
    if (total > 0) {
        percent = static_cast<int>((completed * 100) / total);
    }

    const auto now = std::chrono::steady_clock::now();
    if (percent != 100 &&
        percent == lastPercent &&
        now - lastPost < std::chrono::milliseconds(120)) {
        return;
    }

    std::wstring message = verb;
    if (!currentPath.empty()) {
        message += L" ";
        message += toWide(std::string(currentPath));
    }
    if (total > 0) {
        message += L" (";
        message += formatSize(completed);
        message += L" / ";
        message += formatSize(total);
        message += L")";
    }

    auto update = std::make_unique<TaskProgress>();
    update->percent = percent;
    update->message = std::move(message);
    if (PostMessageW(targetWindow,
                     WM_APP_TASK_PROGRESS,
                     0,
                     reinterpret_cast<LPARAM>(update.get()))) {
        update.release();
        lastPercent = percent;
        lastPost = now;
    }
}

std::optional<fs::path> openFileDialog(const wchar_t* title,
                                       const wchar_t* filter,
                                       bool save) {
    wchar_t fileName[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = gMain;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!save) {
        ofn.Flags |= OFN_FILEMUSTEXIST;
    } else {
        ofn.Flags |= OFN_OVERWRITEPROMPT;
    }

    const auto ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) {
        return std::nullopt;
    }
    return fs::path(fileName);
}

std::optional<fs::path> browseFolder(const wchar_t* title) {
    BROWSEINFOW info{};
    info.hwndOwner = gMain;
    info.lpszTitle = title;
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&info);
    if (pidl == nullptr) {
        return std::nullopt;
    }

    wchar_t path[MAX_PATH]{};
    const auto ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (!ok) {
        return std::nullopt;
    }
    return fs::path(path);
}

INT_PTR CALLBACK passwordDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* password = reinterpret_cast<std::wstring*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    switch (message) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
        CheckDlgButton(dialog, IDC_DEFAULT_PASSWORD, BST_CHECKED);
        SetDlgItemTextW(dialog, IDC_PASSWORD_EDIT, L"169841");
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            password = reinterpret_cast<std::wstring*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
            wchar_t buffer[256]{};
            GetDlgItemTextW(dialog, IDC_PASSWORD_EDIT, buffer, 256);
            *password = buffer;
            if (IsDlgButtonChecked(dialog, IDC_DEFAULT_PASSWORD) == BST_CHECKED && password->empty()) {
                *password = L"169841";
            }
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    default:
        return FALSE;
    }
}

std::optional<std::string> askPassword() {
    std::wstring password = L"169841";
    const auto result = DialogBoxParamW(gInstance,
                                        MAKEINTRESOURCEW(IDD_PASSWORD),
                                        gMain,
                                        passwordDialogProc,
                                        reinterpret_cast<LPARAM>(&password));
    if (result != IDOK) {
        return std::nullopt;
    }
    return toUtf8(password);
}

struct ServerConfigDialogState {
    pk2::ServerConfig config;
    int selectedDivision{-1};
    int selectedGateway{-1};
};

std::wstring dialogText(HWND dialog, int controlId) {
    const auto length = GetWindowTextLengthW(GetDlgItem(dialog, controlId));
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetDlgItemTextW(dialog, controlId, value.data(), length + 1);
    }
    value.resize(static_cast<std::size_t>(length));
    return value;
}

void refreshGatewayList(HWND dialog, ServerConfigDialogState& state) {
    const auto list = GetDlgItem(dialog, IDC_GATEWAY_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    state.selectedGateway = -1;
    SetDlgItemTextW(dialog, IDC_GATEWAY_URL, L"");
    if (state.selectedDivision < 0 ||
        state.selectedDivision >= static_cast<int>(state.config.divisions.size())) {
        return;
    }
    const auto& gateways = state.config.divisions[static_cast<std::size_t>(state.selectedDivision)].gateways;
    for (const auto& gateway : gateways) {
        const auto text = toWide(gateway);
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    if (!gateways.empty()) {
        state.selectedGateway = 0;
        SendMessageW(list, LB_SETCURSEL, 0, 0);
        SetDlgItemTextW(dialog, IDC_GATEWAY_URL, toWide(gateways[0]).c_str());
    }
}

void selectDivision(HWND dialog, ServerConfigDialogState& state, int index) {
    if (index < 0 || index >= static_cast<int>(state.config.divisions.size())) {
        state.selectedDivision = -1;
        SetDlgItemTextW(dialog, IDC_DIVISION_NAME, L"");
        refreshGatewayList(dialog, state);
        return;
    }
    state.selectedDivision = index;
    SendDlgItemMessageW(dialog, IDC_DIVISION_LIST, LB_SETCURSEL, index, 0);
    const auto& division = state.config.divisions[static_cast<std::size_t>(index)];
    SetDlgItemTextW(dialog, IDC_DIVISION_NAME, toWide(division.name).c_str());
    refreshGatewayList(dialog, state);
}

void refreshDivisionList(HWND dialog, ServerConfigDialogState& state, int selection) {
    const auto list = GetDlgItem(dialog, IDC_DIVISION_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const auto& division : state.config.divisions) {
        const auto text = toWide(division.name);
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    selectDivision(dialog, state, selection);
}

void showDialogError(HWND dialog, const std::exception& ex) {
    const auto message = exceptionMessage(ex);
    MessageBoxW(dialog, message.c_str(), L"Server Configuration", MB_OK | MB_ICONERROR);
}

std::uint32_t dialogUnsigned(HWND dialog, int controlId, const char* label, std::uint32_t maximum) {
    BOOL translated = FALSE;
    const auto value = GetDlgItemInt(dialog, controlId, &translated, FALSE);
    if (!translated || value > maximum) {
        throw pk2::Pk2Error(std::string(label) + " must be between 0 and " +
                            std::to_string(maximum) + ".");
    }
    return value;
}

INT_PTR CALLBACK serverConfigDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ServerConfigDialogState*>(
        GetWindowLongPtrW(dialog, GWLP_USERDATA));
    try {
        switch (message) {
        case WM_INITDIALOG:
            state = reinterpret_cast<ServerConfigDialogState*>(lParam);
            SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
            SetDlgItemInt(dialog, IDC_CONTENT_ID, state->config.contentId, FALSE);
            SetDlgItemInt(dialog, IDC_SERVER_VERSION, state->config.version, FALSE);
            SetDlgItemInt(dialog, IDC_GATEWAY_PORT, state->config.port, FALSE);
            refreshDivisionList(dialog, *state, 0);
            return TRUE;
        case WM_COMMAND: {
            if (state == nullptr) {
                return FALSE;
            }
            const auto id = LOWORD(wParam);
            const auto notification = HIWORD(wParam);
            if (id == IDC_DIVISION_LIST && notification == LBN_SELCHANGE) {
                const auto selection = static_cast<int>(
                    SendDlgItemMessageW(dialog, IDC_DIVISION_LIST, LB_GETCURSEL, 0, 0));
                selectDivision(dialog, *state, selection);
                return TRUE;
            }
            if (id == IDC_GATEWAY_LIST && notification == LBN_SELCHANGE) {
                const auto selection = static_cast<int>(
                    SendDlgItemMessageW(dialog, IDC_GATEWAY_LIST, LB_GETCURSEL, 0, 0));
                state->selectedGateway = selection;
                if (selection >= 0 && state->selectedDivision >= 0) {
                    const auto& gateway = state->config.divisions[
                        static_cast<std::size_t>(state->selectedDivision)].gateways[
                        static_cast<std::size_t>(selection)];
                    SetDlgItemTextW(dialog, IDC_GATEWAY_URL, toWide(gateway).c_str());
                }
                return TRUE;
            }
            if (id == IDC_DIVISION_ADD) {
                const auto name = toUtf8(dialogText(dialog, IDC_DIVISION_NAME));
                state->config.divisions.push_back({name, {}});
                refreshDivisionList(dialog, *state,
                                    static_cast<int>(state->config.divisions.size() - 1));
                return TRUE;
            }
            if (id == IDC_DIVISION_UPDATE && state->selectedDivision >= 0) {
                state->config.divisions[static_cast<std::size_t>(state->selectedDivision)].name =
                    toUtf8(dialogText(dialog, IDC_DIVISION_NAME));
                refreshDivisionList(dialog, *state, state->selectedDivision);
                return TRUE;
            }
            if (id == IDC_DIVISION_REMOVE && state->selectedDivision >= 0) {
                const auto index = state->selectedDivision;
                state->config.divisions.erase(state->config.divisions.begin() + index);
                const auto next = std::min(index,
                    static_cast<int>(state->config.divisions.size()) - 1);
                refreshDivisionList(dialog, *state, next);
                return TRUE;
            }
            if ((id == IDC_GATEWAY_ADD || id == IDC_GATEWAY_UPDATE ||
                 id == IDC_GATEWAY_REMOVE) && state->selectedDivision < 0) {
                throw pk2::Pk2Error("Select or add a division first.");
            }
            if (id == IDC_GATEWAY_ADD) {
                auto& gateways = state->config.divisions[
                    static_cast<std::size_t>(state->selectedDivision)].gateways;
                gateways.push_back(toUtf8(dialogText(dialog, IDC_GATEWAY_URL)));
                refreshGatewayList(dialog, *state);
                state->selectedGateway = static_cast<int>(gateways.size() - 1);
                SendDlgItemMessageW(dialog, IDC_GATEWAY_LIST, LB_SETCURSEL,
                                    state->selectedGateway, 0);
                SetDlgItemTextW(dialog, IDC_GATEWAY_URL,
                                toWide(gateways.back()).c_str());
                return TRUE;
            }
            if (id == IDC_GATEWAY_UPDATE && state->selectedGateway >= 0) {
                auto& gateways = state->config.divisions[
                    static_cast<std::size_t>(state->selectedDivision)].gateways;
                gateways[static_cast<std::size_t>(state->selectedGateway)] =
                    toUtf8(dialogText(dialog, IDC_GATEWAY_URL));
                const auto selection = state->selectedGateway;
                refreshGatewayList(dialog, *state);
                state->selectedGateway = selection;
                SendDlgItemMessageW(dialog, IDC_GATEWAY_LIST, LB_SETCURSEL, selection, 0);
                SetDlgItemTextW(dialog, IDC_GATEWAY_URL,
                                toWide(gateways[static_cast<std::size_t>(selection)]).c_str());
                return TRUE;
            }
            if (id == IDC_GATEWAY_REMOVE && state->selectedGateway >= 0) {
                auto& gateways = state->config.divisions[
                    static_cast<std::size_t>(state->selectedDivision)].gateways;
                gateways.erase(gateways.begin() + state->selectedGateway);
                refreshGatewayList(dialog, *state);
                return TRUE;
            }
            if (id == IDOK) {
                if (state->selectedDivision >= 0) {
                    state->config.divisions[static_cast<std::size_t>(state->selectedDivision)].name =
                        toUtf8(dialogText(dialog, IDC_DIVISION_NAME));
                    if (state->selectedGateway >= 0) {
                        state->config.divisions[
                            static_cast<std::size_t>(state->selectedDivision)].gateways[
                            static_cast<std::size_t>(state->selectedGateway)] =
                            toUtf8(dialogText(dialog, IDC_GATEWAY_URL));
                    }
                }
                state->config.contentId = static_cast<std::uint8_t>(
                    dialogUnsigned(dialog, IDC_CONTENT_ID, "Content ID", 255));
                state->config.version = dialogUnsigned(
                    dialog, IDC_SERVER_VERSION, "Version", std::numeric_limits<std::uint32_t>::max());
                state->config.port = static_cast<std::uint16_t>(
                    dialogUnsigned(dialog, IDC_GATEWAY_PORT, "Gateway port", 65535));
                (void)pk2::serializeDivisionInfo(state->config);
                (void)pk2::serializeGatePort(state->config);
                (void)pk2::serializeServerVersion(state->config);
                EndDialog(dialog, IDOK);
                return TRUE;
            }
            if (id == IDCANCEL) {
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            return FALSE;
        }
        default:
            return FALSE;
        }
    } catch (const std::exception& ex) {
        showDialogError(dialog, ex);
        return TRUE;
    }
}

std::wstring treeItemText(HTREEITEM item) {
    wchar_t buffer[260]{};
    TVITEMW tvItem{};
    tvItem.mask = TVIF_TEXT;
    tvItem.hItem = item;
    tvItem.pszText = buffer;
    tvItem.cchTextMax = 260;
    TreeView_GetItem(gTree, &tvItem);
    return buffer;
}

std::string treeStoredPath(HTREEITEM item) {
    TVITEMW tvItem{};
    tvItem.mask = TVIF_PARAM;
    tvItem.hItem = item;
    if (!TreeView_GetItem(gTree, &tvItem) || tvItem.lParam == 0) {
        return {};
    }
    return *reinterpret_cast<std::string*>(tvItem.lParam);
}

std::string treeItemPath(HTREEITEM item) {
    return treeStoredPath(item);
}

bool hasFolderChildren(const std::string& folderPath) {
    for (const auto& entry : gArchive.children(folderPath)) {
        if (entry.type == pk2::EntryType::Folder) {
            return true;
        }
    }
    return false;
}

std::size_t directFileCount(const std::string& folderPath) {
    std::size_t count = 0;
    for (const auto& entry : gArchive.children(folderPath)) {
        if (entry.type == pk2::EntryType::File) {
            ++count;
        }
    }
    return count;
}

std::wstring archiveSummary() {
    std::size_t files = 0;
    std::size_t folders = 0;
    for (const auto& entry : gArchive.listTree()) {
        if (entry.type == pk2::EntryType::Folder) {
            ++folders;
        } else {
            ++files;
        }
    }

    std::wostringstream summary;
    summary << L"PK2 opened. " << files << L" files";
    if (folders > 0) {
        summary << L", " << folders << L" folders";
    } else {
        summary << L", flat root archive";
    }
    summary << L".";
    return summary.str();
}

int getIconIndex(const std::wstring& name, bool isFolder) {
    SHFILEINFOW sfi{};
    DWORD flags = SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON;
    DWORD attrs = isFolder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (SHGetFileInfoW(name.c_str(), attrs, &sfi, sizeof(sfi), flags)) {
        return sfi.iIcon;
    }
    return 0;
}

void copyTextToClipboard(const std::wstring& text) {
    if (OpenClipboard(gMain)) {
        EmptyClipboard();
        const auto bytes = (text.size() + 1) * sizeof(wchar_t);
        auto hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hMem != nullptr) {
            auto* p = GlobalLock(hMem);
            if (p != nullptr) {
                memcpy(p, text.c_str(), bytes);
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);
            }
        }
        CloseClipboard();
    }
}

void addDummyChild(HTREEITEM parent) {
    TVINSERTSTRUCTW insert{};
    insert.hParent = parent;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT;
    insert.item.pszText = const_cast<wchar_t*>(L"");
    TreeView_InsertItem(gTree, &insert);
}

void resetViews() {
    TreeView_DeleteAllItems(gTree);
    ListView_DeleteAllItems(gList);
    gCurrentFolder.clear();
}

void insertFolderTreeItems(HTREEITEM parent, const std::string& folderPath, bool oneLevelOnly) {
    for (const auto& entry : gArchive.children(folderPath)) {
        if (entry.type != pk2::EntryType::Folder) {
            continue;
        }
        const auto name = toWide(entry.name);
        TVINSERTSTRUCTW insert{};
        insert.hParent = parent;
        insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        insert.item.pszText = const_cast<wchar_t*>(name.c_str());
        insert.item.lParam = reinterpret_cast<LPARAM>(new std::string(entry.path));
        insert.item.iImage = getIconIndex(L"folder", true);
        insert.item.iSelectedImage = insert.item.iImage;
        const auto item = TreeView_InsertItem(gTree, &insert);
        if (oneLevelOnly) {
            if (hasFolderChildren(entry.path)) {
                addDummyChild(item);
            }
        } else {
            insertFolderTreeItems(item, entry.path, false);
        }
    }
}

void insertRootFilesTreeItem(HTREEITEM parent) {
    const auto rootFiles = directFileCount("");
    if (rootFiles == 0) {
        return;
    }

    std::wostringstream label;
    label << L"[root files] (" << rootFiles << L")";
    const auto text = label.str();
    TVINSERTSTRUCTW insert{};
    insert.hParent = parent;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    insert.item.pszText = const_cast<wchar_t*>(text.c_str());
    insert.item.lParam = reinterpret_cast<LPARAM>(new std::string());
    insert.item.iImage = getIconIndex(L"file.txt", false);
    insert.item.iSelectedImage = insert.item.iImage;
    TreeView_InsertItem(gTree, &insert);
}

void populateTree() {
    resetViews();
    TVINSERTSTRUCTW root{};
    root.hParent = TVI_ROOT;
    root.hInsertAfter = TVI_ROOT;
    root.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    root.item.pszText = const_cast<wchar_t*>(L"[root]");
    root.item.lParam = reinterpret_cast<LPARAM>(new std::string());
    root.item.iImage = getIconIndex(L"folder", true);
    root.item.iSelectedImage = root.item.iImage;
    const auto rootItem = TreeView_InsertItem(gTree, &root);
    insertFolderTreeItems(rootItem, "", true);
    insertRootFilesTreeItem(rootItem);
    TreeView_Expand(gTree, rootItem, TVE_EXPAND);
    TreeView_SelectItem(gTree, rootItem);
}

void expandTreeItem(HTREEITEM item) {
    const auto child = TreeView_GetChild(gTree, item);
    if (child == nullptr || !treeItemText(child).empty()) {
        return;
    }
    TreeView_DeleteItem(gTree, child);
    insertFolderTreeItems(item, treeItemPath(item), true);
}

void populateList(const std::string& folderPath) {
    ListView_DeleteAllItems(gList);
    gCurrentFolder = folderPath;

    int row = 0;
    for (const auto& entry : gArchive.children(folderPath)) {
        const auto name = toWide(entry.name);
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = row;
        item.pszText = const_cast<wchar_t*>(name.c_str());
        item.iImage = getIconIndex(entry.type == pk2::EntryType::Folder ? L"folder" : name,
                                   entry.type == pk2::EntryType::Folder);
        ListView_InsertItem(gList, &item);

        const auto type = entry.type == pk2::EntryType::Folder ? L"Folder" : L"File";
        ListView_SetItemText(gList, row, 1, const_cast<wchar_t*>(type));

        const auto size = entry.type == pk2::EntryType::File ? formatSize(entry.size) : L"";
        ListView_SetItemText(gList, row, 2, const_cast<wchar_t*>(size.c_str()));

        const auto path = toWide(entry.path);
        ListView_SetItemText(gList, row, 3, const_cast<wchar_t*>(path.c_str()));
        ++row;
    }
}

std::optional<std::string> selectedArchivePath() {
    const auto selectedList = ListView_GetNextItem(gList, -1, LVNI_SELECTED);
    if (selectedList >= 0) {
        wchar_t buffer[1024]{};
        ListView_GetItemText(gList, selectedList, 3, buffer, 1024);
        return toUtf8(buffer);
    }

    const auto selectedTree = TreeView_GetSelection(gTree);
    if (selectedTree != nullptr) {
        return treeItemPath(selectedTree);
    }
    return std::nullopt;
}

std::optional<std::string> archivePathForListRow(int row) {
    if (row < 0) {
        return std::nullopt;
    }
    wchar_t buffer[1024]{};
    ListView_GetItemText(gList, row, 3, buffer, 1024);
    std::string path = toUtf8(buffer);
    if (path.empty()) {
        return std::nullopt;
    }
    return path;
}

void updateTitle() {
    std::wstring title = kAppTitle;
    if (gLoaded && !gArchive.sourcePath().empty()) {
        title = kAppTitlePrefix;
        title += L" - ";
        title += gArchive.sourcePath().filename().wstring();
        if (gArchive.dirty()) {
            title += L" *";
        }
        title += L" (";
        title += kAppCredit;
        title += L")";
    }
    SetWindowTextW(gMain, title.c_str());
}

fs::path archiveNamedOutputFolder(const fs::path& destination) {
    auto stem = gArchive.sourcePath().stem().wstring();
    if (stem.empty()) {
        stem = L"PK2";
    }
    return destination / stem;
}

fs::path archivePathToFilesystemPath(const fs::path& base, const std::string& archivePath) {
    fs::path result = base;
    std::stringstream stream(pk2::normalizeArchivePath(archivePath));
    std::string part;
    while (std::getline(stream, part, '/')) {
        if (!part.empty()) {
            result /= pk2::archivePathPartToFilesystem(part);
        }
    }
    return result;
}

fs::path createDragStagingRoot() {
    wchar_t tempPath[MAX_PATH + 1]{};
    const auto length = GetTempPathW(MAX_PATH, tempPath);
    if (length == 0 || length > MAX_PATH) {
        throw pk2::Pk2Error("Could not locate the Windows temporary folder.");
    }

    const auto base = fs::path(tempPath) / L"PK2WorkbenchPRO" / L"DragOut";
    createDirectoriesChecked(base);
    for (int attempt = 0; attempt < 32; ++attempt) {
        std::wstring name = std::to_wstring(GetCurrentProcessId());
        name += L"-";
        name += std::to_wstring(GetTickCount64());
        name += L"-";
        name += std::to_wstring(attempt);
        const auto candidate = base / name;
        std::error_code error;
        if (fs::create_directory(candidate, error)) {
            return candidate;
        }
    }
    throw pk2::Pk2Error("Could not create a temporary drag extraction folder.");
}

void removeFolderQuietly(const fs::path& path) {
    std::error_code error;
    fs::remove_all(path, error);
}

void updateInlineProgress(std::uint64_t completed,
                          std::uint64_t total,
                          std::string_view currentPath,
                          const std::wstring& verb,
                          int& lastPercent,
                          std::chrono::steady_clock::time_point& lastPost) {
    int percent = 100;
    if (total > 0) {
        percent = static_cast<int>((completed * 100) / total);
    }

    const auto now = std::chrono::steady_clock::now();
    if (percent != 100 &&
        percent == lastPercent &&
        now - lastPost < std::chrono::milliseconds(120)) {
        return;
    }

    std::wstring message = verb;
    if (!currentPath.empty()) {
        message += L" ";
        message += toWide(std::string(currentPath));
    }
    if (total > 0) {
        message += L" (";
        message += formatSize(completed);
        message += L" / ";
        message += formatSize(total);
        message += L")";
    }
    setProgress(percent);
    setStatus(message);
    UpdateWindow(gProgress);
    UpdateWindow(gStatus);
    lastPercent = percent;
    lastPost = now;
}

bool beginShellCopyDrag(const std::vector<fs::path>& paths) {
    auto* dataObject = new HdropDataObject(paths);
    auto* dropSource = new ShellDropSource();
    DWORD effect = DROPEFFECT_NONE;
    const auto result = DoDragDrop(dataObject, dropSource, DROPEFFECT_COPY, &effect);
    dropSource->Release();
    dataObject->Release();

    if (result == DRAGDROP_S_CANCEL || effect == DROPEFFECT_NONE) {
        return false;
    }
    if (FAILED(result)) {
        throw pk2::Pk2Error("Windows shell drag/drop failed.");
    }
    return true;
}

void startArchiveDragOut(const std::string& archivePath) {
    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    if (!gLoaded) {
        setStatus(L"No PK2 loaded.");
        return;
    }
    if (archivePath.empty()) {
        setStatus(L"Drag a file or folder inside the PK2, not [root].");
        return;
    }
    if (!gArchive.find(archivePath)) {
        setStatus(L"Selected archive entry was not found.");
        return;
    }

    fs::path stagingRoot;
    try {
        stagingRoot = createDragStagingRoot();
        setBusy(true, L"Preparing drag extract...");
        int lastPercent = -1;
        auto lastPost = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        auto progress = [&](std::uint64_t completed,
                            std::uint64_t total,
                            std::string_view currentPath) {
            if (GetCurrentThreadId() != gUiThreadId) {
                return;
            }
            updateInlineProgress(completed,
                                 total,
                                 currentPath,
                                 L"Preparing drag extract",
                                 lastPercent,
                                 lastPost);
        };

        gArchive.extract(archivePath,
                         stagingRoot,
                         true,
                         pk2::OverwritePolicy::Replace,
                         progress);

        const auto stagedPath = archivePathToFilesystemPath(stagingRoot, archivePath);
        if (!existsChecked(stagedPath, "Could not inspect staged drag file")) {
            throw pk2::Pk2Error("The selected archive entry was not staged for dragging.");
        }

        setProgress(100);
        setBusy(false, L"Drop into a folder to copy the extracted item.");
        const auto dropped = beginShellCopyDrag({stagedPath});
        if (dropped) {
            setStatus(L"Drag extract complete.");
        } else {
            removeFolderQuietly(stagingRoot);
            setStatus(L"Drag cancelled.");
        }
    } catch (const std::exception& ex) {
        setBusy(false, L"");
        if (!stagingRoot.empty()) {
            removeFolderQuietly(stagingRoot);
        }
        showError(ex);
    }
}

std::string validRefreshFolder(const std::string& folderPath) {
    if (folderPath.empty()) {
        return {};
    }
    const auto info = gArchive.find(folderPath);
    if (info && info->type == pk2::EntryType::Folder) {
        return folderPath;
    }
    return {};
}

void postTaskComplete(HWND targetWindow, std::unique_ptr<TaskResult> result) {
    if (PostMessageW(targetWindow,
                     WM_APP_TASK_COMPLETE,
                     0,
                     reinterpret_cast<LPARAM>(result.get()))) {
        result.release();
    }
}

void startExtraction(const std::string& archivePath,
                     const fs::path& destination,
                     bool recurse) {
    setBusy(true, L"Extracting...");
    const auto targetWindow = gMain;
    std::thread([archivePath, destination, recurse, targetWindow]() {
        auto result = std::make_unique<TaskResult>();
        int lastPercent = -1;
        auto lastPost = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        auto progress = [targetWindow, &lastPercent, &lastPost](
                            std::uint64_t completed,
                            std::uint64_t total,
                            std::string_view currentPath) {
            postProgress(targetWindow,
                         completed,
                         total,
                         currentPath,
                         L"Extracting",
                         lastPercent,
                         lastPost);
        };

        try {
            gArchive.extract(archivePath,
                             destination,
                             recurse,
                             pk2::OverwritePolicy::Replace,
                             progress);
            result->ok = true;
            result->message = L"Extraction complete.";
        } catch (const std::exception& ex) {
            result->ok = false;
            result->message = exceptionMessage(ex);
        }
        postTaskComplete(targetWindow, std::move(result));
    }).detach();
}

void startImportPaths(std::vector<fs::path> paths, std::string targetFolder) {
    if (paths.empty()) {
        return;
    }

    setBusy(true, L"Importing...");
    const auto targetWindow = gMain;
    std::thread([paths = std::move(paths), targetFolder = std::move(targetFolder), targetWindow]() {
        auto result = std::make_unique<TaskResult>();
        int lastPercent = -1;
        auto lastPost = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        auto progress = [targetWindow, &lastPercent, &lastPost](
                            std::uint64_t completed,
                            std::uint64_t total,
                            std::string_view currentPath) {
            postProgress(targetWindow,
                         completed,
                         total,
                         currentPath,
                         L"Importing",
                         lastPercent,
                         lastPost);
        };

        try {
            for (const auto& path : paths) {
                const auto target = pk2::joinArchivePath(targetFolder, pk2::pathFileNameUtf8(path));
                if (isDirectoryChecked(path, "Could not inspect dropped path")) {
                    gArchive.importFolder(path, target, progress);
                } else if (isRegularFileChecked(path, "Could not inspect dropped path")) {
                    gArchive.importFile(path, target, progress);
                } else {
                    throw pk2::Pk2Error("Dropped path is not a file or folder: " +
                                        pk2::pathUtf8(path));
                }
            }
            gArchive.save();
            result->ok = true;
            result->archiveChanged = true;
            result->refreshFolder = targetFolder;
            result->message = paths.size() == 1
                                  ? L"Import complete and saved."
                                  : L"Imports complete and saved.";
        } catch (const std::exception& ex) {
            result->ok = false;
            result->archiveChanged = true;
            result->refreshFolder = targetFolder;
            result->message = exceptionMessage(ex);
        }
        postTaskComplete(targetWindow, std::move(result));
    }).detach();
}

void openArchivePath(const fs::path& archivePath) {
    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    try {
        const auto password = askPassword();
        if (!password) {
            return;
        }
        resetViews();
        setBusy(true, L"Loading PK2 metadata...");

        const auto archivePassword = *password;
        const auto targetWindow = gMain;
        std::thread([archivePath, archivePassword, targetWindow]() {
            auto result = std::make_unique<OpenResult>();
            try {
                result->archive = pk2::Pk2Archive::open(archivePath, archivePassword);
                result->ok = true;
                result->message = L"PK2 opened.";
            } catch (const std::exception& ex) {
                result->ok = false;
                result->message = exceptionMessage(ex);
            }

            if (!PostMessageW(targetWindow,
                              WM_APP_OPEN_COMPLETE,
                              0,
                              reinterpret_cast<LPARAM>(result.get()))) {
                return;
            }
            result.release();
        }).detach();
    } catch (const std::exception& ex) {
        setBusy(false, L"");
        showError(ex);
    }
}

void closeArchive() {
    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    gArchive = pk2::Pk2Archive();
    gLoaded = false;
    resetViews();
    setStatus(L"No PK2 loaded.");
    updateTitle();
}

void saveArchive(bool saveAs) {
    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    if (!gLoaded) {
        setStatus(L"No PK2 loaded.");
        return;
    }
    try {
        if (saveAs || gArchive.sourcePath().empty()) {
            const auto file = openFileDialog(L"Save PK2 As", L"PK2 files (*.pk2)\0*.pk2\0All files\0*.*\0", true);
            if (!file) {
                return;
            }
            gArchive.saveAs(*file);
        } else {
            gArchive.save();
        }
        setStatus(L"PK2 saved.");
        updateTitle();
    } catch (const std::exception& ex) {
        showError(ex);
    }
}

void defragmentArchive() {
    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    if (!gLoaded || gArchive.sourcePath().empty()) {
        setStatus(L"No PK2 loaded.");
        return;
    }
    if (MessageBoxW(gMain,
                    L"Defragmenting will repack the entire PK2 archive to remove slack space and minimize file size.\n\n"
                    L"Do you want to proceed?",
                    L"Defragment PK2",
                    MB_ICONQUESTION | MB_YESNO) != IDYES) {
        return;
    }
    setStatus(L"Defragmenting PK2...");
    try {
        gArchive.saveDefragmented();
        setStatus(L"PK2 successfully defragmented and optimized.");
        populateTree();
        populateList(gCurrentFolder);
        updateTitle();
        MessageBoxW(gMain, L"PK2 archive successfully defragmented and optimized!", L"Defragment PK2", MB_ICONINFORMATION);
    } catch (const std::exception& ex) {
        showError(ex);
    }
}

void extractPath(const std::string& path, bool recurse, bool wrapInArchiveFolder) {
    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    if (!gLoaded) {
        setStatus(L"No PK2 loaded.");
        return;
    }
    try {
        const auto folder = browseFolder(L"Choose extract destination");
        if (!folder) {
            return;
        }
        const auto destination = wrapInArchiveFolder ? archiveNamedOutputFolder(*folder) : *folder;
        startExtraction(path, destination, recurse);
    } catch (const std::exception& ex) {
        showError(ex);
    }
}

void importFile() {
    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    if (!gLoaded) {
        setStatus(L"No PK2 loaded.");
        return;
    }
    try {
        const auto file = openFileDialog(L"Import File", L"All files\0*.*\0", false);
        if (!file) {
            return;
        }
        startImportPaths({*file}, gCurrentFolder);
    } catch (const std::exception& ex) {
        showError(ex);
    }
}

void importFolder() {
    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    if (!gLoaded) {
        setStatus(L"No PK2 loaded.");
        return;
    }
    try {
        const auto folder = browseFolder(L"Choose folder to import");
        if (!folder) {
            return;
        }
        startImportPaths({*folder}, gCurrentFolder);
    } catch (const std::exception& ex) {
        showError(ex);
    }
}

void deleteSelected() {
    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    if (!gLoaded) {
        setStatus(L"No PK2 loaded.");
        return;
    }
    const auto path = selectedArchivePath();
    if (!path || path->empty()) {
        setStatus(L"Select an entry first.");
        return;
    }
    std::wstring question = L"Delete \"";
    question += toWide(*path);
    question += L"\" from the PK2?";
    if (MessageBoxW(gMain, question.c_str(), L"Delete Entry", MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    try {
        gArchive.deleteEntry(*path);
        gArchive.save();
        populateTree();
        populateList(gCurrentFolder);
        setStatus(L"Entry deleted and saved.");
        updateTitle();
    } catch (const std::exception& ex) {
        showError(ex);
    }
}

bool equalsAsciiInsensitive(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

std::optional<std::string> findRootConfigPath(std::string_view fileName) {
    for (const auto& entry : gArchive.children("")) {
        if (entry.type == pk2::EntryType::File && equalsAsciiInsensitive(entry.name, fileName)) {
            return entry.path;
        }
    }
    return std::nullopt;
}

void editServerConfiguration() {
    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    if (!gLoaded) {
        setStatus(L"Open Media.pk2 first.");
        return;
    }

    try {
        const auto divisionPath = findRootConfigPath("DIVISIONINFO.TXT");
        const auto portPath = findRootConfigPath("GATEPORT.TXT");
        auto versionPath = findRootConfigPath("SV.T");
        if (!versionPath) versionPath = findRootConfigPath("SV.TXT");
        if (!versionPath) versionPath = findRootConfigPath("version.txt");

        std::vector<std::uint8_t> divisionBytes;
        if (divisionPath) {
            divisionBytes = gArchive.readFile(*divisionPath);
        }

        std::vector<std::uint8_t> portBytes;
        if (portPath) {
            portBytes = gArchive.readFile(*portPath);
        }

        std::vector<std::uint8_t> versionBytes;
        if (versionPath) {
            versionBytes = gArchive.readFile(*versionPath);
        }

        ServerConfigDialogState state;
        state.config = pk2::parseServerConfig(divisionBytes, portBytes, versionBytes);

        const auto result = DialogBoxParamW(gInstance,
                                            MAKEINTRESOURCEW(IDD_SERVER_CONFIG),
                                            gMain,
                                            serverConfigDialogProc,
                                            reinterpret_cast<LPARAM>(&state));
        if (result != IDOK) {
            return;
        }

        auto divisionOut = pk2::serializeDivisionInfo(state.config);
        auto portOut = pk2::serializeGatePort(state.config);
        auto versionOut = pk2::serializeServerVersion(state.config);

        gArchive.importFileBytes(std::move(divisionOut),
                                divisionPath.value_or("DIVISIONINFO.TXT"));
        gArchive.importFileBytes(std::move(portOut),
                                portPath.value_or("GATEPORT.TXT"));
        gArchive.importFileBytes(std::move(versionOut),
                                versionPath.value_or("SV.T"));

        gArchive.save();
        populateTree();
        populateList(validRefreshFolder(gCurrentFolder));
        updateTitle();
        setStatus(L"Server configuration updated and saved.");
    } catch (const std::exception& ex) {
        showError(ex);
    }
}

void openArchive() {
    const auto file = openFileDialog(L"Open PK2", L"PK2 files (*.pk2)\0*.pk2\0All files\0*.*\0", false);
    if (file) {
        openArchivePath(*file);
    }
}

std::wstring searchText() {
    const auto length = GetWindowTextLengthW(gSearch);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(gSearch, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return text;
}

void populateSearchResults() {
    const auto query = searchText();
    if (query.empty()) {
        populateList(gCurrentFolder);
        return;
    }

    ListView_DeleteAllItems(gList);
    int row = 0;
    for (const auto& entry : gArchive.listTree()) {
        auto searchable = toWide(entry.path);
        std::transform(searchable.begin(), searchable.end(), searchable.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (searchable.find(query) == std::wstring::npos) {
            continue;
        }

        const auto name = toWide(entry.name);
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = row;
        item.pszText = const_cast<wchar_t*>(name.c_str());
        item.iImage = getIconIndex(entry.type == pk2::EntryType::Folder ? L"folder" : name,
                                   entry.type == pk2::EntryType::Folder);
        ListView_InsertItem(gList, &item);
        const auto type = entry.type == pk2::EntryType::Folder ? L"Folder" : L"File";
        ListView_SetItemText(gList, row, 1, const_cast<wchar_t*>(type));
        const auto size = entry.type == pk2::EntryType::File ? formatSize(entry.size) : L"";
        ListView_SetItemText(gList, row, 2, const_cast<wchar_t*>(size.c_str()));
        const auto path = toWide(entry.path);
        ListView_SetItemText(gList, row, 3, const_cast<wchar_t*>(path.c_str()));
        ++row;
    }
    std::wostringstream status;
    status << L"Found " << row << L" matching item" << (row == 1 ? L"." : L"s.");
    setStatus(status.str());
}

LRESULT CALLBACK searchEditProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            if (gLoaded) {
                populateSearchResults();
            }
            return 0;
        } else if (wParam == VK_ESCAPE) {
            SetWindowTextW(hwnd, L"");
            if (gLoaded) {
                populateList(gCurrentFolder);
            }
            return 0;
        }
    }
    return CallWindowProcW(gOriginalSearchProc, hwnd, uMsg, wParam, lParam);
}

bool isTextEntry(const pk2::EntryInfo& entry);

void showTreeContextMenu(HWND treeHwnd, int screenX, int screenY) {
    if (!gLoaded || gBusy) return;

    TVHITTESTINFO ht{};
    POINT pt = {screenX, screenY};
    ScreenToClient(treeHwnd, &pt);
    ht.pt = pt;
    HTREEITEM hitItem = TreeView_HitTest(treeHwnd, &ht);
    if (hitItem != nullptr) {
        TreeView_SelectItem(treeHwnd, hitItem);
    }
    const auto selected = selectedArchivePath();
    if (!selected) return;

    const auto info = gArchive.find(*selected);
    const bool isRoot = selected->empty();
    const bool isFolder = isRoot || (info && info->type == pk2::EntryType::Folder);

    HMENU menu = CreatePopupMenu();
    if (isFolder) {
        AppendMenuW(menu, MF_STRING, IDM_CONTEXT_EXTRACT, L"&Extract Folder...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_CONTEXT_IMPORT_FILE, L"&Import File Here...");
        AppendMenuW(menu, MF_STRING, IDM_CONTEXT_IMPORT_FOLDER, L"Import &Folder Here...");
        if (!isRoot) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDM_CONTEXT_COPY_PATH, L"&Copy Archive Path");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDM_CONTEXT_DELETE, L"&Delete Folder\tDel");
        }
    } else {
        if (info && isTextEntry(*info)) {
            AppendMenuW(menu, MF_STRING, IDM_CONTEXT_EDIT, L"&Edit Text File...\tDouble-Click");
            SetMenuDefaultItem(menu, IDM_CONTEXT_EDIT, FALSE);
        }
        AppendMenuW(menu, MF_STRING, IDM_CONTEXT_EXTRACT, L"&Extract File...");
        AppendMenuW(menu, MF_STRING, IDM_CONTEXT_REPLACE, L"&Replace / Import File...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_CONTEXT_COPY_PATH, L"&Copy Archive Path");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_CONTEXT_DELETE, L"&Delete File\tDel");
    }

    SetForegroundWindow(gMain);
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_TOPALIGN | TPM_LEFTALIGN, screenX, screenY, gMain, nullptr);
    DestroyMenu(menu);
}

void showListContextMenu(HWND listHwnd, int screenX, int screenY) {
    if (!gLoaded || gBusy) return;

    LVHITTESTINFO ht{};
    POINT pt = {screenX, screenY};
    ScreenToClient(listHwnd, &pt);
    ht.pt = pt;
    int hitItem = ListView_HitTest(listHwnd, &ht);
    if (hitItem >= 0) {
        ListView_SetItemState(listHwnd, hitItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }

    const auto selected = selectedArchivePath();
    if (!selected) return;

    const auto info = gArchive.find(*selected);
    if (!info) return;

    HMENU menu = CreatePopupMenu();
    if (info->type == pk2::EntryType::File) {
        if (isTextEntry(*info)) {
            AppendMenuW(menu, MF_STRING, IDM_CONTEXT_EDIT, L"&Edit Text File...\tDouble-Click");
            SetMenuDefaultItem(menu, IDM_CONTEXT_EDIT, FALSE);
        }
        AppendMenuW(menu, MF_STRING, IDM_CONTEXT_EXTRACT, L"&Extract File...");
        AppendMenuW(menu, MF_STRING, IDM_CONTEXT_REPLACE, L"&Replace / Import File...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_CONTEXT_COPY_PATH, L"&Copy Archive Path");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_CONTEXT_DELETE, L"&Delete File\tDel");
    } else {
        AppendMenuW(menu, MF_STRING, IDM_CONTEXT_EXTRACT, L"&Extract Folder...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_CONTEXT_IMPORT_FILE, L"&Import File Here...");
        AppendMenuW(menu, MF_STRING, IDM_CONTEXT_IMPORT_FOLDER, L"Import &Folder Here...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_CONTEXT_COPY_PATH, L"&Copy Archive Path");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_CONTEXT_DELETE, L"&Delete Folder\tDel");
    }

    SetForegroundWindow(gMain);
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_TOPALIGN | TPM_LEFTALIGN, screenX, screenY, gMain, nullptr);
    DestroyMenu(menu);
}

void importFileIntoFolder(const std::string& targetFolder) {
    if (gBusy || !gLoaded) return;
    try {
        const auto file = openFileDialog(L"Import File", L"All files\0*.*\0", false);
        if (!file) return;
        startImportPaths({*file}, targetFolder);
    } catch (const std::exception& ex) {
        showError(ex);
    }
}

void importFolderIntoFolder(const std::string& targetFolder) {
    if (gBusy || !gLoaded) return;
    try {
        const auto folder = browseFolder(L"Choose folder to import");
        if (!folder) return;
        startImportPaths({*folder}, targetFolder);
    } catch (const std::exception& ex) {
        showError(ex);
    }
}

void importFileOver(const std::string& targetFilePath) {
    if (gBusy || !gLoaded) return;
    try {
        const auto file = openFileDialog(L"Replace File", L"All files\0*.*\0", false);
        if (!file) return;
        std::ifstream in(*file, std::ios::binary);
        if (!in) throw pk2::Pk2Error("Could not open replacement file: " + pk2::pathUtf8(*file));
        std::vector<std::uint8_t> newBytes((std::istreambuf_iterator<char>(in)),
                                           std::istreambuf_iterator<char>());
        gArchive.importFileBytes(std::move(newBytes), targetFilePath);
        gArchive.save();
        populateList(gCurrentFolder);
        updateTitle();
        setStatus(L"Replaced and saved: " + toWide(targetFilePath));
    } catch (const std::exception& ex) {
        showError(ex);
    }
}

void showMd5Helper() {
    const auto password = askPassword();
    if (!password) {
        return;
    }
    const auto digest = pk2::md5Hex(*password);
    std::wstring message = L"MD5:\r\n";
    message += toWide(digest);
    MessageBoxW(gMain, message.c_str(), L"Private-server MD5 Helper", MB_OK | MB_ICONINFORMATION);
}

void showAbout() {
    std::wstring message = L"PK2 Workbench PRO\r\n";
    message += kAppCredit;
    message += L"\r\nVersion ";
    message += kAppVersion;
    message += L"\r\n\r\nAll-in-one PK2 editor, extractor, importer, and archive browser.";
    MessageBoxW(gMain, message.c_str(), L"About PK2 Workbench PRO", MB_OK | MB_ICONINFORMATION);
}

struct TextEditorState {
    std::wstring title;
    std::wstring text;
};

INT_PTR CALLBACK textEditorDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<TextEditorState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<TextEditorState*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SetWindowTextW(dialog, state->title.c_str());
        SendDlgItemMessageW(dialog, IDC_TEXT_EDIT, EM_SETLIMITTEXT,
                            64u * 1024u * 1024u, 0);
        SetDlgItemTextW(dialog, IDC_TEXT_EDIT, state->text.c_str());
        SendDlgItemMessageW(dialog, IDC_TEXT_EDIT, EM_SETSEL, 0, 0);
        return TRUE;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(wParam) == IDOK && state != nullptr) {
            const auto edit = GetDlgItem(dialog, IDC_TEXT_EDIT);
            const auto length = GetWindowTextLengthW(edit);
            std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
            GetWindowTextW(edit, text.data(), length + 1);
            text.resize(static_cast<std::size_t>(length));
            state->text = std::move(text);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

bool isTextEntry(const pk2::EntryInfo& entry) {
    if (entry.type != pk2::EntryType::File || entry.name.size() < 4) {
        return false;
    }
    auto extension = entry.name.substr(entry.name.size() - 4);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return extension == ".txt";
}

enum class TextFileEncoding {
    Utf8,
    Utf8Bom,
    Ansi,
    Utf16LittleEndian,
    Utf16BigEndian
};

struct DecodedTextFile {
    std::wstring text;
    TextFileEncoding encoding{TextFileEncoding::Utf8};
};

DecodedTextFile decodeTextFile(const std::vector<std::uint8_t>& bytes) {
    DecodedTextFile result;
    std::size_t offset = 0;
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        result.encoding = TextFileEncoding::Utf16LittleEndian;
        offset = 2;
    } else if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        result.encoding = TextFileEncoding::Utf16BigEndian;
        offset = 2;
    }
    if (offset != 0) {
        if ((bytes.size() - offset) % 2 != 0) {
            throw pk2::Pk2Error("The UTF-16 text file has an incomplete character.");
        }
        result.text.reserve((bytes.size() - offset) / 2);
        for (std::size_t i = offset; i < bytes.size(); i += 2) {
            const auto value = result.encoding == TextFileEncoding::Utf16LittleEndian
                                   ? static_cast<std::uint16_t>(bytes[i] | (bytes[i + 1] << 8))
                                   : static_cast<std::uint16_t>((bytes[i] << 8) | bytes[i + 1]);
            result.text.push_back(static_cast<wchar_t>(value));
        }
        return result;
    }

    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        result.encoding = TextFileEncoding::Utf8Bom;
        offset = 3;
    }
    const auto* data = bytes.empty()
                           ? ""
                           : reinterpret_cast<const char*>(bytes.data() + offset);
    const auto size = static_cast<int>(bytes.size() - offset);
    const auto wideSize = size == 0 ? 0 : MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, data, size, nullptr, 0);
    if (size == 0 || wideSize > 0) {
        result.text.resize(static_cast<std::size_t>(wideSize));
        if (wideSize > 0) {
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, size,
                                result.text.data(), wideSize);
        }
        return result;
    }

    result.encoding = TextFileEncoding::Ansi;
    const auto ansiSize = MultiByteToWideChar(CP_ACP, 0, data, size, nullptr, 0);
    if (ansiSize <= 0) {
        throw pk2::Pk2Error("Windows could not decode this text file.");
    }
    result.text.resize(static_cast<std::size_t>(ansiSize));
    MultiByteToWideChar(CP_ACP, 0, data, size, result.text.data(), ansiSize);
    return result;
}

std::vector<std::uint8_t> encodeTextFile(const std::wstring& text,
                                         TextFileEncoding encoding) {
    std::vector<std::uint8_t> output;
    if (encoding == TextFileEncoding::Utf16LittleEndian ||
        encoding == TextFileEncoding::Utf16BigEndian) {
        output = encoding == TextFileEncoding::Utf16LittleEndian
                     ? std::vector<std::uint8_t>{0xFF, 0xFE}
                     : std::vector<std::uint8_t>{0xFE, 0xFF};
        output.reserve(2 + text.size() * 2);
        for (const auto value : text) {
            const auto code = static_cast<std::uint16_t>(value);
            if (encoding == TextFileEncoding::Utf16LittleEndian) {
                output.push_back(static_cast<std::uint8_t>(code & 0xFF));
                output.push_back(static_cast<std::uint8_t>(code >> 8));
            } else {
                output.push_back(static_cast<std::uint8_t>(code >> 8));
                output.push_back(static_cast<std::uint8_t>(code & 0xFF));
            }
        }
        return output;
    }

    std::string encoded;
    if (encoding == TextFileEncoding::Ansi) {
        const auto size = text.empty() ? 0 : WideCharToMultiByte(
            CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        encoded.resize(static_cast<std::size_t>(size));
        if (size > 0) {
            WideCharToMultiByte(CP_ACP, 0, text.data(), static_cast<int>(text.size()),
                                encoded.data(), size, nullptr, nullptr);
        }
    } else {
        encoded = toUtf8(text);
    }
    if (encoding == TextFileEncoding::Utf8Bom) {
        output = {0xEF, 0xBB, 0xBF};
    }
    output.insert(output.end(), encoded.begin(), encoded.end());
    return output;
}

void editTextEntry(const pk2::EntryInfo& entry) {
    try {
        const auto bytes = gArchive.readFile(entry.path);
        if (bytes.size() > 64u * 1024u * 1024u) {
            throw pk2::Pk2Error("Text files larger than 64 MB cannot be edited in the app.");
        }
        const auto decoded = decodeTextFile(bytes);
        if (decoded.text.find(L'\0') != std::wstring::npos) {
            throw pk2::Pk2Error("This .txt file contains null characters and cannot be safely edited.");
        }

        TextEditorState state;
        state.title = L"Edit " + toWide(entry.path);
        state.text = decoded.text;
        if (DialogBoxParamW(gInstance, MAKEINTRESOURCEW(IDD_TEXT_EDITOR), gMain,
                            textEditorDialogProc, reinterpret_cast<LPARAM>(&state)) != IDOK) {
            return;
        }

        auto output = encodeTextFile(state.text, decoded.encoding);
        gArchive.importFileBytes(std::move(output), entry.path);
        gArchive.save();
        updateTitle();
        populateSearchResults();
        setStatus(L"Text file updated and PK2 saved.");
    } catch (const std::exception& ex) {
        showError(ex);
    }
}

void createControls(HWND window) {
    gTree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_TRACKSELECT,
                            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_TREE), gInstance, nullptr);
    gList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS,
                            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_LIST), gInstance, nullptr);
    gSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                              0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_SEARCH), gInstance, nullptr);
    SendMessageW(gSearch, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"Search files and folders inside the PK2... (Enter to search, Esc to clear)"));
    gSearchButton = CreateWindowExW(0, L"BUTTON", L"Search",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                    0, 0, 0, 0, window,
                                    reinterpret_cast<HMENU>(IDC_SEARCH_BUTTON), gInstance, nullptr);
    gStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                              WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                              window, reinterpret_cast<HMENU>(IDC_STATUS), gInstance, nullptr);
    gProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
                                WS_CHILD | PBS_SMOOTH,
                                0, 0, 0, 0,
                                gStatus,
                                reinterpret_cast<HMENU>(IDC_PROGRESS),
                                gInstance,
                                nullptr);
    SendMessageW(gProgress, PBM_SETRANGE32, 0, 100);

    SetWindowTheme(gTree, L"Explorer", nullptr);
    SetWindowTheme(gList, L"Explorer", nullptr);

    SHFILEINFOW sfi{};
    HIMAGELIST sysSmall = reinterpret_cast<HIMAGELIST>(
        SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi),
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
    if (sysSmall != nullptr) {
        TreeView_SetImageList(gTree, sysSmall, TVSIL_NORMAL);
        ListView_SetImageList(gList, sysSmall, LVSIL_SMALL);
    }

    gOriginalSearchProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(gSearch, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(searchEditProc)));

    ListView_SetExtendedListViewStyle(gList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = const_cast<wchar_t*>(L"Name");
    column.cx = 260;
    ListView_InsertColumn(gList, 0, &column);
    column.pszText = const_cast<wchar_t*>(L"Type");
    column.cx = 80;
    ListView_InsertColumn(gList, 1, &column);
    column.pszText = const_cast<wchar_t*>(L"Size");
    column.cx = 120;
    ListView_InsertColumn(gList, 2, &column);
    column.pszText = const_cast<wchar_t*>(L"Path");
    column.cx = 320;
    ListView_InsertColumn(gList, 3, &column);
}

void layoutControls(HWND window) {
    RECT rect{};
    GetClientRect(window, &rect);
    SendMessageW(gStatus, WM_SIZE, 0, 0);

    RECT statusRect{};
    GetWindowRect(gStatus, &statusRect);
    const auto statusHeight = statusRect.bottom - statusRect.top;
    const auto width = rect.right - rect.left;
    const auto height = rect.bottom - rect.top - statusHeight;
    const auto treeWidth = std::clamp(gTreeWidth, 120, std::max(120, static_cast<int>(width) - 200));
    gTreeWidth = treeWidth;

    const LONG progressWidth = 200;
    const auto statusTextWidth = std::max<LONG>(0, width - progressWidth - 8);
    int statusParts[2] = {static_cast<int>(statusTextWidth), -1};
    SendMessageW(gStatus, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(statusParts));
    RECT progressRect{};
    SendMessageW(gStatus, SB_GETRECT, 1, reinterpret_cast<LPARAM>(&progressRect));
    MoveWindow(gProgress,
               progressRect.left + 4,
               progressRect.top + 3,
               std::max<LONG>(0, progressRect.right - progressRect.left - 8),
               std::max<LONG>(0, progressRect.bottom - progressRect.top - 6),
               TRUE);

    constexpr int searchHeight = 28;
    MoveWindow(gTree, 0, 0, treeWidth, height, TRUE);
    constexpr LONG searchButtonWidth = 76;
    MoveWindow(gSearch, treeWidth + 6, 4,
               std::max<LONG>(0, width - treeWidth - searchButtonWidth - 14),
               searchHeight - 8, TRUE);
    MoveWindow(gSearchButton, width - searchButtonWidth - 4, 3,
               searchButtonWidth, searchHeight - 6, TRUE);
    MoveWindow(gList, treeWidth + 4, searchHeight, width - treeWidth - 4,
               std::max<LONG>(0, height - searchHeight), TRUE);
}

void handleNotify(LPARAM lParam) {
    const auto* header = reinterpret_cast<NMHDR*>(lParam);
    if (header->hwndFrom == gTree && header->code == TVN_DELETEITEMW) {
        const auto* change = reinterpret_cast<NMTREEVIEWW*>(lParam);
        if (change->itemOld.lParam != 0) {
            delete reinterpret_cast<std::string*>(change->itemOld.lParam);
        }
        return;
    }
    if (header->hwndFrom == gTree && header->code == TVN_ITEMEXPANDINGW) {
        const auto* change = reinterpret_cast<NMTREEVIEWW*>(lParam);
        if (change->action == TVE_EXPAND) {
            expandTreeItem(change->itemNew.hItem);
        }
        return;
    }
    if (header->hwndFrom == gTree && header->code == TVN_BEGINDRAGW) {
        const auto* change = reinterpret_cast<NMTREEVIEWW*>(lParam);
        TreeView_SelectItem(gTree, change->itemNew.hItem);
        startArchiveDragOut(treeItemPath(change->itemNew.hItem));
        return;
    }
    if (header->hwndFrom == gTree && header->code == TVN_SELCHANGEDW) {
        const auto* change = reinterpret_cast<NMTREEVIEWW*>(lParam);
        populateList(treeItemPath(change->itemNew.hItem));
        return;
    }

    if (header->hwndFrom == gList && header->code == LVN_BEGINDRAG) {
        const auto* drag = reinterpret_cast<NMLISTVIEW*>(lParam);
        ListView_SetItemState(gList, drag->iItem, LVIS_SELECTED, LVIS_SELECTED);
        if (const auto path = archivePathForListRow(drag->iItem)) {
            startArchiveDragOut(*path);
        }
        return;
    }

    if (header->hwndFrom == gList && header->code == NM_DBLCLK) {
        const auto selected = selectedArchivePath();
        if (!selected) {
            return;
        }
        const auto info = gArchive.find(*selected);
        if (info && info->type == pk2::EntryType::Folder) {
            populateList(*selected);
        } else if (info && isTextEntry(*info)) {
            editTextEntry(*info);
        }
    }
}

void handleDropFiles(HDROP drop) {
    std::vector<fs::path> paths;
    const auto count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        const auto length = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(length + 1, L'\0');
        DragQueryFileW(drop, i, path.data(), length + 1);
        path.resize(length);
        paths.emplace_back(path);
    }
    DragFinish(drop);

    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    if (!gLoaded) {
        if (paths.size() != 1) {
            setStatus(L"Drop one .pk2 file to open it.");
            return;
        }
        auto extension = paths.front().extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (extension != L".pk2" ||
            !isRegularFileChecked(paths.front(), "Could not inspect dropped PK2")) {
            setStatus(L"Drop a valid .pk2 file to open it.");
            return;
        }
        openArchivePath(paths.front());
        return;
    }
    startImportPaths(std::move(paths), gCurrentFolder);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        createControls(window);
        DragAcceptFiles(window, TRUE);
        setStatus(L"No PK2 loaded.");
        return 0;
    case WM_SIZE:
        layoutControls(window);
        return 0;
    case WM_NOTIFY:
        handleNotify(lParam);
        return 0;
    case WM_COMMAND:
        if (gBusy) {
            setStatus(L"Please wait for the current operation to finish.");
            return 0;
        }
        if (LOWORD(wParam) == IDC_SEARCH_BUTTON && HIWORD(wParam) == BN_CLICKED) {
            if (gLoaded) {
                populateSearchResults();
            } else {
                setStatus(L"Open a PK2 before searching.");
            }
            return 0;
        }
        switch (LOWORD(wParam)) {
        case IDM_FILE_OPEN:
            openArchive();
            break;
        case IDM_FILE_CLOSE:
            closeArchive();
            break;
        case IDM_FILE_SAVE:
            saveArchive(false);
            break;
        case IDM_FILE_SAVE_AS:
            saveArchive(true);
            break;
        case IDM_FILE_EXIT:
            DestroyWindow(window);
            break;
        case IDM_EXTRACT_SELECTED:
            if (const auto selected = selectedArchivePath()) {
                extractPath(*selected, true, false);
            }
            break;
        case IDM_EXTRACT_SHOWN:
            extractPath(gCurrentFolder, true, true);
            break;
        case IDM_IMPORT_FILE:
            importFile();
            break;
        case IDM_IMPORT_FOLDER:
            importFolder();
            break;
        case IDM_DELETE_ENTRY:
            deleteSelected();
            break;
        case IDM_TOOLS_SERVER_CONFIG:
            editServerConfiguration();
            break;
        case IDM_TOOLS_DEFRAGMENT:
            defragmentArchive();
            break;
        case IDM_HELP_MD5:
            showMd5Helper();
            break;
        case IDM_HELP_ABOUT:
            showAbout();
            break;
        case IDM_FOCUS_SEARCH:
            SetFocus(gSearch);
            SendMessageW(gSearch, EM_SETSEL, 0, -1);
            break;
        case IDM_VIEW_REFRESH:
            if (gLoaded) {
                populateTree();
                populateList(gCurrentFolder);
            }
            break;
        case IDM_CONTEXT_EDIT:
            if (const auto selected = selectedArchivePath()) {
                if (const auto info = gArchive.find(*selected)) {
                    editTextEntry(*info);
                }
            }
            break;
        case IDM_CONTEXT_EXTRACT:
            if (const auto selected = selectedArchivePath()) {
                extractPath(*selected, true, false);
            }
            break;
        case IDM_CONTEXT_REPLACE:
            if (const auto selected = selectedArchivePath()) {
                importFileOver(*selected);
            }
            break;
        case IDM_CONTEXT_DELETE:
            deleteSelected();
            break;
        case IDM_CONTEXT_COPY_PATH:
            if (const auto selected = selectedArchivePath()) {
                copyTextToClipboard(toWide(*selected));
                setStatus(L"Copied path to clipboard: " + toWide(*selected));
            }
            break;
        case IDM_CONTEXT_IMPORT_FILE:
            if (const auto selected = selectedArchivePath()) {
                importFileIntoFolder(*selected);
            } else {
                importFileIntoFolder(gCurrentFolder);
            }
            break;
        case IDM_CONTEXT_IMPORT_FOLDER:
            if (const auto selected = selectedArchivePath()) {
                importFolderIntoFolder(*selected);
            } else {
                importFolderIntoFolder(gCurrentFolder);
            }
            break;
        default:
            break;
        }
        return 0;
    case WM_SETCURSOR:
        if (reinterpret_cast<HWND>(wParam) == window && LOWORD(lParam) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(window, &pt);
            if (pt.x >= gTreeWidth && pt.x <= gTreeWidth + 6) {
                SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                return TRUE;
            }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lParam);
        if (x >= gTreeWidth - 2 && x <= gTreeWidth + 6) {
            gDraggingSplitter = true;
            SetCapture(window);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        if (gDraggingSplitter) {
            RECT rc{};
            GetClientRect(window, &rc);
            const int width = rc.right - rc.left;
            const int x = GET_X_LPARAM(lParam);
            gTreeWidth = std::clamp(x, 120, std::max(120, width - 200));
            layoutControls(window);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP: {
        if (gDraggingSplitter) {
            gDraggingSplitter = false;
            ReleaseCapture();
            return 0;
        }
        break;
    }
    case WM_CONTEXTMENU: {
        HWND target = reinterpret_cast<HWND>(wParam);
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        if (target == gTree) {
            showTreeContextMenu(gTree, x, y);
            return 0;
        } else if (target == gList) {
            showListContextMenu(gList, x, y);
            return 0;
        }
        return 0;
    }
    case WM_DROPFILES:
        handleDropFiles(reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_CLOSE:
        if (gBusy) {
            setStatus(L"Please wait for the current operation to finish.");
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_APP_OPEN_COMPLETE: {
        std::unique_ptr<OpenResult> result(reinterpret_cast<OpenResult*>(lParam));
        setBusy(false, L"");
        if (result->ok) {
            gArchive = std::move(result->archive);
            gLoaded = true;
            populateTree();
            populateList("");
            setStatus(archiveSummary());
            updateTitle();
        } else {
            gLoaded = false;
            resetViews();
            setStatus(L"PK2 load failed.");
            MessageBoxW(gMain, result->message.c_str(), L"PK2 Tool Error", MB_ICONERROR | MB_OK);
            updateTitle();
        }
        return 0;
    }
    case WM_APP_TASK_PROGRESS: {
        std::unique_ptr<TaskProgress> progress(reinterpret_cast<TaskProgress*>(lParam));
        setProgress(progress->percent);
        setStatus(progress->message);
        return 0;
    }
    case WM_APP_TASK_COMPLETE: {
        std::unique_ptr<TaskResult> result(reinterpret_cast<TaskResult*>(lParam));
        setBusy(false, L"");
        if (result->ok) {
            if (result->archiveChanged) {
                const auto refreshFolder = validRefreshFolder(result->refreshFolder);
                populateTree();
                populateList(refreshFolder);
                updateTitle();
            }
            setStatus(result->message);
        } else {
            if (result->archiveChanged) {
                const auto refreshFolder = validRefreshFolder(result->refreshFolder);
                populateTree();
                populateList(refreshFolder);
                updateTitle();
            }
            setStatus(L"Operation failed.");
            MessageBoxW(gMain, result->message.c_str(), L"PK2 Tool Error", MB_ICONERROR | MB_OK);
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    gInstance = instance;
    gUiThreadId = GetCurrentThreadId();

    const auto oleResult = OleInitialize(nullptr);
    if (FAILED(oleResult)) {
        return 1;
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&controls);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpszClassName = L"PK2WorkbenchPROWindow";
    wc.lpfnWndProc = windowProc;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MAIN_MENU);

    if (!RegisterClassExW(&wc)) {
        OleUninitialize();
        return 1;
    }

    gMain = CreateWindowExW(0,
                            wc.lpszClassName,
                            kAppTitle,
                            WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT,
                            CW_USEDEFAULT,
                            1000,
                            650,
                            nullptr,
                            nullptr,
                            instance,
                            nullptr);
    if (gMain == nullptr) {
        OleUninitialize();
        return 1;
    }

    ShowWindow(gMain, showCommand);
    UpdateWindow(gMain);

    HACCEL accelerators = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(IDR_ACCELERATOR));

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (accelerators == nullptr || !TranslateAcceleratorW(gMain, accelerators, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    const auto exitCode = static_cast<int>(message.wParam);
    OleUninitialize();
    return exitCode;
}
