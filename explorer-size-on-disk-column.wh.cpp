// ==WindhawkMod==
// @id              explorer-size-on-disk-column
// @name            Size on disk column in Explorer details
// @description     Adds a "Size on disk" column to File Explorer's details view for files and folders
// @version         0.1.0
// @author          stoilms
// @github          https://github.com/stoilms
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lpropsys -lshlwapi -luuid
// ==/WindhawkMod==

// Source code is published under The GNU General Public License v3.0.
//
// Structure and the windows.storage.dll symbol hooks are based on m417z's
// "Better file sizes in Explorer details" mod:
// https://github.com/ramensoftware/windhawk-mods/blob/main/mods/explorer-details-better-file-sizes.wh.cpp

// ==WindhawkModReadme==
/*
# Size on disk column in Explorer details

Adds a **Size on disk** column to File Explorer's details view. The value
matches the "Size on disk" figure in a file or folder's Properties dialog as
closely as possible.

## How to use

1. Enable the mod and restart Explorer (or sign out and back in).
2. Open a folder in **Details** view.
3. Right-click any column header and tick **Size on disk**. If it isn't in the
   short list, click **More...** and find it there.

## How the value is calculated

* **Files:** the file's allocation size, as reported by the file system. For
  NTFS-compressed, sparse and CompactOS/WOF-compressed files, the compressed
  size rounded up to the volume's cluster size is used instead. Cloud
  placeholders (OneDrive, etc.) report only what is stored locally and are
  never downloaded.
* **Folders:** the sum of the size on disk of every file underneath,
  calculated manually by walking the folder tree. Junctions and symbolic links
  inside the folder are not followed.

## Notes

* Folder calculation can be slow for large trees. Results are cached for a
  short, configurable time so sorting doesn't recalculate everything.
* Network folders are skipped by default.
* Only regular file-system folders are supported. Libraries, search results,
  zip folders and the Recycle Bin are not.
* Hard links are counted once per link, as the Properties dialog does.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- folderSizes: always
  $name: Show folder sizes
  $description: >-
    Folder sizes are calculated by walking the whole folder tree, which can be
    slow for large folders. With the Shift option, folder sizes are only
    calculated if Shift is held when the list is loaded or refreshed.
  $options:
  - always: Enabled, calculated manually (can be slow)
  - withShiftKey: Enabled, calculated manually while holding the Shift key
  - disabled: Disabled (files only)
- folderAccuracy: fast
  $name: Folder calculation method
  $description: >-
    Fast reads sizes from directory listings. Accurate opens every file, which
    avoids rare stale values from NTFS directory entries but is much slower.
  $options:
  - fast: Fast (directory listings)
  - accurate: Accurate (open each file)
- networkFolders: false
  $name: Calculate folder sizes on network drives
- mixFoldersWhenSorting: false
  $name: Mix files and folders when sorting by size on disk
- cacheSeconds: 10
  $name: Cache duration (seconds)
  $description: >-
    How long a calculated value is reused. Refreshing within this window shows
    cached values.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <propsys.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <shtypes.h>

#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif

using namespace std::string_view_literals;

////////////////////////////////////////////////////////////////////////////////
// Settings and shared state

enum class FolderSizes { always, withShiftKey, disabled };

struct {
    FolderSizes folderSizes;
    bool accurateFolders;
    bool networkFolders;
    bool mixFoldersWhenSorting;
    ULONGLONG cacheMs;
} g_settings;

constexpr GUID kFmtStorage = {0xB725F130,
                              0x47EF,
                              0x101A,
                              {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}};
constexpr PROPERTYKEY kPKEY_Size = {kFmtStorage, 12};
// System.FileAllocationSize: defined by Windows but hidden from the column
// picker (IsColumn = false, no label, no display format).
constexpr PROPERTYKEY kPKEY_SizeOnDisk = {kFmtStorage, 18};

constexpr WCHAR kColumnTitle[] = L"Size on disk";
constexpr UINT kColumnWidthChars = 12;

std::atomic<int> g_hookRefCount;

auto HookRefCountScope() {
    g_hookRefCount++;
    return std::unique_ptr<decltype(g_hookRefCount),
                           void (*)(decltype(g_hookRefCount)*)>{
        &g_hookRefCount, [](auto refCount) { (*refCount)--; }};
}

// Minimal COM smart pointer, to avoid a C++/WinRT dependency.
template <typename T>
class ComPtr {
   public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { Reset(); }
    void Reset() {
        if (m_ptr) {
            m_ptr->Release();
            m_ptr = nullptr;
        }
    }
    T* Get() const { return m_ptr; }
    T** Put() {
        Reset();
        return &m_ptr;
    }
    void** PutVoid() { return reinterpret_cast<void**>(Put()); }
    T* operator->() const { return m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

   private:
    T* m_ptr = nullptr;
};

////////////////////////////////////////////////////////////////////////////////
// Path helpers

bool IsFileSystemPath(std::wstring_view path) {
    bool driveLetter = path.size() >= 3 && path[1] == L':' && path[2] == L'\\';
    bool unc = path.size() >= 3 && path.starts_with(L"\\\\"sv) &&
               !path.starts_with(L"\\\\?\\"sv);
    return driveLetter || unc;
}

std::wstring ToExtendedPath(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\"sv)) {
        return path;
    }
    if (path.starts_with(L"\\\\"sv)) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    return L"\\\\?\\" + path;
}

std::wstring JoinPath(const std::wstring& dir, std::wstring_view name) {
    std::wstring result = dir;
    if (!result.empty() && result.back() != L'\\') {
        result += L'\\';
    }
    result += name;
    return result;
}

bool IsNetworkPath(const std::wstring& path) {
    if (path.starts_with(L"\\\\"sv)) {
        return true;
    }
    if (path.size() >= 2 && path[1] == L':') {
        WCHAR root[] = {path[0], L':', L'\\', L'\0'};
        return GetDriveTypeW(root) == DRIVE_REMOTE;
    }
    return false;
}

////////////////////////////////////////////////////////////////////////////////
// Size on disk calculation

ULONGLONG GetClusterSize(const std::wstring& path) {
    std::wstring root;
    if (path.size() >= 3 && path[1] == L':') {
        root = path.substr(0, 3);
    } else {
        WCHAR volume[MAX_PATH];
        if (!GetVolumePathNameW(path.c_str(), volume, ARRAYSIZE(volume))) {
            return 0;
        }
        root = volume;
    }

    static std::mutex mutex;
    static std::unordered_map<std::wstring, ULONGLONG> cache;

    std::lock_guard lock(mutex);
    if (auto it = cache.find(root); it != cache.end()) {
        return it->second;
    }

    DWORD sectorsPerCluster, bytesPerSector, freeClusters, totalClusters;
    ULONGLONG clusterSize = 0;
    if (GetDiskFreeSpaceW(root.c_str(), &sectorsPerCluster, &bytesPerSector,
                          &freeClusters, &totalClusters)) {
        clusterSize = (ULONGLONG)sectorsPerCluster * bytesPerSector;
    }
    cache[root] = clusterSize;
    return clusterSize;
}

ULONGLONG RoundUp(ULONGLONG value, ULONGLONG granularity) {
    if (!granularity) {
        return value;
    }
    return (value + granularity - 1) / granularity * granularity;
}

bool IsCloudPlaceholder(DWORD attributes) {
    return attributes &
           (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS |
            FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE);
}

// For compressed, sparse and WOF (CompactOS) files the allocation size of the
// main stream doesn't reflect what's really on disk, so ask for the compressed
// size instead.
ULONGLONG AdjustAllocationSize(const std::wstring& path,
                               DWORD attributes,
                               ULONGLONG allocationSize) {
    bool needsCompressedSize =
        (attributes &
         (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE)) ||
        ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
         !IsCloudPlaceholder(attributes));
    if (!needsCompressedSize) {
        return allocationSize;
    }

    DWORD high = 0;
    DWORD low = GetCompressedFileSizeW(ToExtendedPath(path).c_str(), &high);
    if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
        return allocationSize;
    }

    ULONGLONG compressed = ((ULONGLONG)high << 32) | low;
    return RoundUp(compressed, GetClusterSize(path));
}

std::optional<ULONGLONG> GetFileSizeOnDisk(const std::wstring& path) {
    // FILE_FLAG_OPEN_REPARSE_POINT avoids following symlinks and avoids
    // triggering cloud file downloads.
    HANDLE file = CreateFileW(
        ToExtendedPath(path).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    bool ok = GetFileInformationByHandleEx(file, FileBasicInfo, &basic,
                                           sizeof(basic)) &&
              GetFileInformationByHandleEx(file, FileStandardInfo, &standard,
                                           sizeof(standard));
    CloseHandle(file);
    if (!ok) {
        return std::nullopt;
    }

    return AdjustAllocationSize(path, basic.FileAttributes,
                                standard.AllocationSize.QuadPart);
}

std::optional<ULONGLONG> GetFolderSizeOnDisk(const std::wstring& root) {
    // ULONGLONG elements keep the buffer 8-byte aligned, as required.
    std::vector<ULONGLONG> buffer(64 * 1024 / sizeof(ULONGLONG));
    const DWORD bufferBytes = (DWORD)(buffer.size() * sizeof(ULONGLONG));

    std::vector<std::wstring> pending{root};
    ULONGLONG total = 0;

    while (!pending.empty()) {
        std::wstring dir = std::move(pending.back());
        pending.pop_back();

        HANDLE handle = CreateFileW(
            ToExtendedPath(dir).c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            if (dir == root) {
                return std::nullopt;
            }
            continue;  // Access denied etc. - skip, as Properties does.
        }

        FILE_INFO_BY_HANDLE_CLASS infoClass = FileFullDirectoryRestartInfo;
        while (GetFileInformationByHandleEx(handle, infoClass, buffer.data(),
                                            bufferBytes)) {
            infoClass = FileFullDirectoryInfo;

            auto* entry = reinterpret_cast<FILE_FULL_DIR_INFO*>(buffer.data());
            while (true) {
                std::wstring_view name(entry->FileName,
                                       entry->FileNameLength / sizeof(WCHAR));
                DWORD attributes = entry->FileAttributes;

                if (name != L"."sv && name != L".."sv) {
                    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
                        // Don't follow junctions or directory symlinks.
                        if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                            pending.push_back(JoinPath(dir, name));
                        }
                    } else if (g_settings.accurateFolders) {
                        total += GetFileSizeOnDisk(JoinPath(dir, name))
                                     .value_or(0);
                    } else {
                        total += AdjustAllocationSize(
                            JoinPath(dir, name), attributes,
                            entry->AllocationSize.QuadPart);
                    }
                }

                if (!entry->NextEntryOffset) {
                    break;
                }
                entry = reinterpret_cast<FILE_FULL_DIR_INFO*>(
                    reinterpret_cast<BYTE*>(entry) + entry->NextEntryOffset);
            }
        }

        CloseHandle(handle);
    }

    return total;
}

////////////////////////////////////////////////////////////////////////////////
// Cache

struct ItemSize {
    std::optional<ULONGLONG> size;
    bool isFolder = false;
};

struct CacheEntry {
    ItemSize item;
    ULONGLONG tick;
};

std::mutex g_cacheMutex;
std::unordered_map<std::wstring, CacheEntry> g_cache;

bool ShouldCalculateFolder(const std::wstring& path) {
    switch (g_settings.folderSizes) {
        case FolderSizes::disabled:
            return false;
        case FolderSizes::withShiftKey:
            if (GetAsyncKeyState(VK_SHIFT) >= 0) {
                return false;
            }
            break;
        case FolderSizes::always:
            break;
    }

    return g_settings.networkFolders || !IsNetworkPath(path);
}

std::optional<ItemSize> GetItemSizeOnDisk(const std::wstring& path) {
    ULONGLONG now = GetTickCount64();

    {
        std::lock_guard lock(g_cacheMutex);
        if (auto it = g_cache.find(path); it != g_cache.end()) {
            if (now - it->second.tick < g_settings.cacheMs) {
                return it->second.item;
            }
            g_cache.erase(it);
        }
    }

    DWORD attributes = GetFileAttributesW(ToExtendedPath(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return std::nullopt;
    }

    ItemSize item;
    item.isFolder = attributes & FILE_ATTRIBUTE_DIRECTORY;

    if (!item.isFolder) {
        item.size = GetFileSizeOnDisk(path);
    } else if (ShouldCalculateFolder(path)) {
        Wh_Log(L"Calculating folder: %s", path.c_str());
        item.size = GetFolderSizeOnDisk(path);
    } else {
        return item;  // Not calculated, and not cached either.
    }

    if (item.size) {
        std::lock_guard lock(g_cacheMutex);
        if (g_cache.size() > 20000) {
            g_cache.clear();
        }
        g_cache[path] = {item, GetTickCount64()};
    }

    return item;
}

std::optional<std::wstring> GetItemPath(void* pFolder, PCUITEMID_CHILD pidl) {
    ComPtr<IShellFolder> shellFolder;
    HRESULT hr = static_cast<IUnknown*>(pFolder)->QueryInterface(
        IID_IShellFolder, shellFolder.PutVoid());
    if (FAILED(hr) || !shellFolder) {
        return std::nullopt;
    }

    STRRET strret;
    if (FAILED(shellFolder->GetDisplayNameOf(pidl, SHGDN_FORPARSING,
                                             &strret))) {
        return std::nullopt;
    }

    PWSTR raw = nullptr;
    if (FAILED(StrRetToStrW(&strret, pidl, &raw)) || !raw) {
        return std::nullopt;
    }

    std::wstring path = raw;
    CoTaskMemFree(raw);

    if (path.starts_with(L"\\\\?\\"sv) &&
        !path.starts_with(L"\\\\?\\UNC\\"sv)) {
        path = path.substr(4);
    }

    if (!IsFileSystemPath(path)) {
        return std::nullopt;
    }

    return path;
}

std::optional<ItemSize> GetItemSizeOnDisk(void* pFolder, PCUITEMID_CHILD pidl) {
    auto path = GetItemPath(pFolder, pidl);
    if (!path) {
        return std::nullopt;
    }
    return GetItemSizeOnDisk(*path);
}

HRESULT SetStrRet(STRRET* strret, PCWSTR text) {
    strret->uType = STRRET_WSTR;
    return SHStrDupW(text, &strret->pOleStr);
}

////////////////////////////////////////////////////////////////////////////////
// windows.storage.dll hooks (CFSFolder)

using CFSFolder_MapColumnToSCID_t = HRESULT(WINAPI*)(void* pThis,
                                                     UINT column,
                                                     PROPERTYKEY* key);
CFSFolder_MapColumnToSCID_t CFSFolder_MapColumnToSCID_Original;

// 0 = not checked yet, 1 = absent, 2 = present.
std::atomic<int> g_nativeColumnState;

bool IsNativeColumnPresent(void* pThis) {
    int state = g_nativeColumnState;
    if (state == 0) {
        state = 1;
        PROPERTYKEY key;
        for (UINT i = 0; i < 10000 && SUCCEEDED(CFSFolder_MapColumnToSCID_Original(
                                          pThis, i, &key));
             i++) {
            if (IsEqualPropertyKey(key, kPKEY_SizeOnDisk)) {
                state = 2;
                break;
            }
        }
        g_nativeColumnState = state;
        Wh_Log(L"Native size on disk column present: %d", state == 2);
    }
    return state == 2;
}

// True if `column` is the extra index we append after Explorer's last column.
bool IsAppendedColumn(void* pThis, UINT column) {
    PROPERTYKEY key;
    if (SUCCEEDED(CFSFolder_MapColumnToSCID_Original(pThis, column, &key))) {
        return false;
    }
    if (column == 0 ||
        FAILED(CFSFolder_MapColumnToSCID_Original(pThis, column - 1, &key))) {
        return false;
    }
    return !IsNativeColumnPresent(pThis);
}

HRESULT WINAPI CFSFolder_MapColumnToSCID_Hook(void* pThis,
                                              UINT column,
                                              PROPERTYKEY* key) {
    auto scope = HookRefCountScope();

    HRESULT hr = CFSFolder_MapColumnToSCID_Original(pThis, column, key);
    if (SUCCEEDED(hr) || !key || !IsAppendedColumn(pThis, column)) {
        return hr;
    }

    *key = kPKEY_SizeOnDisk;
    return S_OK;
}

HRESULT GetColumnKey(void* pThis, UINT column, PROPERTYKEY* key) {
    HRESULT hr = CFSFolder_MapColumnToSCID_Original(pThis, column, key);
    if (FAILED(hr) && IsAppendedColumn(pThis, column)) {
        *key = kPKEY_SizeOnDisk;
        hr = S_OK;
    }
    return hr;
}

using CFSFolder_GetDetailsEx_t = HRESULT(WINAPI*)(void* pThis,
                                                  PCUITEMID_CHILD pidl,
                                                  const PROPERTYKEY* key,
                                                  VARIANT* value);
CFSFolder_GetDetailsEx_t CFSFolder_GetDetailsEx_Original;
HRESULT WINAPI CFSFolder_GetDetailsEx_Hook(void* pThis,
                                           PCUITEMID_CHILD pidl,
                                           const PROPERTYKEY* key,
                                           VARIANT* value) {
    if (!pidl || !key || !value ||
        !IsEqualPropertyKey(*key, kPKEY_SizeOnDisk)) {
        return CFSFolder_GetDetailsEx_Original(pThis, pidl, key, value);
    }

    auto scope = HookRefCountScope();

    VariantInit(value);
    auto item = GetItemSizeOnDisk(pThis, pidl);
    if (item && item->size) {
        value->vt = VT_UI8;
        value->ullVal = *item->size;
    }
    return S_OK;
}

using CFSFolder_GetDetailsOf_t = HRESULT(WINAPI*)(void* pThis,
                                                  PCUITEMID_CHILD pidl,
                                                  UINT column,
                                                  SHELLDETAILS* details);
CFSFolder_GetDetailsOf_t CFSFolder_GetDetailsOf_Original;
HRESULT WINAPI CFSFolder_GetDetailsOf_Hook(void* pThis,
                                           PCUITEMID_CHILD pidl,
                                           UINT column,
                                           SHELLDETAILS* details) {
    if (!details || !IsAppendedColumn(pThis, column)) {
        return CFSFolder_GetDetailsOf_Original(pThis, pidl, column, details);
    }

    auto scope = HookRefCountScope();

    details->fmt = LVCFMT_RIGHT;
    details->cxChar = kColumnWidthChars;

    if (!pidl) {
        return SetStrRet(&details->str, kColumnTitle);
    }

    auto item = GetItemSizeOnDisk(pThis, pidl);
    if (!item || !item->size) {
        return SetStrRet(&details->str, L"");
    }

    PROPVARIANT propVariant;
    PropVariantInit(&propVariant);
    propVariant.vt = VT_UI8;
    propVariant.uhVal.QuadPart = *item->size;

    PWSTR text = nullptr;
    if (FAILED(PSFormatForDisplayAlloc(kPKEY_Size, propVariant, PDFF_DEFAULT,
                                       &text)) ||
        !text) {
        return SetStrRet(&details->str, L"");
    }

    details->str.uType = STRRET_WSTR;
    details->str.pOleStr = text;
    return S_OK;
}

using CFSFolder_GetDefaultColumnState_t = HRESULT(WINAPI*)(void* pThis,
                                                           UINT column,
                                                           SHCOLSTATEF* flags);
CFSFolder_GetDefaultColumnState_t CFSFolder_GetDefaultColumnState_Original;
HRESULT WINAPI CFSFolder_GetDefaultColumnState_Hook(void* pThis,
                                                    UINT column,
                                                    SHCOLSTATEF* flags) {
    if (!flags || !IsAppendedColumn(pThis, column)) {
        return CFSFolder_GetDefaultColumnState_Original(pThis, column, flags);
    }

    // SLOW asks the view to fetch values on a background thread.
    *flags = SHCOLSTATE_TYPE_INT | SHCOLSTATE_SLOW;
    return S_OK;
}

using CFSFolder_CompareIDs_t = HRESULT(WINAPI*)(void* pThis,
                                                LPARAM lParam,
                                                PCUIDLIST_RELATIVE pidl1,
                                                PCUIDLIST_RELATIVE pidl2);
CFSFolder_CompareIDs_t CFSFolder_CompareIDs_Original;
HRESULT WINAPI CFSFolder_CompareIDs_Hook(void* pThis,
                                         LPARAM lParam,
                                         PCUIDLIST_RELATIVE pidl1,
                                         PCUIDLIST_RELATIVE pidl2) {
    auto original = [=]() {
        return CFSFolder_CompareIDs_Original(pThis, lParam, pidl1, pidl2);
    };

    if (!pidl1 || !pidl2 ||
        (lParam & (SHCIDS_ALLFIELDS | SHCIDS_CANONICALONLY))) {
        return original();
    }

    PROPERTYKEY key;
    UINT column = (UINT)(lParam & SHCIDS_COLUMNMASK);
    if (FAILED(GetColumnKey(pThis, column, &key)) ||
        !IsEqualPropertyKey(key, kPKEY_SizeOnDisk)) {
        return original();
    }

    // Only handle direct children.
    if (!ILIsEmpty(ILNext(pidl1)) || !ILIsEmpty(ILNext(pidl2))) {
        return original();
    }

    auto scope = HookRefCountScope();

    // Tie-break (and fallback) by name, which is column 0 in CFSFolder.
    auto byName = [=]() {
        return CFSFolder_CompareIDs_Original(
            pThis, lParam & ~(LPARAM)SHCIDS_COLUMNMASK, pidl1, pidl2);
    };

    auto item1 = GetItemSizeOnDisk(pThis, (PCUITEMID_CHILD)pidl1);
    auto item2 = GetItemSizeOnDisk(pThis, (PCUITEMID_CHILD)pidl2);
    if (!item1 || !item2) {
        return byName();
    }

    if (!g_settings.mixFoldersWhenSorting &&
        item1->isFolder != item2->isFolder) {
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0,
                            item1->isFolder ? (USHORT)-1 : 1);
    }

    ULONGLONG size1 = item1->size.value_or(0);
    ULONGLONG size2 = item2->size.value_or(0);
    if (size1 != size2) {
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0,
                            size1 < size2 ? (USHORT)-1 : 1);
    }

    return byName();
}

bool HookWindowsStorageSymbols() {
    HMODULE module = LoadLibraryExW(L"windows.storage.dll", nullptr,
                                    LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        Wh_Log(L"Failed to load windows.storage.dll");
        return false;
    }

    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {
            {LR"(public: virtual long __cdecl CFSFolder::MapColumnToSCID(unsigned int,struct _tagpropertykey *))"},
            &CFSFolder_MapColumnToSCID_Original,
            CFSFolder_MapColumnToSCID_Hook,
        },
        {
            {LR"(public: virtual long __cdecl CFSFolder::GetDetailsEx(struct _ITEMID_CHILD const __unaligned *,struct _tagpropertykey const *,struct tagVARIANT *))"},
            &CFSFolder_GetDetailsEx_Original,
            CFSFolder_GetDetailsEx_Hook,
        },
        {
            {LR"(public: virtual long __cdecl CFSFolder::CompareIDs(__int64,struct _ITEMIDLIST_RELATIVE const __unaligned *,struct _ITEMIDLIST_RELATIVE const __unaligned *))"},
            &CFSFolder_CompareIDs_Original,
            CFSFolder_CompareIDs_Hook,
        },
        // The two below are best guesses at the symbol names, so they're
        // optional. If they don't resolve, the mod still loads and relies on
        // the property description hooks for the header and column state.
        {
            {LR"(public: virtual long __cdecl CFSFolder::GetDetailsOf(struct _ITEMID_CHILD const __unaligned *,unsigned int,struct _SHELLDETAILS *))"},
            &CFSFolder_GetDetailsOf_Original,
            CFSFolder_GetDetailsOf_Hook,
            true,
        },
        {
            {LR"(public: virtual long __cdecl CFSFolder::GetDefaultColumnState(unsigned int,unsigned long *))"},
            &CFSFolder_GetDefaultColumnState_Original,
            CFSFolder_GetDefaultColumnState_Hook,
            true,
        },
    };

    return WindhawkUtils::HookSymbols(module, hooks, ARRAYSIZE(hooks));
}

////////////////////////////////////////////////////////////////////////////////
// propsys.dll hooks: give System.FileAllocationSize a label, make it viewable
// and format it like the Size column.
//
// These hook the shared IPropertyDescription implementation via its vtable, so
// every hook first checks whether `this` is the size on disk description.
// Vtable slot indices verified against propsys.h.

constexpr int kVtGetDisplayName = 6;
constexpr int kVtGetTypeFlags = 8;
constexpr int kVtGetDefaultColumnWidth = 10;
constexpr int kVtGetDisplayType = 11;
constexpr int kVtGetColumnState = 12;
constexpr int kVtGetAggregationType = 18;
constexpr int kVtFormatForDisplay = 22;

IPropertyDescription* g_sizeDescription;

bool IsSizeOnDiskDescription(IPropertyDescription* description) {
    PROPERTYKEY key;
    return SUCCEEDED(description->GetPropertyKey(&key)) &&
           IsEqualPropertyKey(key, kPKEY_SizeOnDisk);
}

using GetDisplayName_t = HRESULT(STDMETHODCALLTYPE*)(IPropertyDescription*,
                                                     LPWSTR*);
GetDisplayName_t GetDisplayName_Original;
HRESULT STDMETHODCALLTYPE GetDisplayName_Hook(IPropertyDescription* pThis,
                                              LPWSTR* displayName) {
    if (!displayName || !IsSizeOnDiskDescription(pThis)) {
        return GetDisplayName_Original(pThis, displayName);
    }
    return SHStrDupW(kColumnTitle, displayName);
}

using GetTypeFlags_t = HRESULT(STDMETHODCALLTYPE*)(IPropertyDescription*,
                                                   PROPDESC_TYPE_FLAGS,
                                                   PROPDESC_TYPE_FLAGS*);
GetTypeFlags_t GetTypeFlags_Original;
HRESULT STDMETHODCALLTYPE GetTypeFlags_Hook(IPropertyDescription* pThis,
                                            PROPDESC_TYPE_FLAGS mask,
                                            PROPDESC_TYPE_FLAGS* flags) {
    HRESULT hr = GetTypeFlags_Original(pThis, mask, flags);
    if (SUCCEEDED(hr) && flags && IsSizeOnDiskDescription(pThis)) {
        *flags = (PROPDESC_TYPE_FLAGS)((*flags | PDTF_ISVIEWABLE) & mask);
    }
    return hr;
}

using GetDefaultColumnWidth_t =
    HRESULT(STDMETHODCALLTYPE*)(IPropertyDescription*, UINT*);
GetDefaultColumnWidth_t GetDefaultColumnWidth_Original;
HRESULT STDMETHODCALLTYPE
GetDefaultColumnWidth_Hook(IPropertyDescription* pThis, UINT* width) {
    if (!width || !IsSizeOnDiskDescription(pThis)) {
        return GetDefaultColumnWidth_Original(pThis, width);
    }
    *width = kColumnWidthChars;
    return S_OK;
}

using GetDisplayType_t = HRESULT(STDMETHODCALLTYPE*)(IPropertyDescription*,
                                                     PROPDESC_DISPLAYTYPE*);
GetDisplayType_t GetDisplayType_Original;
HRESULT STDMETHODCALLTYPE GetDisplayType_Hook(IPropertyDescription* pThis,
                                              PROPDESC_DISPLAYTYPE* type) {
    if (!type || !IsSizeOnDiskDescription(pThis)) {
        return GetDisplayType_Original(pThis, type);
    }
    *type = PDDT_NUMBER;
    return S_OK;
}

using GetColumnState_t = HRESULT(STDMETHODCALLTYPE*)(IPropertyDescription*,
                                                     SHCOLSTATEF*);
GetColumnState_t GetColumnState_Original;
HRESULT STDMETHODCALLTYPE GetColumnState_Hook(IPropertyDescription* pThis,
                                              SHCOLSTATEF* flags) {
    if (!flags || !IsSizeOnDiskDescription(pThis)) {
        return GetColumnState_Original(pThis, flags);
    }
    *flags = SHCOLSTATE_TYPE_INT | SHCOLSTATE_SLOW;
    return S_OK;
}

using GetAggregationType_t =
    HRESULT(STDMETHODCALLTYPE*)(IPropertyDescription*, PROPDESC_AGGREGATION_TYPE*);
GetAggregationType_t GetAggregationType_Original;
HRESULT STDMETHODCALLTYPE
GetAggregationType_Hook(IPropertyDescription* pThis,
                        PROPDESC_AGGREGATION_TYPE* type) {
    if (!type || !IsSizeOnDiskDescription(pThis)) {
        return GetAggregationType_Original(pThis, type);
    }
    *type = PDAT_SUM;
    return S_OK;
}

using FormatForDisplay_t = HRESULT(STDMETHODCALLTYPE*)(IPropertyDescription*,
                                                       const PROPVARIANT&,
                                                       PROPDESC_FORMAT_FLAGS,
                                                       LPWSTR*);
FormatForDisplay_t FormatForDisplay_Original;
HRESULT STDMETHODCALLTYPE FormatForDisplay_Hook(IPropertyDescription* pThis,
                                                const PROPVARIANT& value,
                                                PROPDESC_FORMAT_FLAGS flags,
                                                LPWSTR* display) {
    if (!g_sizeDescription || !IsSizeOnDiskDescription(pThis)) {
        return FormatForDisplay_Original(pThis, value, flags, display);
    }
    // Format exactly like the Size column.
    return FormatForDisplay_Original(g_sizeDescription, value, flags, display);
}

// Explorer formats column text through these exports too, which may not go
// through the vtable. Swap our key for Size so the text is formatted the same.
using PSFormatForDisplayAlloc_t = decltype(&PSFormatForDisplayAlloc);
PSFormatForDisplayAlloc_t PSFormatForDisplayAlloc_Original;
HRESULT WINAPI PSFormatForDisplayAlloc_Hook(const PROPERTYKEY& key,
                                            const PROPVARIANT& value,
                                            PROPDESC_FORMAT_FLAGS flags,
                                            PWSTR* display) {
    return PSFormatForDisplayAlloc_Original(
        IsEqualPropertyKey(key, kPKEY_SizeOnDisk) ? kPKEY_Size : key, value,
        flags, display);
}

using PSFormatForDisplay_t = decltype(&PSFormatForDisplay);
PSFormatForDisplay_t PSFormatForDisplay_Original;
HRESULT WINAPI PSFormatForDisplay_Hook(const PROPERTYKEY& key,
                                       const PROPVARIANT& value,
                                       PROPDESC_FORMAT_FLAGS flags,
                                       LPWSTR text,
                                       DWORD textLength) {
    return PSFormatForDisplay_Original(
        IsEqualPropertyKey(key, kPKEY_SizeOnDisk) ? kPKEY_Size : key, value,
        flags, text, textLength);
}

bool HookPropertyDescription() {
    IPropertyDescription* description = nullptr;
    HRESULT hr = PSGetPropertyDescription(kPKEY_SizeOnDisk,
                                          IID_PPV_ARGS(&description));
    if (FAILED(hr) || !description) {
        Wh_Log(L"PSGetPropertyDescription failed: %08X", hr);
        return false;
    }

    hr = PSGetPropertyDescription(kPKEY_Size, IID_PPV_ARGS(&g_sizeDescription));
    if (FAILED(hr)) {
        Wh_Log(L"PSGetPropertyDescription (Size) failed: %08X", hr);
        g_sizeDescription = nullptr;
    }

    void** vtable = *reinterpret_cast<void***>(description);

    auto hook = [vtable](int index, void* hookFunction, void** original) {
        if (!Wh_SetFunctionHook(vtable[index], hookFunction, original)) {
            Wh_Log(L"Failed to hook vtable slot %d", index);
        }
    };

    hook(kVtGetDisplayName, (void*)GetDisplayName_Hook,
         (void**)&GetDisplayName_Original);
    hook(kVtGetTypeFlags, (void*)GetTypeFlags_Hook,
         (void**)&GetTypeFlags_Original);
    hook(kVtGetDefaultColumnWidth, (void*)GetDefaultColumnWidth_Hook,
         (void**)&GetDefaultColumnWidth_Original);
    hook(kVtGetDisplayType, (void*)GetDisplayType_Hook,
         (void**)&GetDisplayType_Original);
    hook(kVtGetColumnState, (void*)GetColumnState_Hook,
         (void**)&GetColumnState_Original);
    hook(kVtGetAggregationType, (void*)GetAggregationType_Hook,
         (void**)&GetAggregationType_Original);
    hook(kVtFormatForDisplay, (void*)FormatForDisplay_Hook,
         (void**)&FormatForDisplay_Original);

    // Property descriptions are cached by the property system for the life of
    // the process, so releasing our reference doesn't free the vtable.
    description->Release();
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// Mod lifecycle

void LoadSettings() {
    PCWSTR folderSizes = Wh_GetStringSetting(L"folderSizes");
    g_settings.folderSizes = FolderSizes::always;
    if (wcscmp(folderSizes, L"withShiftKey") == 0) {
        g_settings.folderSizes = FolderSizes::withShiftKey;
    } else if (wcscmp(folderSizes, L"disabled") == 0) {
        g_settings.folderSizes = FolderSizes::disabled;
    }
    Wh_FreeStringSetting(folderSizes);

    PCWSTR accuracy = Wh_GetStringSetting(L"folderAccuracy");
    g_settings.accurateFolders = wcscmp(accuracy, L"accurate") == 0;
    Wh_FreeStringSetting(accuracy);

    g_settings.networkFolders = Wh_GetIntSetting(L"networkFolders");
    g_settings.mixFoldersWhenSorting =
        Wh_GetIntSetting(L"mixFoldersWhenSorting");

    int cacheSeconds = Wh_GetIntSetting(L"cacheSeconds");
    if (cacheSeconds < 0) {
        cacheSeconds = 0;
    }
    g_settings.cacheMs = (ULONGLONG)cacheSeconds * 1000;
}

BOOL Wh_ModInit() {
    Wh_Log(L">");

    LoadSettings();

    if (!HookWindowsStorageSymbols()) {
        Wh_Log(L"Failed hooking windows.storage.dll symbols");
        return FALSE;
    }

    if (!HookPropertyDescription()) {
        Wh_Log(L"Property description hooks unavailable; continuing");
    }

    WindhawkUtils::Wh_SetFunctionHookT(PSFormatForDisplayAlloc,
                                       PSFormatForDisplayAlloc_Hook,
                                       &PSFormatForDisplayAlloc_Original);
    WindhawkUtils::Wh_SetFunctionHookT(PSFormatForDisplay,
                                       PSFormatForDisplay_Hook,
                                       &PSFormatForDisplay_Original);

    return TRUE;
}

void Wh_ModUninit() {
    Wh_Log(L">");

    while (g_hookRefCount > 0) {
        Sleep(200);
    }

    if (g_sizeDescription) {
        g_sizeDescription->Release();
        g_sizeDescription = nullptr;
    }
}

BOOL Wh_ModSettingsChanged(BOOL* bReload) {
    Wh_Log(L">");
    *bReload = TRUE;
    return TRUE;
}
