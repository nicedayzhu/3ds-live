#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>

static int startupError(DWORD code) {
    if (__argc > 1) {
        fprintf(stderr, "3DS Live could not load its runtime. Extract the entire package and keep the runtime "
                        "folder next to 3DS-Live.exe (Windows error %lu).\n", code);
    } else {
        wchar_t message[256]{};
        swprintf_s(message, L"无法加载 3DS Live。请完整解压发布包，并将 runtime 文件夹保留在程序旁。\nWindows 错误 %lu", code);
        MessageBoxW(nullptr, message, L"3DS Live", MB_OK | MB_ICONERROR);
    }
    return 1;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) return startupError(GetLastError());
    path.resize(length);
    const auto separator = path.find_last_of(L'\\');
    if (separator == std::wstring::npos) return startupError(ERROR_BAD_PATHNAME);
    path.resize(separator + 1);
    const std::wstring runtime = path + L"runtime";
    constexpr DWORD search = LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32;
    if (!SetDefaultDllDirectories(search) || !AddDllDirectory(runtime.c_str()))
        return startupError(GetLastError());
    const std::wstring acceleration = runtime + L"\\acceleration";
    const DWORD attributes = GetFileAttributesW(acceleration.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
        AddDllDirectory(acceleration.c_str());
    const std::wstring client = runtime + L"\\3ds-live-client.dll";
    const HMODULE module = LoadLibraryExW(client.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | search);
    if (!module) return startupError(GetLastError());
    using Entry = int (__cdecl *)(int, char **, const wchar_t *);
    const auto run = reinterpret_cast<Entry>(GetProcAddress(module, "run3dsLive"));
    if (!run) return startupError(GetLastError());
    return run(__argc, __argv, runtime.c_str());
}
