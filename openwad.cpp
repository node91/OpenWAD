/*
===========================================
2026 - OPENWAD v1.0 - node91 - Grand Prix 4
===========================================
TO DO:
- a refactored version with modules
- an optimized version using parallel I/O

example:
src/
│
├── main.cpp
├── openwad.h
├── openwad.cpp
│
├── wad_extract.h
├── wad_extract.cpp
│
├── wad_pack.h
├── wad_pack.cpp
│
├── mapped_file.h
├── mapped_file.cpp
│
├── logger.h
├── logger.cpp
│
└── ui_state.h

Feel free to use and modify this code as
you see fit.
===========================================
*/
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <unordered_set>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

#pragma pack(push, 1)
struct WadHeader {
    uint32_t fileCount;   // Number of entries in the WAD table
};

struct WadItem {
    char     name[128];   // ANSI file name (relative path inside WAD)
    uint32_t dataOffset;  // Offset of file data from start of WAD
    uint32_t dataSize;    // Size of file data in bytes
};
#pragma pack(pop)

static HWND g_hProgress = nullptr;              // Handle to the progress bar control
static HWND g_hLog = nullptr;                   // Handle to the log EDIT control
static HWND g_hMainWnd = nullptr;               // Handle to the main application window
static HHOOK g_hMsgBoxHook = nullptr;           // Hook handle for centering MessageBox
static HWND g_hChkDisableOverwrite = nullptr;   // Handle to "Disable overwrite warning" checkbox
static bool g_DisableOverwriteWarning = false;  // Global flag to skip overwrite confirmations
static std::wstring g_LogBuffer;                // Buffered log text before flushing to UI
static HWND g_hChkOnTop = nullptr;              // Handle to "Keep on top" checkbox
static bool g_KeepOnTop = false;                // Global flag for topmost window state

// ------------------------------------------------------------
// Append a line to the in-memory log buffer
// (actual UI update is deferred to AppendBufferedLog)
// ------------------------------------------------------------
static void LogBuffered(const std::wstring& text)
{
    g_LogBuffer += text;
    g_LogBuffer += L"\r\n";
}

// ------------------------------------------------------------
// Flush buffered log content to the EDIT control in one batch
// and scroll the caret to the bottom
// ------------------------------------------------------------
static void AppendBufferedLog()
{
    if (!g_hLog || g_LogBuffer.empty())
        return;

    // --------------------------------------------------------
    // Move caret to the end of the current log text
    // --------------------------------------------------------
    int len = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, len, len);

    // --------------------------------------------------------
    // Append buffered text to the log control
    // --------------------------------------------------------
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)g_LogBuffer.c_str());

    // --------------------------------------------------------
    // Scroll the caret into view (auto-scroll to bottom)
    // --------------------------------------------------------
    SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);

    g_LogBuffer.clear();
}

// ------------------------------------------------------------
// Directly append a single line of text to the log EDIT control
// ------------------------------------------------------------
static void Log(const std::wstring& text) {
    if (!g_hLog) return;
    int len = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, len, len);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)(text + L"\r\n").c_str());
}

// ------------------------------------------------------------
// Clear the log window and repaint it to show an empty state
// ------------------------------------------------------------
static void ClearLog() {
    if (g_hLog) {
        SetWindowTextW(g_hLog, L"");
        UpdateWindow(g_hLog);
    }
}

// ------------------------------------------------------------
// Format a duration in seconds with three decimal places
// (e.g. "0.123 seconds")
// ------------------------------------------------------------
static std::wstring FormatSeconds(double s)
{
    wchar_t buf[64];
    swprintf(buf, 64, L"%.3f seconds", s);
    return buf;
}

// ------------------------------------------------------------
// CBT hook procedure to center a MessageBox relative to the
// main application window when it is activated
// ------------------------------------------------------------
static LRESULT CALLBACK MsgBoxHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HCBT_ACTIVATE) {
        HWND hMsgBox = (HWND)wParam;

        RECT rcOwner, rcDlg;
        GetWindowRect(g_hMainWnd, &rcOwner);
        GetWindowRect(hMsgBox, &rcDlg);

        int x = rcOwner.left + ((rcOwner.right - rcOwner.left) - (rcDlg.right - rcDlg.left)) / 2;
        int y = rcOwner.top + ((rcOwner.bottom - rcOwner.top) - (rcDlg.bottom - rcDlg.top)) / 2;

        SetWindowPos(hMsgBox, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

        UnhookWindowsHookEx(g_hMsgBoxHook);
    }
    return CallNextHookEx(g_hMsgBoxHook, code, wParam, lParam);
}

// ------------------------------------------------------------
// Ask the user to confirm overwriting an existing directory or
// WAD file, unless overwrite warnings are disabled globally
// ------------------------------------------------------------
static bool ConfirmOverwrite(const std::wstring& target)
{
    if (g_DisableOverwriteWarning)
        return true; // Skip dialog entirely when disabled

    std::wstring msg = L"Files will be overwritten!\n" + target + L"\n\nContinue?";

    // --------------------------------------------------------
    // Install a CBT hook so the MessageBox is centered on the
    // main window when it appears
    // --------------------------------------------------------
    g_hMsgBoxHook = SetWindowsHookExW(WH_CBT, MsgBoxHookProc, nullptr, GetCurrentThreadId());

    int r = MessageBoxW(
        g_hMainWnd,
        msg.c_str(),
        L"Overwrite warning",
        MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2
    );

    return r == IDYES;
}

// ------------------------------------------------------------
// Set the progress bar position in the range 0–100
// ------------------------------------------------------------
static void SetProgress(int percent) {
    if (g_hProgress) SendMessageW(g_hProgress, PBM_SETPOS, percent, 0);
}

// ------------------------------------------------------------
// Display an error message box and log the error text
// ------------------------------------------------------------
static void ShowError(const wchar_t* msg) {
    MessageBoxW(nullptr, msg, L"WAD Tool Error", MB_ICONERROR | MB_OK);
    Log(L"ERROR: " + std::wstring(msg));
}

// ------------------------------------------------------------
// Return true if the given path points to an existing directory
// ------------------------------------------------------------
static bool IsDirectory(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// ------------------------------------------------------------
// Convert an ANSI (CP_ACP) string view to a UTF-16 std::wstring
// ------------------------------------------------------------
static std::wstring ToWideFromAnsi(std::string_view s)
{
    if (s.empty()) return L"";

    int needed = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(needed, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), out.data(), needed);
    return out;
}

// ------------------------------------------------------------
// Convert a UTF-16 std::wstring to an ANSI (CP_ACP) std::string
// ------------------------------------------------------------
static std::string ToAnsiFromWide(const std::wstring& s) {
    if (s.empty()) return "";
    int len = WideCharToMultiByte(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_ACP, 0, s.c_str(), (int)s.size(), out.data(), len, nullptr, nullptr);
    return out;
}

// ------------------------------------------------------------
// RAII wrapper for a read-only memory-mapped input file
// ------------------------------------------------------------
struct MappedFile {
    HANDLE hFile = nullptr;         // Underlying file handle
    HANDLE hMap = nullptr;          // File mapping handle
    const uint8_t* base = nullptr;  // Base address of mapped view
    size_t size = 0;                // Total size of the mapped file

    // --------------------------------------------------------
    // Open the file read-only and map its entire contents into
    // memory. Returns true on success.
    // --------------------------------------------------------
    bool open(const std::wstring& path) {
        hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;

        LARGE_INTEGER li{};
        if (!GetFileSizeEx(hFile, &li)) return false;
        size = static_cast<size_t>(li.QuadPart);

        hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!hMap) return false;

        base = static_cast<const uint8_t*>(
            MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0)
            );
        return base != nullptr;
    }

    // --------------------------------------------------------
    // Unmap the view and close any open handles associated with
    // this mapped file
    // --------------------------------------------------------
    void close() {
        if (base) UnmapViewOfFile(base);
        if (hMap) CloseHandle(hMap);
        if (hFile && hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
        base = nullptr;
        hMap = nullptr;
        hFile = nullptr;
        size = 0;
    }

    ~MappedFile() { close(); }
};

// ------------------------------------------------------------
// RAII wrapper for a read/write memory-mapped output file
// ------------------------------------------------------------
struct MappedOutput {
    HANDLE hFile = nullptr;   // Underlying file handle
    HANDLE hMap = nullptr;    // File mapping handle
    uint8_t* base = nullptr;  // Base address of mapped writable view
    size_t size = 0;          // Total size of the mapped file

    // --------------------------------------------------------
    // Create a new file of the specified size and map it with
    // read/write access. Returns true on success.
    // --------------------------------------------------------
    bool create(const std::wstring& path, size_t totalSize) {
        size = totalSize;

        hFile = CreateFileW(
            path.c_str(),
            GENERIC_WRITE | GENERIC_READ,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (hFile == INVALID_HANDLE_VALUE)
            return false;

        // ----------------------------------------------------
        // Pre-allocate the file to the requested total size by
        // moving the file pointer and setting the end of file
        // ----------------------------------------------------
        LARGE_INTEGER li;
        li.QuadPart = totalSize;
        if (!SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN))
        {
            CloseHandle(hFile);
            hFile = nullptr;
            return false;
        }

        if (!SetEndOfFile(hFile))
        {
            CloseHandle(hFile);
            hFile = nullptr;
            return false;
        }

        // ----------------------------------------------------
        // Create a read/write file mapping object for the file
        // ----------------------------------------------------
        hMap = CreateFileMappingW(hFile, nullptr, PAGE_READWRITE, 0, 0, nullptr);
        if (!hMap) {
            CloseHandle(hFile);
            hFile = nullptr;
            return false;
        }

        base = static_cast<uint8_t*>(
            MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, 0)
            );
        if (!base) {
            CloseHandle(hMap);
            CloseHandle(hFile);
            hMap = nullptr;
            hFile = nullptr;
            return false;
        }
        return true;
    }

    // --------------------------------------------------------
    // Unmap the view and close any open handles associated with
    // this mapped output file
    // --------------------------------------------------------
    void close() {
        if (base) UnmapViewOfFile(base);
        if (hMap) CloseHandle(hMap);
        if (hFile && hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
        base = nullptr;
        hMap = nullptr;
        hFile = nullptr;
        size = 0;
    }

    ~MappedOutput() { close(); }
};

static void ExtractWad(const std::wstring& wadPath)
{
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    Log(L"Reading WAD header");
    SetProgress(0);

    // ------------------------------------------------------------
    // 1. Open and memory-map the WAD file for read-only access
    // ------------------------------------------------------------
    MappedFile mf;
    if (!mf.open(wadPath)) {
        ShowError(L"Failed to memory-map WAD file.");
        return;
    }

    // ------------------------------------------------------------
    // 2. Basic header size check to ensure a valid WAD header is present
    // ------------------------------------------------------------
    if (mf.size < sizeof(WadHeader)) {
        ShowError(L"Invalid WAD file.");
        return;
    }

    const uint8_t* ptr = mf.base;

    const WadHeader* header = reinterpret_cast<const WadHeader*>(ptr);
    const WadItem* table = reinterpret_cast<const WadItem*>(ptr + sizeof(WadHeader));

    // ------------------------------------------------------------
    // 3. Validate that the header + table region fits inside the file
    // ------------------------------------------------------------
    size_t tableBytes = sizeof(WadHeader) + header->fileCount * sizeof(WadItem);
    if (tableBytes > mf.size) {
        ShowError(L"Invalid WAD: header/table exceeds file size.");
        return;
    }

    // ------------------------------------------------------------
    // 4. Validate each entry's data range before extraction
    //    - data must lie within the file
    //    - data must be located after the header + table region
    // ------------------------------------------------------------
    for (size_t i = 0; i < header->fileCount; ++i) {
        const WadItem& wi = table[i];
        uint64_t start = wi.dataOffset;
        uint64_t end = wi.dataOffset + wi.dataSize;
        if (end > mf.size || start < tableBytes) {
            ShowError(L"Invalid WAD: corrupt offsets or sizes.");
            return;
        }
    }

    Log(std::to_wstring(header->fileCount) + L" files found");

    // ------------------------------------------------------------
    // 5. Determine and prepare the output directory:
    //    <wad directory>\<wad file name without extension>
    // ------------------------------------------------------------
    std::filesystem::path wadP(wadPath);
    std::filesystem::path outDir = wadP.parent_path() / wadP.stem();

    if (std::filesystem::exists(outDir)) {
        if (!ConfirmOverwrite(outDir.wstring())) {
            Log(L"Extraction cancelled");
            return;
        }
    }
    else {
        std::error_code ec;
        std::filesystem::create_directory(outDir, ec);
        if (ec) {
            ShowError(L"Failed to create output directory.");
            return;
        }
    }

    Log(L"Extracting...");

    // ------------------------------------------------------------
    // 6. Track directories already created to avoid redundant
    //    create_directories calls during extraction
    // ------------------------------------------------------------
    std::unordered_set<std::wstring> createdDirs;
    createdDirs.reserve(header->fileCount);

    // ------------------------------------------------------------
    // 7. Extract each WAD entry to its corresponding file on disk
    // ------------------------------------------------------------
    for (size_t i = 0; i < header->fileCount; ++i) {
        const WadItem& wi = table[i];

        size_t len = strnlen(wi.name, sizeof(wi.name));
        std::string_view nameAnsi(wi.name, len);
        std::wstring nameW = ToWideFromAnsi(nameAnsi);

        std::filesystem::path outPath = outDir / nameW;
        std::wstring parent = outPath.parent_path().wstring();

        // --------------------------------------------------------
        // Create the parent directory tree once per unique path
        // --------------------------------------------------------
        if (createdDirs.insert(parent).second) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                LogBuffered(L"Failed to create directory: " + parent);
                continue; // Skip file instead of crashing
            }
        }

        // --------------------------------------------------------
        // Copy the file data from the mapped WAD into a new file
        // --------------------------------------------------------
        const uint8_t* src = ptr + wi.dataOffset;

        std::ofstream out(outPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(src), wi.dataSize);

        LogBuffered(L"Extracting: " + nameW);

        SetProgress((int)(((i + 1) * 100) / header->fileCount));
    }
    AppendBufferedLog();

    SetProgress(100);
    Log(L"Extraction complete");

    // ------------------------------------------------------------
    // 8. Measure and log total extraction time
    // ------------------------------------------------------------
    QueryPerformanceCounter(&t1);
    double elapsed = double(t1.QuadPart - t0.QuadPart) / double(freq.QuadPart);

    Log(L"Time taken: " + FormatSeconds(elapsed));

    Log(L"Drop the next WAD or folder");
    SetProgress(0);
}

struct SourceItem {
    std::filesystem::path fullPath;  // Full path to the source file on disk
    std::wstring relPathW;           // Relative path (UTF-16) inside the base folder
    std::string wadName;             // Relative path encoded as ANSI for WAD storage
    std::vector<char> data;          // File contents loaded into memory
};

static void PackFolder(const std::wstring& folderPath)
{
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    Log(L"Reading folder contents...");
    SetProgress(0);

    std::filesystem::path base(folderPath);
    if (!std::filesystem::is_directory(base)) {
        ShowError(L"Path is not a directory.");
        return;
    }

    // ------------------------------------------------------------
    // 1. Count files first (for progress bar)
    // ------------------------------------------------------------
    size_t totalFiles = 0;
    for (auto const& entry : std::filesystem::recursive_directory_iterator(base))
        if (entry.is_regular_file())
            totalFiles++;

    if (totalFiles == 0) {
        Log(L"Folder contains no files.");
        return;
    }
    else
        Log(std::to_wstring(totalFiles) + L" files found");

    // ------------------------------------------------------------
    // 2. Collect files with full 0–100% progress
    //    - build SourceItem list
    //    - load file contents into memory
    // ------------------------------------------------------------
    std::vector<SourceItem> items;
    items.reserve(totalFiles);

    size_t scanned = 0;

    for (auto const& entry : std::filesystem::recursive_directory_iterator(base)) {
        if (!entry.is_regular_file()) continue;

        scanned++;
        SetProgress((int)((scanned * 100) / totalFiles));   // FULL 0–100%

        SourceItem si;
        si.fullPath = entry.path();

        std::filesystem::path rel = std::filesystem::relative(entry.path(), base);
        std::wstring relW;
        try {
            relW = rel.native();
        }
        catch (...) {
            Log(L"Skipping unreadable path: " + entry.path().wstring());
            continue;
        }

        // --------------------------------------------------------
        // Normalize separators to backslashes for WAD internal names
        // --------------------------------------------------------
        std::replace(relW.begin(), relW.end(), L'/', L'\\');
        si.wadName = ToAnsiFromWide(relW);

        si.relPathW = relW;

        // --------------------------------------------------------
        // Read the entire file content into the data buffer
        // --------------------------------------------------------
        std::ifstream in(entry.path(), std::ios::binary | std::ios::ate);
        std::streamsize size = in.tellg();
        in.seekg(0, std::ios::beg);

        si.data.resize((size_t)size);
        if (size > 0)
            in.read(si.data.data(), size);

        items.push_back(std::move(si));
    }

    SetProgress(100);
    Log(L"Collecting files completed");

    // ------------------------------------------------------------
    // 3. Determine output path
    //    - use the base folder name with a .wad extension
    // ------------------------------------------------------------
    std::filesystem::path outPath = base;
    outPath.replace_extension(L".wad");

    if (std::filesystem::exists(outPath)) {
        if (!ConfirmOverwrite(outPath.wstring())) {
            Log(L"Cancelled creating WAD");
            return;
        }
    }

    // ------------------------------------------------------------
    // 4. Compute total WAD size
    //    - header
    //    - item table
    //    - all file data
    // ------------------------------------------------------------
    size_t totalSize =
        sizeof(WadHeader) +
        items.size() * sizeof(WadItem);

    for (auto& si : items)
        totalSize += si.data.size();

    // ------------------------------------------------------------
    // 5. Create memory-mapped output file
    // ------------------------------------------------------------
    MappedOutput mout;
    if (!mout.create(outPath.wstring(), totalSize)) {
        ShowError(L"Failed to create memory-mapped WAD file.");
        return;
    }

    uint8_t* ptr = mout.base;

    // ------------------------------------------------------------
    // 6. Write header + table
    //    - fill WadHeader
    //    - build WadItem table with names, offsets, sizes
    // ------------------------------------------------------------
    WadHeader* header = reinterpret_cast<WadHeader*>(ptr);
    header->fileCount = (uint32_t)items.size();

    WadItem* table = reinterpret_cast<WadItem*>(ptr + sizeof(WadHeader));

    uint32_t offset = sizeof(WadHeader) + (uint32_t)(items.size() * sizeof(WadItem));

    for (size_t i = 0; i < items.size(); ++i) {
        WadItem& wi = table[i];
        memset(&wi, 0, sizeof(WadItem));

        size_t len = items[i].wadName.size();
        if (len >= sizeof(wi.name)) len = sizeof(wi.name) - 1;
        memcpy(wi.name, items[i].wadName.data(), len);

        wi.dataOffset = offset;
        wi.dataSize = (uint32_t)items[i].data.size();

        offset += wi.dataSize;
    }

    // ------------------------------------------------------------
    // 7. Reset progress bar for writing phase
    // ------------------------------------------------------------
    SetProgress(0);
    Log(L"Packing...");

    // ------------------------------------------------------------
    // 8. Write file data with full 0–100% progress
    // ------------------------------------------------------------
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& si = items[i];

        LogBuffered(L"Packing: " + si.relPathW);
        SetProgress((int)(((i + 1) * 100) / items.size()));   // FULL 0–100%

        uint8_t* dst = ptr + table[i].dataOffset;

        if (!si.data.empty())
            memcpy(dst, si.data.data(), si.data.size());
    }

    // ------------------------------------------------------------
    // 9. Done
    //    - unmap file
    //    - flush buffered log
    //    - log elapsed time
    // ------------------------------------------------------------
    mout.close();

    AppendBufferedLog();

    SetProgress(100);
    Log(L"Packing complete.");

    QueryPerformanceCounter(&t1);
    double elapsed = double(t1.QuadPart - t0.QuadPart) / double(freq.QuadPart);

    Log(L"Time taken: " + FormatSeconds(elapsed));

    Log(L"Drop the next WAD or folder");
    SetProgress(0);
}

static void HandleDrop(HDROP hDrop) {
    // ------------------------------------------------------------
    // 1. Determine how many files/folders were dropped
    // ------------------------------------------------------------
    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    if (count == 0) {
        DragFinish(hDrop);
        return;
    }

    // ------------------------------------------------------------
    // 2. Clear previous log output before handling new drop
    // ------------------------------------------------------------
    ClearLog();

    // ------------------------------------------------------------
    // 3. Process each dropped item:
    //    - if directory: pack into WAD
    //    - if .wad file: extract
    //    - otherwise: log unsupported item
    // ------------------------------------------------------------
    for (UINT i = 0; i < count; ++i) {
        wchar_t path[MAX_PATH];
        DragQueryFileW(hDrop, i, path, MAX_PATH);

        std::wstring p(path);
        Log(L"Loading: " + p);
        SetProgress(0);

        if (IsDirectory(p)) {
            PackFolder(p);
        }
        else {
            std::filesystem::path ext = std::filesystem::path(p).extension();
            if (_wcsicmp(ext.c_str(), L".wad") == 0) {
                ExtractWad(p);
            }
            else {
                Log(L"Not a WAD file: " + p);
            }
        }
    }

    // ------------------------------------------------------------
    // 4. Release HDROP handle provided by the shell
    // ------------------------------------------------------------
    DragFinish(hDrop);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HFONT hFont = nullptr;
    switch (msg) {
    case WM_CREATE:
        // --------------------------------------------------------
        // 1. Store main window handle and enable file drag & drop
        // --------------------------------------------------------
        g_hMainWnd = hwnd;
        DragAcceptFiles(hwnd, TRUE);

        // --------------------------------------------------------
        // 2. Create log EDIT control with initial instructions
        // --------------------------------------------------------
        g_hLog = CreateWindowW(
            L"EDIT", L"OpenWAD - Created by node91 - Grand Prix 4\r\n"
            "==========================================\r\n"
            "\r\n"
            "To extract: drop WAD files here\r\n"
            "To pack: drop Windows folders here\r\n",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            10, 40, 460, 220,
            hwnd, nullptr, nullptr, nullptr);

        // --------------------------------------------------------
        // 3. Create 'Disable overwrite warning' checkbox
        // --------------------------------------------------------
        g_hChkDisableOverwrite = CreateWindowW(
            L"BUTTON",
            L"Disable overwrite warning",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            10, 270, 200, 20,
            hwnd,
            (HMENU)1001,
            nullptr,
            nullptr
        );

        // --------------------------------------------------------
        // 4. Create 'Keep on top' checkbox next to the first one
        // --------------------------------------------------------
        g_hChkOnTop = CreateWindowW(
            L"BUTTON",
            L"Keep on top",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            220, 270, 100, 20,      // positioned next to the first checkbox
            hwnd,
            (HMENU)1002,
            nullptr,
            nullptr
        );

        {
            // ----------------------------------------------------
            // 5. Create a Consolas fixed-width font and apply it
            //    to the log and both checkboxes
            // ----------------------------------------------------
            HFONT hFontLocal = CreateFontW(
                -12, 0, 0, 0,
                FW_NORMAL,
                FALSE, FALSE, FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY,
                FIXED_PITCH | FF_MODERN,
                L"Consolas"
            );
            SendMessageW(g_hLog, WM_SETFONT, (WPARAM)hFontLocal, TRUE);
            SendMessageW(g_hChkDisableOverwrite, WM_SETFONT, (WPARAM)hFontLocal, TRUE);
            SendMessageW(g_hChkOnTop, WM_SETFONT, (WPARAM)hFontLocal, TRUE);
        }

        // --------------------------------------------------------
        // 6. Limit log content to 1 MB to avoid unbounded growth
        // --------------------------------------------------------
        SendMessageW(g_hLog, EM_LIMITTEXT, 1 * 1024 * 1024, 0);

        // --------------------------------------------------------
        // 7. Create the progress bar at the top of the window
        // --------------------------------------------------------
        g_hProgress = CreateWindowW(
            PROGRESS_CLASSW, nullptr,
            WS_CHILD | WS_VISIBLE,
            10, 10, 460, 20,
            hwnd, nullptr, nullptr, nullptr);

        SendMessageW(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);

        return 0;

    case WM_DROPFILES:
        // --------------------------------------------------------
        // Handle shell drop (one or more files/folders)
        // --------------------------------------------------------
        HandleDrop((HDROP)wParam);
        return 0;

    case WM_COMMAND:
        // --------------------------------------------------------
        // 1. Toggle 'Disable overwrite warning' option
        // --------------------------------------------------------
        if ((HWND)lParam == g_hChkDisableOverwrite &&
            HIWORD(wParam) == BN_CLICKED)
        {
            g_DisableOverwriteWarning =
                (SendMessageW(g_hChkDisableOverwrite, BM_GETCHECK, 0, 0) == BST_CHECKED);
        }
        // --------------------------------------------------------
        // 2. Toggle 'Keep on top' and update window Z-order
        // --------------------------------------------------------
        else if ((HWND)lParam == g_hChkOnTop &&
            HIWORD(wParam) == BN_CLICKED)
        {
            g_KeepOnTop =
                (SendMessageW(g_hChkOnTop, BM_GETCHECK, 0, 0) == BST_CHECKED);

            SetWindowPos(
                g_hMainWnd,
                g_KeepOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE
            );
        }
        break;

    case WM_DESTROY:
        // --------------------------------------------------------
        // Cleanup and exit message loop
        // --------------------------------------------------------
        PostQuitMessage(0);
        hFont = (HFONT)SendMessageW(g_hLog, WM_GETFONT, 0, 0);
        DeleteObject(hFont);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPWSTR lpCmdLine,
    int nCmdShow
)
{
    // ------------------------------------------------------------
    // 1. Initialize common controls (progress bar class)
    // ------------------------------------------------------------
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    // ------------------------------------------------------------
    // 2. Register the main window class
    // ------------------------------------------------------------
    const wchar_t CLASS_NAME[] = L"WADDragDropWnd";

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClassW(&wc);

    // ------------------------------------------------------------
    // 3. Create the main window with fixed size and no maximize box
    // ------------------------------------------------------------
    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"OpenWAD",
        WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME),
        CW_USEDEFAULT, CW_USEDEFAULT,
        500, 340,
        nullptr, nullptr, hInstance, nullptr);

    ShowWindow(hwnd, nCmdShow);

    // ------------------------------------------------------------
    // 4. Standard message loop
    // ------------------------------------------------------------
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}