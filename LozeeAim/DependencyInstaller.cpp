#pragma execution_character_set("utf-8")

#include "DependencyInstaller.hpp"
#include "ModelPaths.hpp"
#include "RuntimeDependencyLoader.hpp"
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"
#include <onnxruntime_c_api.h>
#include <winhttp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <io.h>
#include <fstream>
#include <commdlg.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// static instance pointer for WndProc
// ---------------------------------------------------------------------------
static DependencyInstaller* g_installer = nullptr;

namespace {
std::string FormatWin32Error(DWORD error);

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string BaseNameOfPath(const std::string& path) {
    const size_t pos = path.find_last_of("\\/");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string ParentDirOfPath(const std::string& path) {
    const size_t pos = path.find_last_of("\\/");
    if (pos == std::string::npos) {
        return {};
    }
    return path.substr(0, pos + 1);
}

std::string EnsureTrailingSlash(std::string path) {
    if (!path.empty() && path.back() != '\\' && path.back() != '/') {
        path += '\\';
    }
    return path;
}

std::string NormalizeDirForCompare(std::string path) {
    char full_path[MAX_PATH] = {};
    const DWORD size = GetFullPathNameA(path.c_str(), MAX_PATH, full_path, nullptr);
    if (size > 0 && size < MAX_PATH) {
        path = full_path;
    }
    path = EnsureTrailingSlash(path);
    std::replace(path.begin(), path.end(), '/', '\\');
    return ToLowerAscii(path);
}

bool SameDirectoryPath(const std::string& lhs, const std::string& rhs) {
    return NormalizeDirForCompare(lhs) == NormalizeDirForCompare(rhs);
}

std::string EscapePowerShellSingleQuoted(std::string value) {
    size_t pos = 0;
    while ((pos = value.find('\'', pos)) != std::string::npos) {
        value.insert(pos, "'");
        pos += 2;
    }
    return value;
}

std::vector<std::string> SplitPatternList(const std::string& patterns) {
    std::vector<std::string> values;
    std::stringstream ss(patterns);
    std::string item;
    while (std::getline(ss, item, ';')) {
        if (!item.empty()) {
            values.push_back(item);
        }
    }
    return values;
}

bool WildcardMatchInsensitive(const char* pattern, const char* text) {
    while (*pattern) {
        if (*pattern == '*') {
            ++pattern;
            if (*pattern == '\0') {
                return true;
            }
            while (*text) {
                if (WildcardMatchInsensitive(pattern, text)) {
                    return true;
                }
                ++text;
            }
            return false;
        }

        if (*pattern == '?') {
            if (*text == '\0') {
                return false;
            }
            ++pattern;
            ++text;
            continue;
        }

        if (std::tolower(static_cast<unsigned char>(*pattern)) !=
            std::tolower(static_cast<unsigned char>(*text))) {
            return false;
        }
        ++pattern;
        ++text;
    }
    return *text == '\0';
}

bool MatchesAnyPattern(const std::string& filename, const std::vector<std::string>& patterns) {
    for (const auto& pattern : patterns) {
        const std::string leaf_pattern = BaseNameOfPath(pattern);
        if (WildcardMatchInsensitive(leaf_pattern.c_str(), filename.c_str())) {
            return true;
        }
    }
    return false;
}

void CollectMatchingFiles(
    const std::string& dir,
    const std::vector<std::string>& patterns,
    std::vector<std::string>& out_files) {
    const std::string search_dir = EnsureTrailingSlash(dir);
    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA((search_dir + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        const std::string name = fd.cFileName;
        if (name == "." || name == "..") {
            continue;
        }

        const std::string path = search_dir + name;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            CollectMatchingFiles(path, patterns, out_files);
        } else if (MatchesAnyPattern(name, patterns)) {
            out_files.push_back(path);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

bool DeleteDirectoryTree(const std::string& dir) {
    const DWORD attrs = GetFileAttributesA(dir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return DeleteFileA(dir.c_str()) != FALSE;
    }

    const std::string search_dir = EnsureTrailingSlash(dir);
    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA((search_dir + "*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::string name = fd.cFileName;
            if (name == "." || name == "..") {
                continue;
            }

            const std::string path = search_dir + name;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                DeleteDirectoryTree(path);
            } else {
                SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(path.c_str());
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    SetFileAttributesA(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryA(dir.c_str()) != FALSE;
}

int DeleteMatchingFilesInDirectory(const std::string& dir, const std::vector<std::string>& patterns) {
    int deleted = 0;
    const std::string search_dir = EnsureTrailingSlash(dir);
    for (const std::string& pattern : patterns) {
        WIN32_FIND_DATAA fd = {};
        HANDLE h = FindFirstFileA((search_dir + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                continue;
            }
            const std::string path = search_dir + fd.cFileName;
            SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileA(path.c_str())) {
                ++deleted;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return deleted;
}

int CleanTensorRtRuntimeFiles(const std::string& dir) {
    return DeleteMatchingFilesInDirectory(
        dir,
        {
            "nvinfer_10.dll",
            "nvinfer_dispatch_10.dll",
            "nvinfer_lean_10.dll",
            "nvinfer_plugin_10.dll",
            "nvinfer_vc_plugin_10.dll",
            "nvonnxparser_10.dll",
            "nvinfer_builder_resource_10.dll",
            "nvinfer_builder_resource_*_10.dll",
            "trtexec.exe",
        });
}

std::string BrowseFolder(HWND owner, const char* title) {
    BROWSEINFOA bi = {};
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
    if (!pidl) {
        return {};
    }

    char path[MAX_PATH] = {};
    const bool ok = SHGetPathFromIDListA(pidl, path) != FALSE;
    CoTaskMemFree(pidl);
    return ok ? std::string(path) : std::string{};
}

struct CudaInstallCandidate {
    std::string bin_dir;
    int major = 0;
    int minor = 0;
};

constexpr int kTensorRtSupportedCudaMajor = 12;
constexpr int kTensorRtMaxSupportedCudaMinor = 8;
constexpr const char* kTensorRtRecommendedStack = "TensorRT 10.9 + CUDA 12.8 + cuDNN 9";

bool ParseCudaVersionDir(const std::string& name, int* major, int* minor) {
    if (!major || !minor) {
        return false;
    }
    int parsed_major = 0;
    int parsed_minor = 0;
    if (sscanf_s(name.c_str(), "v%d.%d", &parsed_major, &parsed_minor) != 2) {
        return false;
    }
    *major = parsed_major;
    *minor = parsed_minor;
    return true;
}

bool ParseCudaVersionToken(const std::string& token, int* major, int* minor) {
    if (!major || !minor) {
        return false;
    }

    const std::string lower = ToLowerAscii(token);
    int parsed_major = 0;
    int parsed_minor = 0;
    if (sscanf_s(lower.c_str(), "v%d.%d", &parsed_major, &parsed_minor) == 2 ||
        sscanf_s(lower.c_str(), "cuda-%d.%d", &parsed_major, &parsed_minor) == 2 ||
        sscanf_s(lower.c_str(), "%d.%d", &parsed_major, &parsed_minor) == 2) {
        *major = parsed_major;
        *minor = parsed_minor;
        return true;
    }
    return false;
}

bool ParseCudaVersionFromPath(std::string path, int* major, int* minor) {
    std::replace(path.begin(), path.end(), '/', '\\');
    size_t start = 0;
    while (start < path.size()) {
        const size_t end = path.find('\\', start);
        const std::string token = path.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        if (ParseCudaVersionToken(token, major, minor)) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

bool IsSupportedTensorRtCudaVersion(int major, int minor) {
    return major == kTensorRtSupportedCudaMajor && minor <= kTensorRtMaxSupportedCudaMinor;
}

bool ResolveConfiguredTensorRtCudaVersion(int* major, int* minor) {
    return ParseCudaVersionFromPath(runtime_deps::TensorRtCudaDir(), major, minor);
}

std::string FormatCudaVersion(int major, int minor) {
    return "v" + std::to_string(major) + "." + std::to_string(minor);
}

bool HasRequiredCuda12RuntimeFiles(const std::string& bin_dir) {
    const std::string dir = EnsureTrailingSlash(bin_dir);
    return _access((dir + "cudart64_12.dll").c_str(), 0) != -1 &&
        _access((dir + "cublas64_12.dll").c_str(), 0) != -1 &&
        _access((dir + "cublasLt64_12.dll").c_str(), 0) != -1;
}

bool HasSupportedCuda12RuntimeFiles(const std::string& bin_dir, std::string* issue = nullptr) {
    const std::string dir = EnsureTrailingSlash(bin_dir);
    if (dir.empty()) {
        if (issue) {
            *issue = "CUDA 目录未设置，推荐 " + std::string(kTensorRtRecommendedStack) + "。";
        }
        return false;
    }

    if (!HasRequiredCuda12RuntimeFiles(dir)) {
        if (issue) {
            *issue = "CUDA 目录缺少 cudart64_12.dll、cublas64_12.dll、cublasLt64_12.dll 或 cufft64_11.dll。";
        }
        return false;
    }

    int major = 0;
    int minor = 0;
    if (!ParseCudaVersionFromPath(dir, &major, &minor)) {
        if (issue) {
            *issue = "无法从路径确认 CUDA 版本，请选择 CUDA v12.0-v12.8 的 bin 目录。";
        }
        return false;
    }

    if (!IsSupportedTensorRtCudaVersion(major, minor)) {
        if (issue) {
            *issue = "当前 CUDA v" + std::to_string(major) + "." + std::to_string(minor) +
                " 超出 ONNX Runtime TensorRT 兼容范围，请使用 CUDA v12.0-v12.8。";
        }
        return false;
    }

    return true;
}

const std::vector<std::string>& RequiredCuda12RuntimeFiles() {
    static const std::vector<std::string> files = {
        "cudart64_12.dll",
        "cublas64_12.dll",
        "cublasLt64_12.dll",
        "cufft64_11.dll",
    };
    return files;
}

const std::vector<std::string>& RequiredOnnxRuntimeTensorRtProviderFiles() {
    static const std::vector<std::string> files = {
        "onnxruntime_providers_shared.dll",
        "onnxruntime_providers_tensorrt.dll",
        "onnxruntime_providers_cuda.dll",
    };
    return files;
}

const std::vector<std::string>& RequiredTensorRtRuntimeFiles() {
    static const std::vector<std::string> files = {
        "nvinfer_10.dll",
        "nvinfer_dispatch_10.dll",
        "nvinfer_lean_10.dll",
        "nvinfer_plugin_10.dll",
        "nvinfer_vc_plugin_10.dll",
        "nvonnxparser_10.dll",
        "nvinfer_builder_resource_10.dll",
    };
    return files;
}

const std::vector<std::string>& RequiredTensorRtRuntimePatterns() {
    static const std::vector<std::string> files = {
    };
    return files;
}

const std::vector<std::string>& RequiredCudnnRuntimeFiles() {
    static const std::vector<std::string> files = {
        "cudnn64_9.dll",
        "cudnn_adv64_9.dll",
        "cudnn_cnn64_9.dll",
        "cudnn_engines_precompiled64_9.dll",
        "cudnn_engines_runtime_compiled64_9.dll",
        "cudnn_graph64_9.dll",
        "cudnn_heuristic64_9.dll",
        "cudnn_ops64_9.dll",
    };
    return files;
}

bool HasCudnnRuntimeFiles(const std::string& bin_dir) {
    const std::string dir = EnsureTrailingSlash(bin_dir);
    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA((dir + "cudnn*_9.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    FindClose(h);
    return true;
}

bool HasRequiredCudnnRuntimeFiles(const std::string& bin_dir) {
    const std::string dir = EnsureTrailingSlash(bin_dir);
    for (const std::string& filename : RequiredCudnnRuntimeFiles()) {
        if (_access((dir + filename).c_str(), 0) == -1) {
            return false;
        }
    }
    return true;
}

bool HasSupportedCudnnRuntimeFiles(const std::string& bin_dir, std::string* issue = nullptr) {
    const std::string dir = EnsureTrailingSlash(bin_dir);
    if (dir.empty()) {
        if (issue) {
            *issue = "cuDNN 目录未设置，请使用与当前 CUDA 版本同步的 cuDNN 9。";
        }
        return false;
    }

    if (!HasRequiredCudnnRuntimeFiles(dir)) {
        if (issue) {
            *issue = "cuDNN 目录缺少 cudnn*_9.dll。";
        }
        return false;
    }

    int major = 0;
    int minor = 0;
    if (!ParseCudaVersionFromPath(dir, &major, &minor)) {
        return true;
    }

    if (!IsSupportedTensorRtCudaVersion(major, minor)) {
        if (issue) {
            *issue = "当前 cuDNN 目录面向 CUDA v" + std::to_string(major) + "." +
                std::to_string(minor) + "，请使用 CUDA v12.0-v12.8 对应的 cuDNN 9。";
        }
        return false;
    }

    int cuda_major = 0;
    int cuda_minor = 0;
    if (ResolveConfiguredTensorRtCudaVersion(&cuda_major, &cuda_minor) &&
        (major != cuda_major || minor != cuda_minor)) {
        if (issue) {
            *issue = "当前 cuDNN 目录面向 CUDA " + FormatCudaVersion(major, minor) +
                "，但当前 CUDA 是 " + FormatCudaVersion(cuda_major, cuda_minor) +
                "；请使用同版本目录或 cuda12 通用包。";
        }
        return false;
    }

    return true;
}

bool HasAllFilesInDirectory(const std::string& dir, const std::vector<std::string>& files) {
    const std::string search_dir = EnsureTrailingSlash(dir);
    for (const std::string& filename : files) {
        if (_access((search_dir + filename).c_str(), 0) == -1) {
            return false;
        }
    }
    return true;
}

std::string JoinFileNames(const std::vector<std::string>& files) {
    std::string result;
    for (const std::string& file : files) {
        if (!result.empty()) {
            result += ", ";
        }
        result += file;
    }
    return result;
}

bool FindDependencyInDirectory(const std::string& dir, const std::string& filename, std::string* resolved_path) {
    if (dir.empty()) {
        return false;
    }

    const std::string search_dir = EnsureTrailingSlash(dir);
    if (filename.find('*') != std::string::npos) {
        WIN32_FIND_DATAA fd = {};
        HANDLE h = FindFirstFileA((search_dir + filename).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            return false;
        }
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                continue;
            }
            if (resolved_path) {
                *resolved_path = search_dir + fd.cFileName;
            }
            FindClose(h);
            return true;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        return false;
    }

    const std::string path = search_dir + filename;
    if (_access(path.c_str(), 0) == -1) {
        return false;
    }
    if (resolved_path) {
        *resolved_path = path;
    }
    return true;
}

bool IsCudaReferenceDependencyName(const std::string& filename) {
    const std::string lower = ToLowerAscii(filename);
    return lower == "cuda 12 runtime" ||
        lower == "cudart64_12.dll" ||
        lower == "cublas64_12.dll" ||
        lower == "cublaslt64_12.dll";
}

bool IsCudnnReferenceDependencyName(const std::string& filename) {
    const std::string lower = ToLowerAscii(filename);
    return lower == "cudnn 9 runtime" ||
        (lower.rfind("cudnn", 0) == 0 &&
        lower.size() >= 4 &&
        lower.substr(lower.size() - 4) == ".dll");
}

bool IsCudaReferenceGroupName(const std::string& filename) {
    return ToLowerAscii(filename) == "cuda 12 runtime";
}

bool IsCudnnReferenceGroupName(const std::string& filename) {
    return ToLowerAscii(filename) == "cudnn 9 runtime";
}

bool IsTensorRtRuntimeGroupName(const std::string& filename) {
    return ToLowerAscii(filename) == "tensorrt 10 runtime";
}

bool IsOnnxRuntimeTensorRtProviderGroupName(const std::string& filename) {
    return ToLowerAscii(filename) == "onnx runtime tensorrt providers";
}

bool FindOnnxRuntimeTensorRtProviderGroup(const std::string& dir, std::string* resolved_path) {
    if (!HasAllFilesInDirectory(dir, RequiredOnnxRuntimeTensorRtProviderFiles())) {
        return false;
    }
    if (resolved_path) {
        *resolved_path = EnsureTrailingSlash(dir) + JoinFileNames(RequiredOnnxRuntimeTensorRtProviderFiles());
    }
    return true;
}

bool HasRequiredTensorRtRuntimeFiles(const std::string& dir) {
    if (!HasAllFilesInDirectory(dir, RequiredTensorRtRuntimeFiles())) {
        return false;
    }
    for (const std::string& pattern : RequiredTensorRtRuntimePatterns()) {
        if (!FindDependencyInDirectory(dir, pattern, nullptr)) {
            return false;
        }
    }
    return true;
}

std::string FormatTensorRtVersion(int major, int minor, int patch, int build) {
    return std::to_string(major) + "." +
        std::to_string(minor) + "." +
        std::to_string(patch) + "." +
        std::to_string(build);
}

bool ProbeTensorRtVersion(const std::string& dir, int* major, int* minor, int* patch, int* build, std::string* issue) {
    const std::string path = EnsureTrailingSlash(dir) + "nvinfer_10.dll";
    HMODULE lib = LoadLibraryExA(
        path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!lib) {
        if (issue) {
            *issue = "无法加载 nvinfer_10.dll: " + FormatWin32Error(GetLastError());
        }
        return false;
    }

    using VersionFn = int (*)();
    auto get_major = reinterpret_cast<VersionFn>(GetProcAddress(lib, "getInferLibMajorVersion"));
    auto get_minor = reinterpret_cast<VersionFn>(GetProcAddress(lib, "getInferLibMinorVersion"));
    auto get_patch = reinterpret_cast<VersionFn>(GetProcAddress(lib, "getInferLibPatchVersion"));
    auto get_build = reinterpret_cast<VersionFn>(GetProcAddress(lib, "getInferLibBuildVersion"));
    if (!get_major || !get_minor || !get_patch || !get_build) {
        if (issue) {
            *issue = "nvinfer_10.dll 缺少 TensorRT 版本导出函数。";
        }
        FreeLibrary(lib);
        return false;
    }

    if (major) *major = get_major();
    if (minor) *minor = get_minor();
    if (patch) *patch = get_patch();
    if (build) *build = get_build();
    FreeLibrary(lib);
    return true;
}

bool ReadPeTimestamp(const std::string& path, DWORD* timestamp) {
    if (!timestamp) {
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    IMAGE_DOS_HEADER dos = {};
    file.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!file || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
        return false;
    }

    file.seekg(dos.e_lfanew, std::ios::beg);
    DWORD signature = 0;
    file.read(reinterpret_cast<char*>(&signature), sizeof(signature));
    if (!file || signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    IMAGE_FILE_HEADER header = {};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file) {
        return false;
    }

    *timestamp = header.TimeDateStamp;
    return true;
}

bool PeTimestampsClose(DWORD lhs, DWORD rhs) {
    const long long delta =
        lhs > rhs ? static_cast<long long>(lhs - rhs) : static_cast<long long>(rhs - lhs);
    constexpr long long seconds_per_day = 24LL * 60LL * 60LL;
    constexpr long long max_expected_delta = 90LL * seconds_per_day;
    return delta <= max_expected_delta;
}

bool TensorRtRuntimeFilesLookLikeSameSdk(const std::string& dir, std::string* issue) {
    const std::string base_dir = EnsureTrailingSlash(dir);
    DWORD nvinfer_timestamp = 0;
    if (!ReadPeTimestamp(base_dir + "nvinfer_10.dll", &nvinfer_timestamp)) {
        if (issue) {
            *issue = "无法读取 nvinfer_10.dll 的 PE 时间戳。";
        }
        return false;
    }

    std::vector<std::string> resource_files;
    CollectMatchingFiles(
        base_dir,
        {"nvinfer_builder_resource_10.dll", "nvinfer_builder_resource_*_10.dll"},
        resource_files);
    for (const std::string& resource_file : resource_files) {
        DWORD resource_timestamp = 0;
        if (!ReadPeTimestamp(resource_file, &resource_timestamp)) {
            continue;
        }
        if (!PeTimestampsClose(nvinfer_timestamp, resource_timestamp)) {
            if (issue) {
                *issue = "TensorRT SDK 混用了不同版本的 builder resource，请清空 TensorRT 目录后重新导入同一个 10.9 SDK。";
            }
            return false;
        }
    }

    return true;
}

bool HasSupportedTensorRtRuntimeFiles(const std::string& dir, std::string* issue = nullptr) {
    if (!HasRequiredTensorRtRuntimeFiles(dir)) {
        if (issue) {
            *issue = "TensorRT SDK 缺少 nvinfer、nvonnxparser 或 builder resource DLL。";
        }
        return false;
    }

    int major = 0;
    int minor = 0;
    int patch = 0;
    int build = 0;
    if (!ProbeTensorRtVersion(dir, &major, &minor, &patch, &build, issue)) {
        return false;
    }

    if (major != 10 || minor != 9) {
        if (issue) {
            *issue = "当前 TensorRT " + FormatTensorRtVersion(major, minor, patch, build) +
                " 不匹配，推荐 TensorRT 10.9.0.34 CUDA 12.8。";
        }
        return false;
    }

    if (!TensorRtRuntimeFilesLookLikeSameSdk(dir, issue)) {
        return false;
    }

    return true;
}

std::string ResolveCudnnDownloadUrlForCurrentCuda() {
    int cuda_major = 0;
    int cuda_minor = 0;
    if (ResolveConfiguredTensorRtCudaVersion(&cuda_major, &cuda_minor) &&
        cuda_major != kTensorRtSupportedCudaMajor) {
        return {};
    }

    return "https://developer.download.nvidia.com/compute/cudnn/redist/cudnn/windows-x86_64/"
        "cudnn-windows-x86_64-9.22.0.52_cuda12-archive.zip";
}

bool FindTensorRtRuntimeGroup(const std::string& dir, std::string* resolved_path) {
    if (!HasSupportedTensorRtRuntimeFiles(dir)) {
        return false;
    }

    if (resolved_path) {
        std::vector<std::string> display_files = RequiredTensorRtRuntimeFiles();
        display_files.insert(
            display_files.end(),
            RequiredTensorRtRuntimePatterns().begin(),
            RequiredTensorRtRuntimePatterns().end());
        *resolved_path = EnsureTrailingSlash(dir) + JoinFileNames(display_files);
    }
    return true;
}

bool FindCudaReferenceDependency(const std::string& filename, std::string* resolved_path) {
    const std::string dir = runtime_deps::TensorRtCudaDir();
    if (IsCudaReferenceGroupName(filename)) {
        if (!HasSupportedCuda12RuntimeFiles(dir)) {
            return false;
        }
        if (resolved_path) {
            *resolved_path = EnsureTrailingSlash(dir) + JoinFileNames(RequiredCuda12RuntimeFiles());
        }
        return true;
    }

    if (!HasSupportedCuda12RuntimeFiles(dir)) {
        return false;
    }
    return FindDependencyInDirectory(dir, filename, resolved_path);
}

bool FindCudnnReferenceDependency(const std::string& filename, std::string* resolved_path) {
    const std::string dir = runtime_deps::TensorRtCudnnDir();
    if (IsCudnnReferenceGroupName(filename)) {
        if (!HasSupportedCudnnRuntimeFiles(dir)) {
            return false;
        }
        if (resolved_path) {
            *resolved_path = EnsureTrailingSlash(dir) + JoinFileNames(RequiredCudnnRuntimeFiles());
        }
        return true;
    }

    if (!HasSupportedCudnnRuntimeFiles(dir)) {
        return false;
    }
    return FindDependencyInDirectory(dir, filename, resolved_path);
}

bool ResolveCuda12ReferenceDir(const std::string& src_dir, std::string* resolved_dir) {
    const std::string dir = EnsureTrailingSlash(src_dir);
    if (HasSupportedCuda12RuntimeFiles(dir)) {
        if (resolved_dir) {
            *resolved_dir = dir;
        }
        return true;
    }

    std::vector<std::string> matches;
    CollectMatchingFiles(dir, {"cudart64_12.dll"}, matches);
    for (const std::string& match : matches) {
        const std::string parent = ParentDirOfPath(match);
        if (HasSupportedCuda12RuntimeFiles(parent)) {
            if (resolved_dir) {
                *resolved_dir = parent;
            }
            return true;
        }
    }

    return false;
}

bool ResolveCudnnReferenceDir(const std::string& src_dir, std::string* resolved_dir) {
    const std::string dir = EnsureTrailingSlash(src_dir);
    if (HasSupportedCudnnRuntimeFiles(dir)) {
        if (resolved_dir) {
            *resolved_dir = dir;
        }
        return true;
    }

    std::vector<std::string> matches;
    CollectMatchingFiles(dir, {"cudnn64_9.dll"}, matches);
    for (const std::string& match : matches) {
        const std::string parent = ParentDirOfPath(match);
        if (HasSupportedCudnnRuntimeFiles(parent)) {
            if (resolved_dir) {
                *resolved_dir = parent;
            }
            return true;
        }
    }

    return false;
}

std::vector<CudaInstallCandidate> FindInstalledCuda12Bins(bool compatible_only = true) {
    std::vector<CudaInstallCandidate> candidates;
    const std::string root = "C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA\\";

    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA((root + "v*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return candidates;
    }

    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            continue;
        }

        int major = 0;
        int minor = 0;
        const std::string name = fd.cFileName;
        if (!ParseCudaVersionDir(name, &major, &minor) || major != 12) {
            continue;
        }
        if (compatible_only && !IsSupportedTensorRtCudaVersion(major, minor)) {
            continue;
        }

        const std::string bin_dir = root + name + "\\bin\\";
        if (HasRequiredCuda12RuntimeFiles(bin_dir)) {
            candidates.push_back({bin_dir, major, minor});
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.major != rhs.major) {
            return lhs.major > rhs.major;
        }
        return lhs.minor > rhs.minor;
    });
    return candidates;
}

std::string GetEnvironmentString(const char* name) {
    char buffer[MAX_PATH * 4] = {};
    const DWORD size = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof(buffer)));
    if (size == 0 || size >= sizeof(buffer)) {
        return {};
    }
    return buffer;
}

void AddUniqueDirectory(std::vector<std::string>* dirs, const std::string& dir) {
    if (!dirs) {
        return;
    }

    const std::string normalized = EnsureTrailingSlash(dir);
    if (normalized.empty()) {
        return;
    }

    const std::string lower = ToLowerAscii(normalized);
    for (const std::string& existing : *dirs) {
        if (ToLowerAscii(existing) == lower) {
            return;
        }
    }
    dirs->push_back(normalized);
}

std::vector<std::string> FindInstalledCudnnBins() {
    std::vector<std::string> probe_dirs;

    AddUniqueDirectory(&probe_dirs, runtime_deps::TensorRtCudaDir());
    for (const CudaInstallCandidate& candidate : FindInstalledCuda12Bins()) {
        AddUniqueDirectory(&probe_dirs, candidate.bin_dir);
    }

    for (const char* env_name : {"CUDNN_PATH", "CUDNN_HOME", "CUDA_PATH"}) {
        const std::string env_value = GetEnvironmentString(env_name);
        AddUniqueDirectory(&probe_dirs, env_value);
        AddUniqueDirectory(&probe_dirs, EnsureTrailingSlash(env_value) + "bin\\");
    }

    AddUniqueDirectory(&probe_dirs, "C:\\Program Files\\NVIDIA\\CUDNN\\");
    AddUniqueDirectory(&probe_dirs, "C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA\\");
    AddUniqueDirectory(&probe_dirs, "C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDNN\\");

    std::vector<std::string> result;
    for (const std::string& dir : probe_dirs) {
        std::string resolved_dir;
        if (ResolveCudnnReferenceDir(dir, &resolved_dir)) {
            AddUniqueDirectory(&result, resolved_dir);
        }
    }

    std::sort(result.begin(), result.end(), [](const std::string& lhs, const std::string& rhs) {
        return ToLowerAscii(lhs) < ToLowerAscii(rhs);
    });
    return result;
}

bool IsYoloModelName(const std::string& filename) {
    const std::string lower = ToLowerAscii(filename);
    if (lower.size() < 6 || lower.substr(lower.size() - 5) != ".onnx") {
        return false;
    }
    return lower.find("nn_model") == std::string::npos;
}

bool RequiresAppLocalFile(const std::string& filename) {
    const std::string lower = ToLowerAscii(filename);
    return lower == "onnxruntime.dll" ||
        lower == "onnxruntime_providers_shared.dll" ||
        lower == "onnxruntime_providers_tensorrt.dll" ||
        lower == "directml.dll" ||
        lower == "opencv_world4120.dll";
}

std::string FormatWin32Error(DWORD error) {
    char* message = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&message),
        0,
        nullptr);

    std::string result = "Win32 error " + std::to_string(error);
    if (size > 0 && message) {
        result += ": ";
        result += message;
        while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) {
            result.pop_back();
        }
    }
    if (message) {
        LocalFree(message);
    }
    return result;
}

struct OrtProviderProbe {
    bool ok = false;
    std::vector<std::string> providers;
    std::string error;
};

OrtProviderProbe ProbeOrtProviders(const std::string& ort_path) {
    OrtProviderProbe result;
    HMODULE lib = LoadLibraryExA(
        ort_path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!lib) {
        result.error = "LoadLibrary failed for onnxruntime.dll: " + FormatWin32Error(GetLastError());
        return result;
    }

    using OrtGetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)();
    auto get_api_base = reinterpret_cast<OrtGetApiBaseFn>(GetProcAddress(lib, "OrtGetApiBase"));
    if (!get_api_base) {
        result.error = "OrtGetApiBase not found in onnxruntime.dll";
        FreeLibrary(lib);
        return result;
    }

    const OrtApiBase* api_base = get_api_base();
    const OrtApi* api = api_base ? api_base->GetApi(ORT_API_VERSION) : nullptr;
    if (!api) {
        result.error = "onnxruntime.dll does not support the compiled ORT API version";
        FreeLibrary(lib);
        return result;
    }

    char** providers = nullptr;
    int provider_count = 0;
    OrtStatus* status = api->GetAvailableProviders(&providers, &provider_count);
    if (status) {
        const char* message = api->GetErrorMessage(status);
        result.error = message ? message : "GetAvailableProviders failed";
        api->ReleaseStatus(status);
        FreeLibrary(lib);
        return result;
    }

    for (int i = 0; i < provider_count; ++i) {
        if (providers[i]) {
            result.providers.emplace_back(providers[i]);
        }
    }
    api->ReleaseAvailableProviders(providers, provider_count);
    FreeLibrary(lib);
    result.ok = true;
    return result;
}

bool HasOrtProvider(const std::vector<std::string>& providers, const std::string& token) {
    const std::string lower_token = ToLowerAscii(token);
    for (const auto& provider : providers) {
        if (ToLowerAscii(provider).find(lower_token) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string JoinProviders(const std::vector<std::string>& providers) {
    if (providers.empty()) {
        return "(none)";
    }
    std::string text;
    for (const auto& provider : providers) {
        if (!text.empty()) {
            text += ", ";
        }
        text += provider;
    }
    return text;
}

int64_t QueryContentLength(HINTERNET request) {
    DWORD numeric_length = 0;
    DWORD numeric_size = sizeof(numeric_length);
    if (WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &numeric_length,
            &numeric_size,
            WINHTTP_NO_HEADER_INDEX)) {
        return static_cast<int64_t>(numeric_length);
    }

    wchar_t text_length[64] = {};
    DWORD text_size = sizeof(text_length);
    if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_CONTENT_LENGTH,
            WINHTTP_HEADER_NAME_BY_INDEX,
            text_length,
            &text_size,
            WINHTTP_NO_HEADER_INDEX)) {
        return 0;
    }

    wchar_t* end = nullptr;
    const unsigned long long parsed = std::wcstoull(text_length, &end, 10);
    if (end == text_length || parsed > static_cast<unsigned long long>(INT64_MAX)) {
        return 0;
    }
    return static_cast<int64_t>(parsed);
}

DWORD QueryHttpStatus(HINTERNET request) {
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code,
            &status_size,
            WINHTTP_NO_HEADER_INDEX)) {
        return status_code;
    }
    return 0;
}

std::string FormatBytes(int64_t bytes) {
    if (bytes < 0) {
        bytes = 0;
    }

    constexpr double kibi = 1024.0;
    const char* units[] = { "B", "KB", "MB", "GB" };
    double value = static_cast<double>(bytes);
    int unit_index = 0;
    while (value >= kibi && unit_index < 3) {
        value /= kibi;
        ++unit_index;
    }

    char buffer[64] = {};
    if (unit_index == 0) {
        snprintf(buffer, sizeof(buffer), "%lld %s", static_cast<long long>(bytes), units[unit_index]);
    } else {
        snprintf(buffer, sizeof(buffer), "%.1f %s", value, units[unit_index]);
    }
    return buffer;
}

float SafeProgressFraction(int64_t downloaded, int64_t total) {
    if (total <= 0) {
        return 0.0f;
    }
    double fraction = static_cast<double>(downloaded) / static_cast<double>(total);
    if (fraction < 0.0) {
        fraction = 0.0;
    } else if (fraction > 1.0) {
        fraction = 1.0;
    }
    return static_cast<float>(fraction);
}

ImVec4 UiBlue() {
    return ImVec4(0.05f, 0.42f, 0.95f, 1.0f);
}

ImVec4 UiBlueSoft() {
    return ImVec4(0.90f, 0.96f, 1.0f, 1.0f);
}

ImVec4 UiText() {
    return ImVec4(0.08f, 0.12f, 0.20f, 1.0f);
}

ImVec4 UiMuted() {
    return ImVec4(0.43f, 0.50f, 0.62f, 1.0f);
}

ImVec4 UiSuccess() {
    return ImVec4(0.00f, 0.62f, 0.42f, 1.0f);
}

ImVec4 UiWarning() {
    return ImVec4(0.92f, 0.55f, 0.08f, 1.0f);
}

ImVec4 UiDanger() {
    return ImVec4(0.86f, 0.20f, 0.22f, 1.0f);
}

void ApplyBlueWhiteTechStyle() {
    ImGui::StyleColorsLight();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.ChildRounding = 10.0f;
    style.FrameRounding = 7.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 7.0f;
    style.WindowPadding = ImVec2(16.0f, 12.0f);
    style.FramePadding = ImVec2(10.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 5.0f);
    style.ScrollbarSize = 11.0f;
    style.Colors[ImGuiCol_Text] = UiText();
    style.Colors[ImGuiCol_TextDisabled] = UiMuted();
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.96f, 0.98f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.92f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.98f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.74f, 0.84f, 0.96f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.91f, 0.96f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.82f, 0.91f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.72f, 0.86f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.34f, 0.80f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.05f, 0.42f, 0.95f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.10f, 0.43f, 0.88f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.05f, 0.53f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.02f, 0.34f, 0.80f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.84f, 0.93f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.74f, 0.88f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.62f, 0.80f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = UiBlue();
    style.Colors[ImGuiCol_SliderGrab] = UiBlue();
    style.Colors[ImGuiCol_Separator] = ImVec4(0.76f, 0.86f, 0.96f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.91f, 0.96f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.62f, 0.76f, 0.94f, 1.0f);
}

void SectionTitle(const char* text) {
    ImGui::TextColored(UiBlue(), "%s", text);
    ImGui::Separator();
}

void MetricCard(const char* label, const char* value, const ImVec4& color) {
    ImGui::BeginChild(
        label,
        ImVec2(0, 50),
        ImGuiChildFlags_Borders,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetCursorPosY((50.0f - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextColored(UiMuted(), "%s", label);
    const float value_width = ImGui::CalcTextSize(value).x;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - value_width + ImGui::GetCursorPosX());
    ImGui::TextColored(color, "%s", value);
    ImGui::EndChild();
}

void TextEllipsisLine(const char* label, const std::string& value) {
    ImGui::TextColored(UiMuted(), "%s", label);
    ImGui::SameLine();
    const char* text = value.empty() ? "(未设置)" : value.c_str();
    ImGui::TextUnformatted(text);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", text);
    }
}

bool ActionButton(const char* label, float min_width = 78.0f) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    float width = min_width;
    const float label_width = ImGui::CalcTextSize(label).x + 30.0f;
    if (width < label_width) {
        width = label_width;
    }
    const bool clicked = ImGui::Button(label, ImVec2(width, 28.0f));
    ImGui::PopStyleVar();
    return clicked;
}

const char* BackendDisplayName(int backend) {
    switch (backend) {
    case 0:
        return "DirectML";
    case 1:
        return "TensorRT";
    case 2:
        return "CPU";
    default:
        return "Unknown";
    }
}

bool LaunchMainProcess() {
    char exe_path[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, exe_path, MAX_PATH)) {
        return false;
    }

    const std::string work_dir = ParentDirOfPath(exe_path);
    HINSTANCE result = ShellExecuteA(
        nullptr,
        "open",
        exe_path,
        nullptr,
        work_dir.empty() ? nullptr : work_dir.c_str(),
        SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}
}

bool DependencyInstaller::OpenManagerProcess() {
    HWND existing = FindWindowW(L"LozeeAimDep", nullptr);
    if (existing) {
        ShowWindow(existing, SW_SHOWNORMAL);
        SetForegroundWindow(existing);
        return true;
    }

    char exe_path[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, exe_path, MAX_PATH)) {
        return false;
    }

    HINSTANCE result = ShellExecuteA(
        nullptr,
        "open",
        exe_path,
        "--deps",
        nullptr,
        SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

LRESULT CALLBACK DependencyInstaller::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_installer && g_installer->d3d_device && wParam != SIZE_MINIMIZED) {
            if (g_installer->rtv) { g_installer->rtv->Release(); g_installer->rtv = nullptr; }
            g_installer->swap_chain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            ID3D11Texture2D* back_buffer = nullptr;
            g_installer->swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
            if (back_buffer) {
                g_installer->d3d_device->CreateRenderTargetView(back_buffer, nullptr, &g_installer->rtv);
                back_buffer->Release();
            }
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
DependencyInstaller::DependencyInstaller() {
    model_paths::EnsureModelDirs();
    runtime_deps::EnsureDependencyDirs();

    all_files = {
        // ---- ONNX Runtime — must match backend; DML / GPU are different builds ----
        {"onnxruntime.dll",
         "ONNX Runtime 核心运行时。TensorRT 模式会从同一个 GPU 包顺带提取 provider DLL。",
         "",
         "onnxruntime.dll;onnxruntime_providers_shared.dll;onnxruntime_providers_tensorrt.dll;onnxruntime_providers_cuda.dll",
         "https://github.com/microsoft/onnxruntime/releases/tag/v1.23.2",
         true, true, true},

        {"ONNX Runtime TensorRT Providers",
         "ONNX Runtime TensorRT provider 组；CUDA provider 是 TensorRT EP 的同包依赖，必须一起保留。",
         "",
         "onnxruntime_providers_shared.dll;onnxruntime_providers_tensorrt.dll;onnxruntime_providers_cuda.dll",
         "https://github.com/microsoft/onnxruntime/releases/tag/v1.23.2",
         false, true, false},

        {"DirectML.dll",
         "DirectML 后端运行时，DirectML 模式必须存在。",
         "https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/1.15.4",
         "DirectML.dll",
         "https://www.nuget.org/packages/Microsoft.AI.DirectML",
         true, false, false},

        // ---- OpenCV ----
        {"opencv_world4120.dll",
         "OpenCV 4.12.0 图像处理运行库，用于截图帧预处理。",
         "https://github.com/opencv/opencv/releases/download/4.12.0/opencv-4.12.0-windows.exe",
         "opencv_world4120.dll",
         "https://github.com/opencv/opencv/releases/tag/4.12.0",
         true, true, true},

        // ---- YOLO model (wildcard: YOLO .onnx in models/yolo, excluding NN models) ----
        {"*.onnx",
         "YOLO 模型资源 — 请至少放置一个非 NN 的 .onnx 到 models\\yolo",
         "",
         "",
         "",
         true, true, true},

        // ---- TensorRT SDK ----
        {"CUDA 12 Runtime",
         "CUDA 12 运行时目录引用，需要 CUDA 12.0-12.8 bin 目录中的 cudart、cublas、cublasLt 与 cuFFT。",
         "https://developer.download.nvidia.com/compute/cuda/12.8.1/local_installers/cuda_12.8.1_572.61_windows.exe",
         "",
         "https://developer.nvidia.com/cuda-toolkit-archive",
         false, true, false},

        {"TensorRT 10 Runtime",
         "TensorRT 10.9 SDK 运行库，与 ONNX Runtime TensorRT EP 的 CUDA 12.8 组合匹配。",
         "https://developer.nvidia.com/downloads/compute/machine-learning/tensorrt/10.9.0/zip/TensorRT-10.9.0.34.Windows.win10.cuda-12.8.zip",
         "nvinfer_10.dll;nvinfer_dispatch_10.dll;nvinfer_lean_10.dll;nvinfer_plugin_10.dll;nvinfer_vc_plugin_10.dll;nvonnxparser_10.dll;nvinfer_builder_resource_10.dll",
         "https://developer.nvidia.com/tensorrt/download/10x",
         false, true, false},

        // ---- cuDNN ----
        {"cuDNN 9 Runtime",
         "NVIDIA cuDNN 9 运行时目录引用，需要与当前 CUDA 12.x 版本同步；cuda12 通用包可用于 CUDA 12.0-12.8。",
         "",
         "cudnn*_9.dll",
         "https://developer.nvidia.com/cudnn",
         false, true, false},

        // ---- Known optional / unused files ----
        {"trtexec.exe",
          "可选 TensorRT 命令行工具，用于 benchmark 或预构建 engine；LozeeAim 不会调用。",
         "",
         "",
         "https://docs.nvidia.com/deeplearning/tensorrt/latest/reference/command-line-programs.html",
         false, false, false},

        {"nvblas64_12.dll",
          "NVBLAS 拦截库；LozeeAim 不需要。",
         "",
         "",
         "https://developer.nvidia.com/cuda-toolkit-archive",
         false, false, false},

        {"nvinfer_dispatch_10.dll",
          "TensorRT dispatch 运行库；当前 ONNX Runtime TensorRT 路径不需要。",
         "",
         "",
         "https://developer.nvidia.com/tensorrt/download/10x",
         false, false, false},

        {"nvinfer_vc_plugin_10.dll",
          "TensorRT VC plugin；当前 ONNX Runtime TensorRT 路径不需要。",
         "",
         "",
         "https://developer.nvidia.com/tensorrt/download/10x",
         false, false, false},
    };

    file_present.resize(all_files.size(), false);
    file_runtime_issue.resize(all_files.size(), false);
    file_location.resize(all_files.size());
    LoadSelectedBackendFromConfig();
    CheckFiles();
}

DependencyInstaller::~DependencyInstaller() {
    if (download_thread.joinable()) download_thread.join();
}

// ---------------------------------------------------------------------------
bool DependencyInstaller::InitWindow() {
    wc = {
        sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandle(nullptr), nullptr, LoadCursor(nullptr, IDC_ARROW),
        nullptr, nullptr, L"LozeeAimDep", nullptr
    };
    RegisterClassExW(&wc);

    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int x = (screen_w - window_w) / 2;
    int y = (screen_h - window_h) / 2;

    hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"LozeeAim - 依赖管理器",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        x, y, window_w, window_h,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (!hwnd) return false;

    // D3D11
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL feature_level;
    const D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION, &sd,
        &swap_chain, &d3d_device, &feature_level, &d3d_context
    );
    if (FAILED(hr)) return false;

    ID3D11Texture2D* back_buffer = nullptr;
    swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (back_buffer) {
        d3d_device->CreateRenderTargetView(back_buffer, nullptr, &rtv);
        back_buffer->Release();
    }

    // ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    // Font
    if (_access("font.ttc", 0) != -1) {
        io.Fonts->AddFontFromFileTTF("font.ttc", 16.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    } else {
        char win_dir[MAX_PATH];
        GetWindowsDirectoryA(win_dir, MAX_PATH);
        std::string font_path = std::string(win_dir) + "\\Fonts\\msyh.ttc";
        io.Fonts->AddFontFromFileTTF(font_path.c_str(), 16.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    }

    ApplyBlueWhiteTechStyle();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(d3d_device, d3d_context);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    d3d_ok = true;
    return true;
}

void DependencyInstaller::CleanupD3D() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (rtv) { rtv->Release(); rtv = nullptr; }
    if (swap_chain) { swap_chain->Release(); swap_chain = nullptr; }
    if (d3d_context) { d3d_context->Release(); d3d_context = nullptr; }
    if (d3d_device) { d3d_device->Release(); d3d_device = nullptr; }
    if (hwnd) { DestroyWindow(hwnd); hwnd = nullptr; }
    if (wc.lpszClassName) UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

// ---------------------------------------------------------------------------
std::string DependencyInstaller::GetExeDir() const {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    return s.substr(0, s.rfind('\\') + 1);
}

bool DependencyInstaller::FileExists(const std::string& filename) const {
    std::string full = GetExeDir() + filename;
    return _access(full.c_str(), 0) != -1;
}

std::string DependencyInstaller::GetInstallDirForFile(const DepFile& file) const {
    return runtime_deps::InstallDirForDependency(file.filename, selected_backend);
}

std::vector<std::string> DependencyInstaller::ListLocalYoloModels() const {
    model_paths::EnsureModelDirs();
    std::vector<std::string> models;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((model_paths::YoloDirA() + "*.onnx").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return models;
    }

    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const std::string filename = fd.cFileName;
        if (IsYoloModelName(filename)) {
            models.push_back(filename);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    std::sort(models.begin(), models.end(), [](const std::string& a, const std::string& b) {
        return ToLowerAscii(a) < ToLowerAscii(b);
    });
    return models;
}

bool DependencyInstaller::FindDependencyFile(const DepFile& file, std::string* resolved_path) const {
    const std::string& filename = file.filename;
    if (filename == "*.onnx") {
        const auto models = ListLocalYoloModels();
        if (models.empty()) {
            return false;
        }
        if (resolved_path) {
            *resolved_path = model_paths::YoloDirA() + models.front();
        }
        return true;
    }

    if (selected_backend == runtime_deps::BackendTensorRT) {
        if (ToLowerAscii(filename) == "onnxruntime.dll") {
            const std::string path = runtime_deps::DependencyPath(filename, runtime_deps::BackendTensorRT);
            if (_access(path.c_str(), 0) == -1) {
                return false;
            }
            if (resolved_path) {
                *resolved_path = path;
            }
            return true;
        }
        if (IsOnnxRuntimeTensorRtProviderGroupName(filename)) {
            return FindOnnxRuntimeTensorRtProviderGroup(runtime_deps::BackendDir(runtime_deps::BackendTensorRT), resolved_path);
        }
        if (IsTensorRtRuntimeGroupName(filename)) {
            return FindTensorRtRuntimeGroup(runtime_deps::BackendDir(runtime_deps::BackendTensorRT), resolved_path);
        }
        if (IsCudaReferenceDependencyName(filename) &&
            FindCudaReferenceDependency(filename, resolved_path)) {
            return true;
        }
        if (IsCudnnReferenceDependencyName(filename) &&
            FindCudnnReferenceDependency(filename, resolved_path)) {
            return true;
        }
    }

    if (filename.find('*') != std::string::npos) {
        std::string pattern = GetInstallDirForFile(file) + filename;
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                continue;
            }
            if (resolved_path) *resolved_path = GetInstallDirForFile(file) + fd.cFileName;
            FindClose(h);
            return true;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        return false;
    }

    std::string local_path = runtime_deps::DependencyPath(filename, selected_backend);
    if (_access(local_path.c_str(), 0) != -1) {
        if (resolved_path) *resolved_path = local_path;
        return true;
    }

    if (RequiresAppLocalFile(filename) || IsNeededForSelectedBackend(file)) {
        return false;
    }

    char found[MAX_PATH * 2] = {};
    DWORD n = SearchPathA(nullptr, filename.c_str(), nullptr, static_cast<DWORD>(sizeof(found)), found, nullptr);
    if (n > 0 && n < sizeof(found)) {
        if (resolved_path) *resolved_path = found;
        return true;
    }

    return false;
}

bool DependencyInstaller::IsNeededForSelectedBackend(const DepFile& file) const {
    switch (selected_backend) {
    case 0: return file.required_dml;
    case 1: return file.required_trt;
    case 2: return file.required_cpu;
    default: return false;
    }
}

std::string DependencyInstaller::ResolveDownloadUrl(const DepFile& file) const {
    if (!file.download_url.empty()) return file.download_url;
    if (IsOnnxRuntimeTensorRtProviderGroupName(file.filename)) {
        return "https://github.com/microsoft/onnxruntime/releases/download/v1.23.2/onnxruntime-win-x64-gpu-1.23.2.zip";
    }
    if (IsCudnnReferenceGroupName(file.filename)) {
        return ResolveCudnnDownloadUrlForCurrentCuda();
    }
    if (file.filename == "onnxruntime.dll") {
        if (selected_backend == 1) {
            return "https://github.com/microsoft/onnxruntime/releases/download/v1.23.2/onnxruntime-win-x64-gpu-1.23.2.zip";
        }
        if (selected_backend == 0) {
            return "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.24.3";
        }
        return "https://github.com/microsoft/onnxruntime/releases/download/v1.23.2/onnxruntime-win-x64-1.23.2.zip";
    }
    return {};
}

int DependencyInstaller::CountRequiredFiles() const {
    int count = 0;
    for (const auto& file : all_files) {
        if (IsNeededForSelectedBackend(file)) ++count;
    }
    return count;
}

int DependencyInstaller::CountMissingRequiredFiles() const {
    int count = 0;
    for (size_t i = 0; i < all_files.size(); ++i) {
        if (IsNeededForSelectedBackend(all_files[i]) && !file_present[i]) ++count;
    }
    return count;
}

int DependencyInstaller::CountRuntimeIssueRequiredFiles() const {
    int count = 0;
    for (size_t i = 0; i < all_files.size(); ++i) {
        const bool runtime_issue = i < file_runtime_issue.size() && file_runtime_issue[i];
        if (IsNeededForSelectedBackend(all_files[i]) && file_present[i] && runtime_issue) ++count;
    }
    return count;
}

int DependencyInstaller::CountPresentUnusedKnownFiles() const {
    int count = 0;
    for (size_t i = 0; i < all_files.size(); ++i) {
        if (!IsNeededForSelectedBackend(all_files[i]) && file_present[i]) ++count;
    }
    return count;
}

std::vector<std::string> DependencyInstaller::ListRootLegacyDependencies() const {
    return runtime_deps::ListRootLegacyDependencies();
}

void DependencyInstaller::ValidateBackendRuntime() {
    int ort_index = -1;
    int directml_index = -1;
    int tensorrt_provider_index = -1;
    int tensorrt_runtime_index = -1;
    int cuda_runtime_index = -1;
    int cudnn_runtime_index = -1;

    for (size_t i = 0; i < all_files.size(); ++i) {
        const std::string name = ToLowerAscii(all_files[i].filename);
        if (name == "onnxruntime.dll") {
            ort_index = static_cast<int>(i);
        } else if (name == "directml.dll") {
            directml_index = static_cast<int>(i);
        } else if (IsOnnxRuntimeTensorRtProviderGroupName(all_files[i].filename)) {
            tensorrt_provider_index = static_cast<int>(i);
        } else if (IsTensorRtRuntimeGroupName(all_files[i].filename)) {
            tensorrt_runtime_index = static_cast<int>(i);
        } else if (IsCudaReferenceGroupName(all_files[i].filename)) {
            cuda_runtime_index = static_cast<int>(i);
        } else if (IsCudnnReferenceGroupName(all_files[i].filename)) {
            cudnn_runtime_index = static_cast<int>(i);
        }
    }

    if (ort_index < 0 || !file_present[ort_index]) {
        return;
    }

    std::string runtime_error;
    if (!runtime_deps::Configure(
            selected_backend,
            selected_backend == runtime_deps::BackendCPU,
            &runtime_error)) {
        AddLog("ERR: " + runtime_error);
    }

    const std::string ort_path = runtime_deps::DependencyPath("onnxruntime.dll", selected_backend);
    const OrtProviderProbe probe = ProbeOrtProviders(ort_path);
    if (!probe.ok) {
        file_runtime_issue[ort_index] = true;
        file_location[ort_index] = probe.error;
        AddLog("ERR: " + probe.error);
        return;
    }

    AddLog("ONNX Runtime providers: " + JoinProviders(probe.providers));

    if (selected_backend == 0) {
        if (directml_index >= 0 && !file_present[directml_index]) {
            file_location[directml_index] = "DirectML 模式需要把 DirectML.dll 放在 deps\\directml。";
        }

        if (!HasOrtProvider(probe.providers, "dml")) {
            file_runtime_issue[ort_index] = true;
            file_location[ort_index] =
                "Wrong ONNX Runtime package: DirectML mode requires DmlExecutionProvider. "
                "Download the ONNX Runtime DirectML build.";
            AddLog("ERR: ONNX Runtime build does not contain DmlExecutionProvider");
        }
    } else if (selected_backend == 1) {
        if (tensorrt_provider_index >= 0 && !file_present[tensorrt_provider_index]) {
            file_location[tensorrt_provider_index] =
                "TensorRT 模式需要同包的 onnxruntime_providers_shared.dll 与 onnxruntime_providers_tensorrt.dll。";
        }
        if (cuda_runtime_index >= 0 && !file_present[cuda_runtime_index]) {
            std::string issue;
            const std::string cuda_dir = runtime_deps::TensorRtCudaDir();
            if (!cuda_dir.empty() && HasRequiredCuda12RuntimeFiles(cuda_dir) &&
                !HasSupportedCuda12RuntimeFiles(cuda_dir, &issue)) {
                file_present[cuda_runtime_index] = true;
                file_runtime_issue[cuda_runtime_index] = true;
                file_location[cuda_runtime_index] =
                    issue.empty() ? "当前 CUDA 版本不兼容，请使用 CUDA v12.0-v12.8。" : issue;
            } else {
                HasSupportedCuda12RuntimeFiles(cuda_dir, &issue);
                file_location[cuda_runtime_index] =
                    issue.empty() ? "TensorRT 模式推荐 CUDA v12.0-v12.8。" : issue;
            }
        }
        if (tensorrt_runtime_index >= 0 && !file_present[tensorrt_runtime_index]) {
            std::string issue;
            const std::string tensorrt_dir = runtime_deps::BackendDir(runtime_deps::BackendTensorRT);
            if (HasRequiredTensorRtRuntimeFiles(tensorrt_dir) &&
                !HasSupportedTensorRtRuntimeFiles(tensorrt_dir, &issue)) {
                file_present[tensorrt_runtime_index] = true;
                file_runtime_issue[tensorrt_runtime_index] = true;
                file_location[tensorrt_runtime_index] =
                    issue.empty() ? "当前 TensorRT SDK 版本不兼容，请使用 TensorRT 10.9。" : issue;
            } else {
                HasSupportedTensorRtRuntimeFiles(tensorrt_dir, &issue);
                file_location[tensorrt_runtime_index] =
                    issue.empty() ? "TensorRT 模式推荐 TensorRT 10.9.0.34 Windows CUDA 12.8 ZIP。" : issue;
            }
        }
        if (cudnn_runtime_index >= 0 && !file_present[cudnn_runtime_index]) {
            std::string issue;
            const std::string cudnn_dir = runtime_deps::TensorRtCudnnDir();
            if (!cudnn_dir.empty() && HasRequiredCudnnRuntimeFiles(cudnn_dir) &&
                !HasSupportedCudnnRuntimeFiles(cudnn_dir, &issue)) {
                file_present[cudnn_runtime_index] = true;
                file_runtime_issue[cudnn_runtime_index] = true;
                file_location[cudnn_runtime_index] =
                    issue.empty() ? "当前 cuDNN 目录不兼容，请使用 CUDA v12.0-v12.8 对应的 cuDNN 9。" : issue;
            } else {
                HasSupportedCudnnRuntimeFiles(cudnn_dir, &issue);
                file_location[cudnn_runtime_index] =
                    issue.empty() ? "TensorRT 模式需要与当前 CUDA 版本同步的 cuDNN 9。" : issue;
            }
        }
        const bool support_runtime_ready =
            (tensorrt_provider_index < 0 || file_present[tensorrt_provider_index]) &&
            (tensorrt_runtime_index < 0 || file_present[tensorrt_runtime_index]) &&
            (cuda_runtime_index < 0 || file_present[cuda_runtime_index]) &&
            (cudnn_runtime_index < 0 || file_present[cudnn_runtime_index]);
        const bool support_runtime_compatible =
            (tensorrt_runtime_index < 0 || !file_runtime_issue[tensorrt_runtime_index]) &&
            (cuda_runtime_index < 0 || !file_runtime_issue[cuda_runtime_index]) &&
            (cudnn_runtime_index < 0 || !file_runtime_issue[cudnn_runtime_index]);
        if (!support_runtime_ready) {
            return;
        }
        if (!support_runtime_compatible) {
            return;
        }
        if (!HasOrtProvider(probe.providers, "tensorrt")) {
            file_runtime_issue[ort_index] = true;
            file_location[ort_index] =
                "Wrong ONNX Runtime package: TensorRT mode requires TensorrtExecutionProvider. "
                "Download the ONNX Runtime GPU build.";
            AddLog("ERR: ONNX Runtime build does not contain TensorrtExecutionProvider");
            return;
        }
        std::string preload_error;
        if (!runtime_deps::PreloadTensorRtProviderDependencies(&preload_error)) {
            const int error_index = tensorrt_provider_index >= 0 ? tensorrt_provider_index : ort_index;
            file_runtime_issue[error_index] = true;
            file_location[error_index] = "TensorRT provider load failed: " + preload_error +
                "。推荐组合：" + std::string(kTensorRtRecommendedStack) + "。";
            AddLog("ERR: TensorRT provider load failed: " + preload_error);
        }
    }
}

void DependencyInstaller::LoadSelectedBackendFromConfig() {
    std::ifstream f(GetExeDir() + "global.ini");
    if (!f) return;

    int provider = selected_backend;
    bool use_cpu = false;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        try {
            if (key == "inference_provider") {
                provider = std::stoi(value);
            } else if (key == "use_cpu_inference") {
                use_cpu = (value == "1");
            }
        } catch (...) {
            continue;
        }
    }

    if (provider < 0 || provider > 2) provider = 0;
    selected_backend = use_cpu ? 2 : provider;
}

void DependencyInstaller::SaveSelectedBackendToConfig() {
    const std::string config_path = GetExeDir() + "global.ini";
    std::vector<std::string> lines;
    {
        std::ifstream input(config_path);
        std::string line;
        while (std::getline(input, line)) {
            lines.push_back(line);
        }
    }

    const int provider = selected_backend == runtime_deps::BackendCPU
        ? runtime_deps::BackendDirectML
        : selected_backend;
    const std::string provider_value = std::to_string(provider);
    const std::string cpu_value = selected_backend == runtime_deps::BackendCPU ? "1" : "0";
    bool wrote_provider = false;
    bool wrote_cpu = false;

    for (std::string& line : lines) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        const std::string key = line.substr(0, eq);
        if (key == "inference_provider") {
            line = "inference_provider=" + provider_value;
            wrote_provider = true;
        } else if (key == "use_cpu_inference") {
            line = "use_cpu_inference=" + cpu_value;
            wrote_cpu = true;
        }
    }

    if (!wrote_provider) {
        lines.push_back("inference_provider=" + provider_value);
    }
    if (!wrote_cpu) {
        lines.push_back("use_cpu_inference=" + cpu_value);
    }

    std::ofstream output(config_path, std::ios::out | std::ios::trunc);
    if (!output) {
        AddLog("ERR: 无法写入 global.ini");
        return;
    }

    for (const std::string& line : lines) {
        output << line << "\n";
    }
    AddLog(std::string("已保存推理后端: ") + BackendDisplayName(selected_backend));
}

void DependencyInstaller::CheckFiles() {
    EnsureAutoReferenceDirs();
    if (file_runtime_issue.size() != all_files.size()) {
        file_runtime_issue.resize(all_files.size(), false);
    }
    for (size_t i = 0; i < all_files.size(); i++) {
        std::string resolved;
        file_runtime_issue[i] = false;
        file_present[i] = FindDependencyFile(all_files[i], &resolved);
        file_location[i] = file_present[i] ? resolved : "";
    }

    ValidateBackendRuntime();
    all_ready = (CountMissingRequiredFiles() == 0 && CountRuntimeIssueRequiredFiles() == 0);
}

bool DependencyInstaller::CopyLocalFile(const std::string& src_path, const std::string& filename) {
    runtime_deps::EnsureDependencyDirs();
    std::string dst = runtime_deps::InstallDirForDependency(filename, selected_backend) + filename;
    if (CopyFileA(src_path.c_str(), dst.c_str(), FALSE)) {
        AddLog("Copied: " + filename);
        return true;
    }
    AddLog("Copy failed: " + filename);
    return false;
}

bool DependencyInstaller::CopyLocalFileToRunDir(const std::string& src_path) {
    model_paths::EnsureModelDirs();
    const std::string filename = BaseNameOfPath(src_path);
    if (filename.empty()) {
        AddLog("Copy failed: empty filename");
        return false;
    }

    const std::string dst = model_paths::YoloDirA() + filename;
    if (_stricmp(src_path.c_str(), dst.c_str()) == 0) {
        AddLog("Already in models/yolo: " + filename);
        return true;
    }

    if (CopyFileA(src_path.c_str(), dst.c_str(), FALSE)) {
        AddLog("Copied to models/yolo: " + filename);
        return true;
    }

    AddLog("Copy failed: " + filename);
    return false;
}

void DependencyInstaller::BrowseAndCopy(const std::string& filename) {
    OPENFILENAMEA ofn = {};
    char file_buf[MAX_PATH * 2] = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "DLL 文件\0*.dll\0ONNX 模型\0*.onnx\0所有文件\0*.*\0";
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = sizeof(file_buf);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

    if (GetOpenFileNameA(&ofn)) {
        if (CopyLocalFile(file_buf, filename)) {
            CheckFiles();
        }
    }
}

void DependencyInstaller::BrowseAndCopyYoloModel() {
    OPENFILENAMEA ofn = {};
    char file_buf[MAX_PATH * 2] = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "ONNX 模型\0*.onnx\0所有文件\0*.*\0";
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = sizeof(file_buf);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

    if (GetOpenFileNameA(&ofn)) {
        const std::string filename = BaseNameOfPath(file_buf);
        if (!IsYoloModelName(filename)) {
            AddLog("WARN: selected file looks like an NN model: " + filename);
        }
        if (CopyLocalFileToRunDir(file_buf)) {
            CheckFiles();
        }
    }
}

bool DependencyInstaller::ImportFilesFromDirectory(
    const std::string& src_dir,
    const std::vector<std::string>& patterns,
    const std::string& dest_dir) {
    if (src_dir.empty()) {
        return false;
    }

    CreateDirectoryA(dest_dir.c_str(), nullptr);
    std::vector<std::string> matched_files;
    CollectMatchingFiles(src_dir, patterns, matched_files);
    if (matched_files.empty()) {
        AddLog("No matching files found in: " + src_dir);
        return false;
    }

    int copied = 0;
    for (const auto& src : matched_files) {
        const std::string filename = BaseNameOfPath(src);
        const std::string dst = dest_dir + filename;
        DeleteFileA(dst.c_str());
        if (CopyFileA(src.c_str(), dst.c_str(), FALSE)) {
            ++copied;
            AddLog("Copied: " + filename);
        }
    }

    CheckFiles();
    AddLog("Import complete: " + std::to_string(copied) + " file(s)");
    return copied > 0;
}

void DependencyInstaller::EnsureAutoReferenceDirs() {
    if (selected_backend != runtime_deps::BackendTensorRT) {
        return;
    }

    std::string cuda_issue;
    const std::string configured_cuda_dir = runtime_deps::TensorRtCudaDir();
    if (configured_cuda_dir.empty() || !HasSupportedCuda12RuntimeFiles(configured_cuda_dir, &cuda_issue)) {
        if (!configured_cuda_dir.empty()) {
            AddLog("CUDA directory rejected: " + cuda_issue);
        }
        const auto candidates = FindInstalledCuda12Bins();
        if (!candidates.empty()) {
            runtime_deps::SetTensorRtCudaDir(candidates.front().bin_dir);
            AddLog("Using compatible CUDA directory: " + candidates.front().bin_dir);
        } else if (!configured_cuda_dir.empty()) {
            const auto installed_cuda = FindInstalledCuda12Bins(false);
            if (!installed_cuda.empty()) {
                const auto newest = installed_cuda.front();
                AddLog("Found CUDA v" + std::to_string(newest.major) + "." +
                    std::to_string(newest.minor) + ", but TensorRT EP requires CUDA v12.0-v12.8.");
            }
        }
    }

    std::string cudnn_issue;
    const std::string configured_cudnn_dir = runtime_deps::TensorRtCudnnDir();
    if (configured_cudnn_dir.empty() || !HasSupportedCudnnRuntimeFiles(configured_cudnn_dir, &cudnn_issue)) {
        if (!configured_cudnn_dir.empty()) {
            AddLog("cuDNN directory rejected: " + cudnn_issue);
        }
        const std::string cuda_dir = runtime_deps::TensorRtCudaDir();
        if (HasSupportedCudnnRuntimeFiles(cuda_dir)) {
            runtime_deps::SetTensorRtCudnnDir(cuda_dir);
            AddLog("Using cuDNN directory: " + cuda_dir);
        } else {
            const auto candidates = FindInstalledCudnnBins();
            if (!candidates.empty()) {
                runtime_deps::SetTensorRtCudnnDir(candidates.front());
                AddLog("Using cuDNN directory: " + candidates.front());
            } else if (!configured_cudnn_dir.empty()) {
                AddLog("No cuDNN 9 directory matching the selected CUDA version was found.");
            }
        }
    }
}

void DependencyInstaller::BrowseAndImportTensorRtSdk() {
    const std::string dir = BrowseFolder(hwnd, "选择 TensorRT SDK 根目录或 lib 目录");
    if (dir.empty()) {
        return;
    }
    const std::string dest_dir = runtime_deps::BackendDir(runtime_deps::BackendTensorRT);
    if (!SameDirectoryPath(dir, dest_dir)) {
        const int deleted_count = CleanTensorRtRuntimeFiles(dest_dir);
        if (deleted_count > 0) {
            AddLog("Cleaned old TensorRT SDK files before import: " + std::to_string(deleted_count));
        }
    }
    ImportFilesFromDirectory(
        dir,
        {
            "nvinfer_10.dll",
            "nvinfer_dispatch_10.dll",
            "nvinfer_lean_10.dll",
            "nvinfer_plugin_10.dll",
            "nvinfer_vc_plugin_10.dll",
            "nvonnxparser_10.dll",
            "nvinfer_builder_resource_10.dll",
        },
        dest_dir);
}

void DependencyInstaller::BrowseAndImportCudaBin() {
    if (TryAutoImportInstalledCuda()) {
        return;
    }

    AddLog("Please select a CUDA 12.0-12.8 Toolkit directory or bin directory.");
    const std::string dir = BrowseFolder(hwnd, "选择 CUDA 12.0-12.8 安装目录或 bin 目录");
    if (dir.empty()) {
        return;
    }

    std::string resolved_dir;
    if (!ResolveCuda12ReferenceDir(dir, &resolved_dir)) {
        std::string issue;
        HasSupportedCuda12RuntimeFiles(EnsureTrailingSlash(dir), &issue);
        AddLog(issue.empty() ? ("No compatible CUDA 12 runtime DLLs found in: " + dir) : issue);
        return;
    }

    runtime_deps::SetTensorRtCudaDir(resolved_dir);
    AddLog("Using compatible CUDA directory: " + resolved_dir);
    runtime_deps::SetTensorRtCudnnDir("");
    AddLog("cuDNN directory reset; it must match the selected CUDA version.");
    CheckFiles();
}

bool DependencyInstaller::TryAutoImportInstalledCuda() {
    const auto candidates = FindInstalledCuda12Bins();
    if (candidates.empty()) {
        const auto installed_cuda = FindInstalledCuda12Bins(false);
        if (!installed_cuda.empty()) {
            const auto newest = installed_cuda.front();
            AddLog("Found CUDA v" + std::to_string(newest.major) + "." +
                std::to_string(newest.minor) + ", but TensorRT EP requires CUDA v12.0-v12.8.");
        } else {
            AddLog("No CUDA 12.0-12.8 Toolkit with required runtime DLLs was found.");
        }
        return false;
    }

    runtime_deps::SetTensorRtCudaDir(candidates.front().bin_dir);
    AddLog("Using compatible CUDA directory: " + candidates.front().bin_dir);
    runtime_deps::SetTensorRtCudnnDir("");
    AddLog("cuDNN directory reset; it must match the selected CUDA version.");
    CheckFiles();
    return true;
}

void DependencyInstaller::BrowseAndImportCudnnBin() {
    if (TryAutoImportInstalledCudnn()) {
        return;
    }

    AddLog("Please select a cuDNN 9 bin directory matching the selected CUDA version.");
    const std::string dir = BrowseFolder(hwnd, "选择与当前 CUDA 版本同步的 cuDNN bin 目录");
    if (dir.empty()) {
        return;
    }

    std::string resolved_dir;
    if (!ResolveCudnnReferenceDir(dir, &resolved_dir)) {
        std::string issue;
        HasSupportedCudnnRuntimeFiles(EnsureTrailingSlash(dir), &issue);
        AddLog(issue.empty() ? ("No compatible cuDNN 9 runtime DLLs found in: " + dir) : issue);
        return;
    }

    runtime_deps::SetTensorRtCudnnDir(resolved_dir);
    AddLog("Using cuDNN directory: " + resolved_dir);
    CheckFiles();
}

bool DependencyInstaller::TryAutoImportInstalledCudnn() {
    const auto candidates = FindInstalledCudnnBins();
    if (candidates.empty()) {
        AddLog("No cuDNN 9 runtime directory matching the selected CUDA version was found.");
        return false;
    }

    runtime_deps::SetTensorRtCudnnDir(candidates.front());
    AddLog("Using cuDNN directory: " + candidates.front());
    CheckFiles();
    return true;
}

// ---------------------------------------------------------------------------
void DependencyInstaller::AddLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(log_mtx);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    log_lines.push_back(std::string(buf) + " " + msg);
    if (log_lines.size() > 200) log_lines.erase(log_lines.begin());
}

void DependencyInstaller::StartDownload(const DepFile& file) {
    if (current_task || download_thread.joinable()) {
        if (download_thread.joinable()) download_thread.join();
        current_task.reset();
    }

    const std::string url = ResolveDownloadUrl(file);
    const std::string filename = file.filename;
    const std::string extract_pattern = file.extract_from;
    const std::string install_dir = GetInstallDirForFile(file);

    current_task = std::make_unique<DownloadTask>();
    current_task->url = url;
    current_task->filename = filename;
    AddLog("Downloading: " + filename + " -> " + install_dir);

    const int download_backend = selected_backend;
    download_thread = std::thread([this, extract_pattern, install_dir, download_backend]() {
        DownloadTask& task = *current_task;

        // Determine actual download filename from URL
        std::string url_basename = task.url.substr(task.url.rfind('/') + 1);
        size_t q = url_basename.find('?');
        if (q != std::string::npos) url_basename = url_basename.substr(0, q);

        bool is_zip = (url_basename.size() > 4 &&
                       _stricmp(url_basename.c_str() + url_basename.size() - 4, ".zip") == 0);
        bool is_exe = (url_basename.size() > 4 &&
                       _stricmp(url_basename.c_str() + url_basename.size() - 4, ".exe") == 0);
        std::string download_fn = url_basename;
        if (task.filename == "onnxruntime.dll" && download_backend == 0) {
            download_fn = "microsoft.ml.onnxruntime.directml.1.24.3.nupkg";
            is_zip = true;
            is_exe = false;
        } else if (task.filename == "DirectML.dll") {
            download_fn = "microsoft.ai.directml.1.15.4.nupkg";
            is_zip = true;
            is_exe = false;
        }

        HINTERNET session = WinHttpOpen(
            L"LozeeAimDep/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0
        );
        if (!session) {
            task.failed = true;
            task.error_msg = "WinHttpOpen failed";
            AddLog("ERR: WinHttpOpen failed for " + download_fn);
            return;
        }

        // Parse URL
        std::wstring wurl(task.url.begin(), task.url.end());
        URL_COMPONENTS url_comp = {};
        url_comp.dwStructSize = sizeof(url_comp);
        wchar_t host[256] = {}, path[1024] = {};
        url_comp.lpszHostName = host;
        url_comp.dwHostNameLength = 256;
        url_comp.lpszUrlPath = path;
        url_comp.dwUrlPathLength = 1024;
        WinHttpCrackUrl(wurl.c_str(), 0, 0, &url_comp);

        HINTERNET conn = WinHttpConnect(session, host, url_comp.nPort, 0);
        if (!conn) {
            task.failed = true;
            task.error_msg = "WinHttpConnect failed";
            AddLog("ERR: Cannot connect to host for " + download_fn);
            WinHttpCloseHandle(session);
            return;
        }

        DWORD flags = (url_comp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!req) {
            task.failed = true;
            task.error_msg = "WinHttpOpenRequest failed";
            AddLog("ERR: Request failed for " + download_fn);
            WinHttpCloseHandle(conn);
            WinHttpCloseHandle(session);
            return;
        }

        // Follow HTTP redirects (GitHub, CDN, etc.)
        DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));

        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, nullptr)) {
            task.failed = true;
            task.error_msg = "SendRequest/ReceiveResponse failed";
            AddLog("ERR: HTTP request failed for " + download_fn);
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            WinHttpCloseHandle(session);
            return;
        }

        const DWORD http_status = QueryHttpStatus(req);
        if (http_status < 200 || http_status >= 300) {
            task.failed = true;
            task.error_msg = "HTTP " + std::to_string(http_status);
            AddLog("ERR: HTTP " + std::to_string(http_status) + " for " + download_fn);
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            WinHttpCloseHandle(session);
            return;
        }

        task.total_bytes = QueryContentLength(req);

        // Download to file
        CreateDirectoryA(install_dir.c_str(), nullptr);
        std::string out_path = install_dir + download_fn;
        std::string tmp_path = out_path + ".tmp";
        std::ofstream out_file(tmp_path, std::ios::binary);
        if (!out_file) {
            task.failed = true;
            task.error_msg = "Cannot create file";
            AddLog("ERR: Cannot write " + download_fn);
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            WinHttpCloseHandle(session);
            return;
        }

        int64_t downloaded = 0;
        char buf[65536];
        DWORD bytes_read = 0;
        while (WinHttpReadData(req, buf, sizeof(buf), &bytes_read) && bytes_read > 0) {
            out_file.write(buf, bytes_read);
            downloaded += static_cast<int64_t>(bytes_read);
            task.downloaded_bytes = downloaded;
            if (task.total_bytes > 0) {
                task.progress = SafeProgressFraction(downloaded, task.total_bytes.load());
            }
        }
        out_file.close();

        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);

        if (downloaded == 0 && task.total_bytes == 0) {
            DeleteFileA(tmp_path.c_str());
            task.failed = true;
            task.error_msg = "Empty download";
            AddLog("ERR: Empty download: " + download_fn);
        } else {
            DeleteFileA(out_path.c_str());
            MoveFileA(tmp_path.c_str(), out_path.c_str());
            task.progress = 1.0f;

            if (!extract_pattern.empty()) {
                AddLog("Extracting: " + task.filename);

                bool extracted = false;
                std::string extraction_output_dir = install_dir;
                if (download_backend == runtime_deps::BackendTensorRT &&
                    IsCudnnReferenceDependencyName(task.filename)) {
                    extraction_output_dir = runtime_deps::BackendDir(runtime_deps::BackendTensorRT) + "cudnn\\";
                    CreateDirectoryA(extraction_output_dir.c_str(), nullptr);
                } else if (download_backend == runtime_deps::BackendTensorRT &&
                    IsTensorRtRuntimeGroupName(task.filename)) {
                    const int deleted_count = CleanTensorRtRuntimeFiles(extraction_output_dir);
                    if (deleted_count > 0) {
                        AddLog("Cleaned old TensorRT SDK files before extraction: " + std::to_string(deleted_count));
                    }
                }
                if (is_exe && _stricmp(task.filename.c_str(), "opencv_world4120.dll") == 0) {
                    extracted = ExtractFromOpenCvInstaller(out_path, extraction_output_dir, extract_pattern);
                } else {
                    std::string tmp_extract = install_dir + "_extract_" + task.filename;
                    CreateDirectoryA(tmp_extract.c_str(), nullptr);
                    extracted = ExtractFromZip(out_path, tmp_extract, extraction_output_dir, extract_pattern);
                }

                if (extracted) {
                    if (download_backend == runtime_deps::BackendTensorRT &&
                        IsCudnnReferenceDependencyName(task.filename)) {
                        runtime_deps::SetTensorRtCudnnDir(extraction_output_dir);
                        AddLog("Using cuDNN directory: " + extraction_output_dir);
                    }
                    AddLog("Extract OK: " + task.filename);
                    DeleteFileA(out_path.c_str());
                } else {
                    AddLog("WARN: auto-extract failed, keeping " + download_fn +
                        " for manual extraction");
                }
            } else if (is_exe) {
                AddLog("Downloaded installer: " + download_fn +
                    " — run it manually and copy DLLs to this folder");
            } else if (is_zip) {
                AddLog("Downloaded archive — extract and place: " + task.filename);
            }

            task.completed = true;
            AddLog("OK: " + download_fn);
        }
    });
}

// ---------------------------------------------------------------------------
bool DependencyInstaller::ExtractFromZip(
    const std::string& zip_path,
    const std::string& dest_dir,
    const std::string& output_dir,
    const std::string& pattern) {
    DeleteDirectoryTree(dest_dir);
    CreateDirectoryA(dest_dir.c_str(), nullptr);

    const std::string extract_dir = EnsureTrailingSlash(dest_dir);
    const std::string escaped_zip = EscapePowerShellSingleQuoted(zip_path);
    const std::string escaped_dir = EscapePowerShellSingleQuoted(extract_dir);
    const std::string escaped_patterns = EscapePowerShellSingleQuoted(pattern);

    std::string ps_cmd = "powershell -NoProfile -Command \""
        "Add-Type -AssemblyName System.IO.Compression.FileSystem;"
        "$zip = [System.IO.Compression.ZipFile]::OpenRead('" + escaped_zip + "');"
        "$patterns = '" + escaped_patterns + "' -split ';';"
        "$count = 0;"
        "foreach($e in $zip.Entries) {"
        "  $name = Split-Path $e.FullName -Leaf;"
        "  foreach($pat in $patterns) {"
        "    if ([string]::IsNullOrWhiteSpace($pat)) { continue }"
        "    $leafPat = Split-Path $pat -Leaf;"
        "    if ($name -like $leafPat) {"
        "      $dest = '" + escaped_dir + "' + $name;"
        "      $d = Split-Path $dest -Parent;"
        "      if (!(Test-Path $d)) { [System.IO.Directory]::CreateDirectory($d) | Out-Null };"
        "      [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $dest, $true);"
        "      $count++;"
        "      break;"
        "    }"
        "  }"
        "}"
        "$zip.Dispose();"
        "if ($count -eq 0) { exit 2 }\"";

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (!CreateProcessA(nullptr, ps_cmd.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }

    DWORD wait = WaitForSingleObject(pi.hProcess, 120000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (wait != WAIT_OBJECT_0 || exit_code != 0) {
        DeleteDirectoryTree(dest_dir);
        return false;
    }

    std::string exe_dir = output_dir;
    if (!exe_dir.empty() && exe_dir.back() != '\\' && exe_dir.back() != '/') {
        exe_dir += '\\';
    }
    CreateDirectoryA(exe_dir.c_str(), nullptr);
    int moved_count = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((extract_dir + "*.*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.') continue;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            std::string src = extract_dir + fd.cFileName;
            std::string dst = exe_dir + fd.cFileName;
            DeleteFileA(dst.c_str());
            if (MoveFileA(src.c_str(), dst.c_str())) {
                ++moved_count;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    DeleteDirectoryTree(dest_dir);
    return moved_count > 0;
}

bool DependencyInstaller::ExtractFromOpenCvInstaller(
    const std::string& installer_path,
    const std::string& output_dir,
    const std::string& pattern) {
    const std::string extract_dir =
        EnsureTrailingSlash(GetExeDir()) + "_opencv_extract_" +
        std::to_string(GetCurrentProcessId()) + "_" +
        std::to_string(GetTickCount64());

    DeleteDirectoryTree(extract_dir);
    if (!CreateDirectoryA(extract_dir.c_str(), nullptr)) {
        return false;
    }

    std::string cmd = "\"" + installer_path + "\" -y -o\"" + extract_dir + "\"";
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DeleteDirectoryTree(extract_dir);
        return false;
    }

    const DWORD wait = WaitForSingleObject(pi.hProcess, 300000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (wait != WAIT_OBJECT_0 || exit_code != 0) {
        DeleteDirectoryTree(extract_dir);
        return false;
    }

    const auto patterns = SplitPatternList(pattern);
    std::vector<std::string> matched_files;
    CollectMatchingFiles(extract_dir, patterns, matched_files);

    int copied_count = 0;
    std::string exe_dir = output_dir;
    if (!exe_dir.empty() && exe_dir.back() != '\\' && exe_dir.back() != '/') {
        exe_dir += '\\';
    }
    CreateDirectoryA(exe_dir.c_str(), nullptr);
    for (const auto& src : matched_files) {
        const std::string dst = exe_dir + BaseNameOfPath(src);
        DeleteFileA(dst.c_str());
        if (CopyFileA(src.c_str(), dst.c_str(), FALSE)) {
            ++copied_count;
        }
    }

    DeleteDirectoryTree(extract_dir);
    return copied_count > 0;
}

void DependencyInstaller::Render() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)window_w, (float)window_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::Begin("LozeeAim 依赖管理器", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    const int required_count = CountRequiredFiles();
    const int missing_count = CountMissingRequiredFiles();
    const int runtime_issue_count = CountRuntimeIssueRequiredFiles();
    const auto legacy_files = ListRootLegacyDependencies();

    ImGui::TextColored(UiBlue(), "LozeeAim 依赖中心");
    ImGui::SameLine();
    ImGui::TextDisabled("后端隔离 · 自动下载 · 路径引用");
    ImGui::Separator();

    ImGui::BeginChild(
        "overview_panel",
        ImVec2(0, 122),
        ImGuiChildFlags_Borders,
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    SectionTitle("概览");
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(UiMuted(), "推理后端");
    ImGui::SameLine();
    if (ImGui::RadioButton("DirectML", &selected_backend, 0)) {
        SaveSelectedBackendToConfig();
        CheckFiles();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("TensorRT", &selected_backend, 1)) {
        SaveSelectedBackendToConfig();
        CheckFiles();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("CPU", &selected_backend, 2)) {
        SaveSelectedBackendToConfig();
        CheckFiles();
    }
    ImGui::SameLine();
    ImGui::TextColored(UiMuted(), "当前: %s", BackendDisplayName(selected_backend));

    char required_text[32] = {};
    char missing_text[32] = {};
    snprintf(required_text, sizeof(required_text), "%d", required_count);
    snprintf(missing_text, sizeof(missing_text), "%d", missing_count);

    ImGui::Columns(3, "summary_cards", false);
    MetricCard("所需依赖", required_text, UiBlue());
    ImGui::NextColumn();
    MetricCard("缺失依赖", missing_text, missing_count == 0 ? UiSuccess() : UiDanger());
    ImGui::NextColumn();
    MetricCard("状态", all_ready ? "READY" : "CHECK", all_ready ? UiSuccess() : UiWarning());
    ImGui::Columns(1);
    ImGui::EndChild();

    if (selected_backend == 1) {
        ImGui::BeginChild(
            "tensorrt_actions_panel",
            ImVec2(0, 118),
            ImGuiChildFlags_Borders,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        SectionTitle("CUDA / cuDNN");
        TextEllipsisLine("CUDA:", runtime_deps::TensorRtCudaDir());
        TextEllipsisLine("cuDNN:", runtime_deps::TensorRtCudnnDir());
        if (ImGui::Button("选择 CUDA 目录", ImVec2(150, 26))) {
            BrowseAndImportCudaBin();
        }
        ImGui::SameLine();
        if (ImGui::Button("选择 cuDNN 目录", ImVec2(150, 26))) {
            BrowseAndImportCudnnBin();
        }
        ImGui::EndChild();
    }
    if (!legacy_files.empty()) {
        ImGui::TextColored(
            UiWarning(),
            "检测到根目录旧散装依赖，建议迁移到 deps 目录，避免后端互相污染。");
        if (ImGui::CollapsingHeader("根目录旧依赖列表")) {
            ImGui::BeginChild("legacy_root_deps", ImVec2(0, 70), true);
            for (const auto& file : legacy_files) {
                ImGui::BulletText("%s", file.c_str());
            }
            ImGui::EndChild();
        }
    }

    const auto local_yolo_models = ListLocalYoloModels();
    const float model_panel_height = 52.0f;
    ImGui::BeginChild(
        "model_panel",
        ImVec2(0, model_panel_height),
        ImGuiChildFlags_Borders,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    std::string yolo_status_text;
    ImVec4 yolo_status_color = UiDanger();
    if (local_yolo_models.empty()) {
        yolo_status_text = "YOLO 模型缺失，请导入 .onnx";
    } else {
        char status_buf[256] = {};
        snprintf(
            status_buf,
            sizeof(status_buf),
            "YOLO 已就绪 %d 个，当前：%s",
            static_cast<int>(local_yolo_models.size()),
            local_yolo_models.front().c_str());
        yolo_status_text = status_buf;
        yolo_status_color = UiSuccess();
    }
    const std::string yolo_tooltip_text =
        local_yolo_models.empty() ? model_paths::YoloDirA() : local_yolo_models.front();
    const float import_model_width = 106.0f;
    const float model_content_width = ImGui::GetContentRegionAvail().x;
    float model_info_width = model_content_width - import_model_width - 16.0f;
    if (model_info_width < 260.0f) {
        model_info_width = 260.0f;
    }
    ImGui::Columns(2, "model_panel_columns", false);
    ImGui::SetColumnWidth(0, model_info_width);
    ImGui::SetCursorPosY((model_panel_height - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextColored(UiBlue(), "YOLO 模型");
    ImGui::SameLine();
    ImGui::TextColored(yolo_status_color, "%s", yolo_status_text.c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", yolo_tooltip_text.c_str());
    }
    ImGui::NextColumn();
    if (ActionButton("导入模型", import_model_width)) {
        BrowseAndCopyYoloModel();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("导入 YOLO ONNX 模型到 models\\yolo。");
    }
    ImGui::Columns(1);
    ImGui::EndChild();

    // ---- File list ----
    int visible_required_rows = 0;
    for (const auto& file : all_files) {
        if (IsNeededForSelectedBackend(file) && file.filename != "*.onnx") {
            ++visible_required_rows;
        }
    }
    float deps_panel_height = ImGui::GetContentRegionAvail().y - 182.0f;
    if (selected_backend != runtime_deps::BackendTensorRT) {
        const float compact_deps_height =
            38.0f + static_cast<float>(visible_required_rows) * (72.0f + ImGui::GetStyle().ItemSpacing.y);
        if (deps_panel_height > compact_deps_height) {
            deps_panel_height = compact_deps_height;
        }
    }
    if (deps_panel_height < 150.0f) {
        deps_panel_height = 150.0f;
    }
    ImGui::BeginChild("required_deps_panel", ImVec2(0, deps_panel_height), ImGuiChildFlags_Borders);
    SectionTitle("所需依赖");

    for (size_t i = 0; i < all_files.size(); i++) {
        const bool needed = IsNeededForSelectedBackend(all_files[i]);
        if (!needed) continue;
        if (all_files[i].filename == "*.onnx") continue;

        ImGui::PushID(static_cast<int>(i));
        ImGui::BeginChild(
            "dep_row",
            ImVec2(0, 72),
            ImGuiChildFlags_Borders,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        const float row_width = ImGui::GetContentRegionAvail().x;
        const float status_col_width = 92.0f;
        float action_col_width = 330.0f;
        float info_col_width =
            row_width - status_col_width - action_col_width - 20.0f;
        if (info_col_width < 300.0f) {
            action_col_width = 310.0f;
            info_col_width =
                row_width - status_col_width - action_col_width - 20.0f;
        }
        if (info_col_width < 220.0f) {
            info_col_width = 220.0f;
        }

        ImGui::Columns(3, "dep_row_columns", false);
        ImGui::SetColumnWidth(0, info_col_width);
        ImGui::SetColumnWidth(1, action_col_width);
        ImGui::SetColumnWidth(2, status_col_width);

        ImGui::Text("%s", all_files[i].filename.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", all_files[i].description.c_str());
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetColumnWidth() - 12.0f);
        ImGui::TextColored(UiMuted(), "%s", all_files[i].description.c_str());
        ImGui::PopTextWrapPos();

        ImGui::NextColumn();
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        const bool runtime_issue = i < file_runtime_issue.size() && file_runtime_issue[i];
        if (!file_present[i] || runtime_issue) {
            bool has_action_item = false;
            bool is_current = current_task && current_task->filename == all_files[i].filename
                           && !current_task->completed.load() && !current_task->failed.load();

            const std::string dl_url = ResolveDownloadUrl(all_files[i]);

            if (!is_current && !dl_url.empty()) {
                if (ActionButton("下载", 82.0f)) {
                    StartDownload(all_files[i]);
                }
                has_action_item = true;
            } else if (is_current) {
                const int64_t total_bytes = current_task->total_bytes.load();
                if (total_bytes > 0) {
                    ImGui::ProgressBar(
                        SafeProgressFraction(current_task->downloaded_bytes.load(), total_bytes),
                        ImVec2(120, 0));
                } else {
                    const float pulse = static_cast<float>(std::fmod(ImGui::GetTime() * 0.35, 1.0));
                    ImGui::ProgressBar(pulse, ImVec2(120, 0), "下载中...");
                }
                has_action_item = true;
            }
            if (selected_backend == runtime_deps::BackendTensorRT &&
                IsTensorRtRuntimeGroupName(all_files[i].filename)) {
                if (has_action_item) ImGui::SameLine();
                if (ActionButton("导入目录", 96.0f)) {
                    BrowseAndImportTensorRtSdk();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("选择 TensorRT SDK 根目录、bin 目录或 lib 目录，并复制同一 SDK 包内的必要 DLL。");
                }
                has_action_item = true;
            } else if (selected_backend == runtime_deps::BackendTensorRT &&
                (IsCudaReferenceDependencyName(all_files[i].filename) ||
                 IsCudnnReferenceDependencyName(all_files[i].filename))) {
                if (has_action_item) ImGui::SameLine();
                if (ActionButton("选择目录", 96.0f)) {
                    if (IsCudaReferenceDependencyName(all_files[i].filename)) {
                        BrowseAndImportCudaBin();
                    } else {
                        BrowseAndImportCudnnBin();
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("保存依赖 bin 目录引用，不复制 CUDA/cuDNN DLL。");
                }
                has_action_item = true;
            } else if (all_files[i].filename.find('*') == std::string::npos) {
                if (has_action_item) ImGui::SameLine();
                if (ActionButton("导入", 82.0f)) {
                    BrowseAndCopy(all_files[i].filename);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("从本机已下载/已安装目录选择并复制到当前隔离依赖目录。");
                }
                has_action_item = true;
            }
            if (!all_files[i].info_url.empty()) {
                if (has_action_item) ImGui::SameLine();
                if (ActionButton("打开页面", 102.0f)) {
                    ShellExecuteA(nullptr, "open", all_files[i].info_url.c_str(),
                                  nullptr, nullptr, SW_SHOW);
                    AddLog("打开: " + all_files[i].info_url);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("在浏览器中打开下载页面");
                }
            }
        }

        ImGui::NextColumn();
        ImGui::Dummy(ImVec2(0.0f, 11.0f));
        const bool compatibility_issue =
            runtime_issue &&
            (IsCudaReferenceGroupName(all_files[i].filename) ||
             IsCudnnReferenceGroupName(all_files[i].filename) ||
             IsTensorRtRuntimeGroupName(all_files[i].filename));
        const char* status_text = compatibility_issue ? "不兼容" :
            (runtime_issue ? "加载失败" : (file_present[i] ? "已就绪" : "缺失"));
        const ImVec4 status_color = runtime_issue ? UiWarning() : (file_present[i] ? UiSuccess() : UiDanger());
        const float status_text_width = ImGui::CalcTextSize(status_text).x;
        const float status_indent =
            ImGui::GetColumnWidth() - status_text_width - ImGui::GetStyle().ItemSpacing.x * 2.0f;
        if (status_indent > 0.0f) {
            ImGui::Indent(status_indent);
        }
        ImGui::TextColored(status_color, "%s", status_text);
        if (ImGui::IsItemHovered() && !file_location[i].empty()) {
            ImGui::SetTooltip("%s", file_location[i].c_str());
        }
        if (status_indent > 0.0f) {
            ImGui::Unindent(status_indent);
        }
        ImGui::Columns(1);

        ImGui::EndChild();
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Progress ----
    bool download_active = current_task && !current_task->completed.load() && !current_task->failed.load();
    if (download_active) {
        const int64_t downloaded_bytes = current_task->downloaded_bytes.load();
        const int64_t total_bytes = current_task->total_bytes.load();
        ImGui::Text("下载中: %s", current_task->filename.c_str());
        if (total_bytes > 0) {
            const float progress = SafeProgressFraction(downloaded_bytes, total_bytes);
            char overlay[32] = {};
            snprintf(overlay, sizeof(overlay), "%.1f%%", progress * 100.0f);
            ImGui::ProgressBar(progress, ImVec2(-1, 20), overlay);
            ImGui::Text("%s / %s",
                FormatBytes(downloaded_bytes).c_str(),
                FormatBytes(total_bytes).c_str());
        } else {
            const float pulse = static_cast<float>(std::fmod(ImGui::GetTime() * 0.35, 1.0));
            ImGui::ProgressBar(pulse, ImVec2(-1, 20), "下载中...");
            ImGui::Text("%s / 未知大小",
                FormatBytes(downloaded_bytes).c_str());
        }
    } else if (current_task && current_task->completed.load()) {
        ImGui::TextColored(UiSuccess(), "下载完成: %s", current_task->filename.c_str());
    } else if (current_task && current_task->failed.load()) {
        ImGui::TextColored(UiDanger(), "下载失败: %s", current_task->filename.c_str());
        if (!current_task->error_msg.empty())
            ImGui::TextDisabled("%s", current_task->error_msg.c_str());
        if (download_thread.joinable()) { download_thread.join(); }
        current_task.reset();
    }

    // ---- Download All ----
    if (!download_active) {
        int missing_count = 0;
        for (size_t i = 0; i < all_files.size(); i++) {
            const bool needed = IsNeededForSelectedBackend(all_files[i]);
            const bool has_url = !ResolveDownloadUrl(all_files[i]).empty();
            const bool runtime_issue = i < file_runtime_issue.size() && file_runtime_issue[i];
            if (needed && (!file_present[i] || runtime_issue) && has_url) missing_count++;
        }

        if (missing_count > 0) {
            if (ImGui::Button("一键下载所有", ImVec2(160, 28))) {
                for (size_t i = 0; i < all_files.size(); i++) {
                    const bool needed = IsNeededForSelectedBackend(all_files[i]);
                    const std::string dl_url = ResolveDownloadUrl(all_files[i]);
                    const bool runtime_issue = i < file_runtime_issue.size() && file_runtime_issue[i];
                    if (needed && (!file_present[i] || runtime_issue) && !dl_url.empty()) {
                        StartDownload(all_files[i]);
                        break;
                    }
                }
            }
        }
    }

    ImGui::SameLine();

    if (all_ready) {
        ImGui::PushStyleColor(ImGuiCol_Button, UiSuccess());
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        if (ImGui::Button("依赖就绪 - 退出并重启", ImVec2(240, 28))) {
            if (LaunchMainProcess()) {
                AddLog("正在重启主程序");
                window_should_close = true;
            } else {
                AddLog("ERR: 重启主程序失败");
            }
        }
        ImGui::PopStyleColor(2);
    } else {
        if (missing_count > 0) {
            ImGui::TextColored(UiWarning(), "有依赖缺失，请下载或打开页面手动下载");
        } else if (runtime_issue_count > 0) {
            ImGui::TextColored(UiWarning(), "依赖文件已存在，但运行时加载失败，请查看异常项提示");
        } else {
            ImGui::TextColored(UiWarning(), "依赖状态需要重新检查");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Log ----
    ImGui::TextColored(UiBlue(), "日志:");
    ImGui::BeginChild("log_area", ImVec2(0, 120), true);
    {
        std::lock_guard<std::mutex> lock(log_mtx);
        for (const auto& line : log_lines) {
            ImGui::TextUnformatted(line.c_str());
        }
        if (!log_lines.empty())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::Spacing();
    if (ImGui::Button("退出", ImVec2(80, 24))) {
        window_should_close = true;
    }

    ImGui::PopStyleVar(2);
    ImGui::End();

    // Render
    ImGui::Render();
    const float clear_color[4] = { 0.96f, 0.98f, 1.0f, 1.0f };
    d3d_context->OMSetRenderTargets(1, &rtv, nullptr);
    d3d_context->ClearRenderTargetView(rtv, clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    HRESULT present_hr = swap_chain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    if (present_hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
    if (FAILED(present_hr)) {
        swap_chain->Present(0, 0);
    }

    // Update window size tracking
    RECT rc;
    GetClientRect(hwnd, &rc);
    window_w = rc.right - rc.left;
    window_h = rc.bottom - rc.top;
}

// ---------------------------------------------------------------------------
bool DependencyInstaller::CheckAllReady() {
    LoadSelectedBackendFromConfig();
    CheckFiles();
    return all_ready;
}

// ---------------------------------------------------------------------------
int DependencyInstaller::Run() {
    g_installer = this;
    if (!InitWindow()) {
        g_installer = nullptr;
        return -1;
    }

    AddLog("依赖管理器启动");
    AddLog("请选择后端并确保所有文件就位");

    LoadSelectedBackendFromConfig();
    CheckFiles();

    MSG msg;
    while (!window_should_close) {
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                window_should_close = true;
                break;
            }
        }
        if (window_should_close) break;

        Render();

        // Auto-continue download queue after each completion
        if (current_task && current_task->completed.load()) {
            if (download_thread.joinable()) download_thread.join();
            size_t task_idx = ~0ULL;
            for (size_t i = 0; i < all_files.size(); i++) {
                if (all_files[i].filename == current_task->filename) {
                    task_idx = i;
                    break;
                }
            }
            current_task.reset();
            CheckFiles();

            // Start next download if any
            for (size_t i = task_idx + 1; i < all_files.size(); i++) {
                const bool needed = IsNeededForSelectedBackend(all_files[i]);
                const std::string dl_url = ResolveDownloadUrl(all_files[i]);
                if (needed && !file_present[i] && !dl_url.empty()) {
                    StartDownload(all_files[i]);
                    break;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (download_thread.joinable()) download_thread.join();
    CleanupD3D();
    g_installer = nullptr;

    return 0;
}
