#include "RuntimeDependencyLoader.hpp"

#include "Config.hpp"
#include "ModelPaths.hpp"

#include <algorithm>
#include <cctype>
#include <delayimp.h>
#include <fstream>
#include <vector>

namespace {

int g_active_backend = runtime_deps::BackendDirectML;
std::vector<DLL_DIRECTORY_COOKIE> g_dll_dir_cookies;
std::vector<HMODULE> g_preloaded_modules;

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 1) {
        return {};
    }

    std::vector<wchar_t> buffer(static_cast<size_t>(size));
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, buffer.data(), size);
    return std::wstring(buffer.data());
}

void AddDllDir(const std::string& dir) {
    const std::wstring wide = Utf8ToWide(dir);
    if (wide.empty()) {
        return;
    }
    DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(wide.c_str());
    if (cookie) {
        g_dll_dir_cookies.push_back(cookie);
    }
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string Trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string EnsureTrailingSlash(std::string path) {
    path = Trim(path);
    if (!path.empty() && path.back() != '\\' && path.back() != '/') {
        path += '\\';
    }
    return path;
}

int BackendFromConfigOrActive() {
    Config cfg;
    cfg.Load();
    const int backend = cfg.use_cpu_inference ? runtime_deps::BackendCPU : cfg.inference_provider;
    if (backend < runtime_deps::BackendDirectML || backend > runtime_deps::BackendCPU) {
        return g_active_backend;
    }
    return backend;
}

bool DirectoryExists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::string FormatWin32Error(DWORD error) {
    char* message = nullptr;
    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&message),
        0,
        nullptr);

    std::string result = message ? Trim(message) : ("Win32 error " + std::to_string(error));
    if (message) {
        LocalFree(message);
    }
    return result;
}

bool PreloadDllFromPath(const std::string& path, const std::string& name, std::string* error) {
    if (!model_paths::FileExistsA(path)) {
        if (error) {
            *error = name + " missing: " + path;
        }
        return false;
    }

    HMODULE module = LoadLibraryExA(
        path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!module) {
        if (error) {
            *error = name + " load failed from " + path + ": " + FormatWin32Error(GetLastError());
        }
        return false;
    }

    g_preloaded_modules.push_back(module);
    return true;
}

std::string ReadPathValue(const std::string& key) {
    std::ifstream file(runtime_deps::DepPathsConfigPath());
    if (!file) {
        return {};
    }

    std::string line;
    while (std::getline(file, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }

        const size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        if (Trim(trimmed.substr(0, eq)) == key) {
            return EnsureTrailingSlash(trimmed.substr(eq + 1));
        }
    }

    return {};
}

void WritePathValue(const std::string& key, const std::string& value) {
    const std::string path = runtime_deps::DepPathsConfigPath();
    std::vector<std::string> lines;

    {
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line)) {
            lines.push_back(line);
        }
    }

    bool replaced = false;
    const std::string normalized = EnsureTrailingSlash(value);
    for (std::string& line : lines) {
        const std::string trimmed = Trim(line);
        const size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        if (Trim(trimmed.substr(0, eq)) == key) {
            line = key + "=" + normalized;
            replaced = true;
            break;
        }
    }

    if (!replaced) {
        lines.push_back(key + "=" + normalized);
    }

    std::ofstream file(path, std::ios::out | std::ios::trunc);
    for (const std::string& line : lines) {
        file << line << "\n";
    }
}

bool IsLegacyDependencyName(const std::string& filename) {
    const std::string lower = Lower(filename);
    return lower == "onnxruntime.dll" ||
        lower == "directml.dll" ||
        lower == "opencv_world4120.dll" ||
        lower.rfind("onnxruntime_providers_", 0) == 0 ||
        lower.rfind("cudart64_", 0) == 0 ||
        lower.rfind("cublas", 0) == 0 ||
        lower.rfind("cudnn", 0) == 0 ||
        lower.rfind("nvinfer", 0) == 0 ||
        lower == "nvonnxparser_10.dll";
}

HMODULE LoadRuntimeLibrary(const char* dll_name) {
    if (!dll_name) {
        return nullptr;
    }

    const std::string lower = Lower(dll_name);
    const int backend = lower == "onnxruntime.dll" ? BackendFromConfigOrActive() : g_active_backend;
    const std::string path = runtime_deps::DependencyPath(dll_name, backend);
    if (path.empty() || !model_paths::FileExistsA(path)) {
        return nullptr;
    }
    return LoadLibraryExA(
        path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
}

FARPROC WINAPI DelayLoadHook(unsigned dli_notify, PDelayLoadInfo delay_load_info) {
    if (dli_notify != dliNotePreLoadLibrary || !delay_load_info || !delay_load_info->szDll) {
        return nullptr;
    }

    const std::string dll_name = Lower(delay_load_info->szDll);
    if (dll_name == "onnxruntime.dll" || dll_name == "opencv_world4120.dll" || dll_name == "opencv_world4120d.dll") {
        return reinterpret_cast<FARPROC>(LoadRuntimeLibrary(delay_load_info->szDll));
    }
    return nullptr;
}

}  // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = DelayLoadHook;

namespace runtime_deps {

std::string DepsRootDir() {
    return model_paths::ExeDirA() + "deps\\";
}

std::string CommonDir() {
    return DepsRootDir() + "common\\";
}

std::string BackendDir(int backend) {
    switch (backend) {
    case BackendTensorRT:
        return DepsRootDir() + "tensorrt\\";
    case BackendCPU:
        return DepsRootDir() + "cpu\\";
    case BackendDirectML:
    default:
        return DepsRootDir() + "directml\\";
    }
}

std::string DepPathsConfigPath() {
    return model_paths::ExeDirA() + "dep_paths.ini";
}

void EnsureDependencyDirs() {
    model_paths::EnsureDirA(DepsRootDir());
    model_paths::EnsureDirA(CommonDir());
    model_paths::EnsureDirA(BackendDir(BackendDirectML));
    model_paths::EnsureDirA(BackendDir(BackendTensorRT));
    model_paths::EnsureDirA(BackendDir(BackendCPU));
}

std::string TensorRtCudaDir() {
    return ReadPathValue("tensorrt_cuda_dir");
}

std::string TensorRtCudnnDir() {
    return ReadPathValue("tensorrt_cudnn_dir");
}

void SetTensorRtCudaDir(const std::string& dir) {
    WritePathValue("tensorrt_cuda_dir", dir);
}

void SetTensorRtCudnnDir(const std::string& dir) {
    WritePathValue("tensorrt_cudnn_dir", dir);
}

std::vector<std::string> TensorRtReferenceDirs() {
    std::vector<std::string> dirs;
    const std::string cuda_dir = TensorRtCudaDir();
    const std::string cudnn_dir = TensorRtCudnnDir();
    if (DirectoryExists(cuda_dir)) {
        dirs.push_back(cuda_dir);
    }
    if (DirectoryExists(cudnn_dir) && Lower(cudnn_dir) != Lower(cuda_dir)) {
        dirs.push_back(cudnn_dir);
    }
    return dirs;
}

std::string InstallDirForDependency(const std::string& filename, int backend) {
    const std::string lower = Lower(filename);
    if (lower == "opencv_world4120.dll" || lower == "opencv_world4120d.dll") {
        return CommonDir();
    }
    if (lower == "*.onnx") {
        return model_paths::YoloDirA();
    }
    return BackendDir(backend);
}

std::string DependencyPath(const std::string& filename, int backend) {
    const std::string lower = Lower(filename);
    if (lower == "opencv_world4120.dll" || lower == "opencv_world4120d.dll") {
        return CommonDir() + filename;
    }
    if (lower == "directml.dll") {
        return BackendDir(BackendDirectML) + filename;
    }
    if (lower == "onnxruntime.dll") {
        return BackendDir(backend) + filename;
    }
    if (lower.rfind("onnxruntime_providers_", 0) == 0 ||
        lower.rfind("cudart64_", 0) == 0 ||
        lower.rfind("cublas", 0) == 0 ||
        lower.rfind("cudnn", 0) == 0 ||
        lower.rfind("nvinfer", 0) == 0 ||
        lower == "nvonnxparser_10.dll") {
        return BackendDir(BackendTensorRT) + filename;
    }
    return InstallDirForDependency(filename, backend) + filename;
}

std::vector<std::string> ListRootLegacyDependencies() {
    std::vector<std::string> files;
    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA((model_paths::ExeDirA() + "*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return files;
    }

    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const std::string filename = fd.cFileName;
        if (IsLegacyDependencyName(filename)) {
            files.push_back(filename);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    std::sort(files.begin(), files.end());
    return files;
}

bool Configure(int backend, bool use_cpu, std::string* error) {
    EnsureDependencyDirs();
    g_active_backend = use_cpu ? BackendCPU : backend;
    if (g_active_backend < BackendDirectML || g_active_backend > BackendCPU) {
        g_active_backend = BackendDirectML;
    }

    SetDllDirectoryA("");
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS)) {
        if (error) {
            *error = "SetDefaultDllDirectories failed";
        }
        return false;
    }

    AddDllDir(CommonDir());
    AddDllDir(BackendDir(g_active_backend));
    if (g_active_backend == BackendTensorRT) {
        for (const std::string& dir : TensorRtReferenceDirs()) {
            AddDllDir(dir);
        }
    }
    return true;
}

bool ConfigureFromConfig(std::string* error) {
    Config cfg;
    cfg.Load();
    return Configure(cfg.inference_provider, cfg.use_cpu_inference, error);
}

bool PreloadTensorRtProviderDependencies(std::string* error) {
    const std::vector<std::string> required_files = {
        "onnxruntime_providers_shared.dll",
        "onnxruntime_providers_tensorrt.dll",
        "onnxruntime_providers_cuda.dll",
        "nvinfer_10.dll",
        "nvonnxparser_10.dll",
    };

    const std::string backend_dir = BackendDir(BackendTensorRT);
    for (const std::string& filename : required_files) {
        const std::string path = backend_dir + filename;
        if (!model_paths::FileExistsA(path)) {
            if (error) {
                *error = filename + " missing: " + path;
            }
            return false;
        }
    }

    if (!PreloadDllFromPath(
        backend_dir + "onnxruntime_providers_shared.dll",
        "onnxruntime_providers_shared.dll",
        error)) {
        return false;
    }

    return true;
}

}  // namespace runtime_deps
