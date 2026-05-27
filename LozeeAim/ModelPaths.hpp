#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <string>

namespace model_paths {

inline std::string ExeDirA() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string value(path);
    const size_t pos = value.find_last_of("\\/");
    return pos == std::string::npos ? ".\\" : value.substr(0, pos + 1);
}

inline std::wstring ExeDirW() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring value(path);
    const size_t pos = value.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L".\\" : value.substr(0, pos + 1);
}

inline bool IsAbsoluteA(const std::string& path) {
    if (path.size() >= 2 && path[1] == ':') {
        return true;
    }
    return path.size() >= 2 &&
        (path[0] == '\\' || path[0] == '/') &&
        (path[1] == '\\' || path[1] == '/');
}

inline bool IsAbsoluteW(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') {
        return true;
    }
    return path.size() >= 2 &&
        (path[0] == L'\\' || path[0] == L'/') &&
        (path[1] == L'\\' || path[1] == L'/');
}

inline bool FileExistsA(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool FileExistsW(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline void EnsureDirA(const std::string& path) {
    CreateDirectoryA(path.c_str(), nullptr);
}

inline std::string ModelsDirA() {
    return ExeDirA() + "models\\";
}

inline std::string YoloDirA() {
    return ModelsDirA() + "yolo\\";
}

inline std::string NnDirA() {
    return ModelsDirA() + "nn\\";
}

inline std::wstring ModelsDirW() {
    return ExeDirW() + L"models\\";
}

inline std::wstring YoloDirW() {
    return ModelsDirW() + L"yolo\\";
}

inline void EnsureModelDirs() {
    EnsureDirA(ModelsDirA());
    EnsureDirA(YoloDirA());
    EnsureDirA(NnDirA());
}

inline std::string BaseNameA(const std::string& path) {
    const size_t pos = path.find_last_of("\\/");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

inline std::wstring BaseNameW(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

inline std::string YoloConfigPath(const std::string& filename) {
    return "models\\yolo\\" + BaseNameA(filename);
}

inline std::string NnConfigPath(const std::string& filename) {
    return "models\\nn\\" + BaseNameA(filename);
}

inline std::string ExeRelativeA(const std::string& relative_path) {
    return ExeDirA() + relative_path;
}

inline std::wstring ExeRelativeW(const std::wstring& relative_path) {
    return ExeDirW() + relative_path;
}

inline std::string ResolveExistingPathA(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    if (IsAbsoluteA(path)) {
        return FileExistsA(path) ? path : std::string{};
    }
    const std::string exe_relative = ExeRelativeA(path);
    if (FileExistsA(exe_relative)) {
        return exe_relative;
    }
    return FileExistsA(path) ? path : std::string{};
}

inline std::wstring ResolveExistingPathW(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }
    if (IsAbsoluteW(path)) {
        return FileExistsW(path) ? path : std::wstring{};
    }
    const std::wstring exe_relative = ExeRelativeW(path);
    if (FileExistsW(exe_relative)) {
        return exe_relative;
    }
    return FileExistsW(path) ? path : std::wstring{};
}

inline std::string ResolveOutputPathA(const std::string& config_path, const std::string& model_dir, const std::string& default_name) {
    EnsureModelDirs();
    if (config_path.empty()) {
        return model_dir + default_name;
    }
    if (IsAbsoluteA(config_path)) {
        return config_path;
    }
    const std::string base = BaseNameA(config_path);
    if (config_path.find('\\') == std::string::npos && config_path.find('/') == std::string::npos) {
        return model_dir + base;
    }
    return ExeRelativeA(config_path);
}

inline std::string ReplaceExtension(std::string path, const std::string& extension) {
    const size_t slash = path.find_last_of("\\/");
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        path.resize(dot);
    }
    path += extension;
    return path;
}

}  // namespace model_paths
