#include "AimAssistant.hpp"
#include "DependencyInstaller.hpp"
#include "ModelPaths.hpp"
#include "ObjectDetector.hpp"
#include "RuntimeDependencyLoader.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "onnxruntime.lib")

#ifdef _DEBUG
#pragma comment(lib, "opencv_world4120d.lib")
#else
#pragma comment(lib, "opencv_world4120.lib")
#endif

static std::ofstream g_logFile;

struct ComScope {
    HRESULT hr;

    ComScope() : hr(CoInitialize(nullptr)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }
};

namespace {
constexpr const char* kTensorRtBuildArg = "--build-trt-engine";
constexpr const char* kTensorRtManifestName = "lozeeaim_trt_engine.manifest";
static ULONGLONG g_tensor_rt_wait_start_ms = 0;

struct FileStamp {
    ULONGLONG size = 0;
    ULONGLONG write_time = 0;
};

bool IsTensorRtMode(const Config& cfg) {
    return cfg.inference_provider == runtime_deps::BackendTensorRT && !cfg.use_cpu_inference;
}

std::string TensorRtEngineCacheDir() {
    return model_paths::ExeDirA() + "engine_cache\\";
}

std::string TensorRtManifestPath() {
    return TensorRtEngineCacheDir() + kTensorRtManifestName;
}

std::string QuoteArg(const std::string& value) {
    return "\"" + value + "\"";
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string output(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, output.data(), size, nullptr, nullptr);
    return output;
}

std::wstring NormalizeWidePath(const std::wstring& path) {
    wchar_t full_path[MAX_PATH] = {};
    const DWORD size = GetFullPathNameW(path.c_str(), MAX_PATH, full_path, nullptr);
    if (size == 0 || size >= MAX_PATH) {
        return path;
    }
    return full_path;
}

bool GetFileStamp(const std::wstring& path, FileStamp& stamp) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return false;
    }
    stamp.size =
        (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) |
        static_cast<ULONGLONG>(data.nFileSizeLow);
    stamp.write_time =
        (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) |
        static_cast<ULONGLONG>(data.ftLastWriteTime.dwLowDateTime);
    return true;
}

bool GetFileStampA(const std::string& path, FileStamp& stamp) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
        return false;
    }
    stamp.size =
        (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) |
        static_cast<ULONGLONG>(data.nFileSizeLow);
    stamp.write_time =
        (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) |
        static_cast<ULONGLONG>(data.ftLastWriteTime.dwLowDateTime);
    return true;
}

std::string NormalizePathA(const std::string& path) {
    char full_path[MAX_PATH] = {};
    const DWORD size = GetFullPathNameA(path.c_str(), MAX_PATH, full_path, nullptr);
    if (size == 0 || size >= MAX_PATH) {
        return path;
    }
    return full_path;
}

void AppendFileStamp(std::ostringstream& signature, const std::string& label, const std::string& path) {
    FileStamp stamp;
    const std::string normalized = NormalizePathA(path);
    signature << label << "=" << normalized << ":";
    if (GetFileStampA(normalized, stamp)) {
        signature << stamp.size << ":" << stamp.write_time;
    } else {
        signature << "missing";
    }
    signature << ";";
}

std::string JoinDirFile(std::string dir, const char* filename) {
    if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') {
        dir += '\\';
    }
    return dir + filename;
}

std::string BuildTensorRtRuntimeSignature() {
    std::ostringstream signature;
    signature << "provider_options=trt_fp16_cache_v1;";
    AppendFileStamp(signature, "ort", runtime_deps::DependencyPath("onnxruntime.dll", runtime_deps::BackendTensorRT));
    AppendFileStamp(signature, "ort_cuda", runtime_deps::DependencyPath("onnxruntime_providers_cuda.dll", runtime_deps::BackendTensorRT));
    AppendFileStamp(signature, "ort_tensorrt", runtime_deps::DependencyPath("onnxruntime_providers_tensorrt.dll", runtime_deps::BackendTensorRT));
    AppendFileStamp(signature, "trt", runtime_deps::DependencyPath("nvinfer_10.dll", runtime_deps::BackendTensorRT));
    AppendFileStamp(signature, "cuda", JoinDirFile(runtime_deps::TensorRtCudaDir(), "cudart64_12.dll"));
    AppendFileStamp(signature, "cudnn", JoinDirFile(runtime_deps::TensorRtCudnnDir(), "cudnn64_9.dll"));
    return signature.str();
}

std::map<std::string, std::string> ReadKeyValueFile(const std::string& path) {
    std::map<std::string, std::string> values;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        values[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return values;
}

bool HasNonEmptyCacheFile(const std::string& pattern) {
    WIN32_FIND_DATAA fd = {};
    HANDLE handle = FindFirstFileA((TensorRtEngineCacheDir() + pattern).c_str(), &fd);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool found = false;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const ULONGLONG size =
            (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) |
            static_cast<ULONGLONG>(fd.nFileSizeLow);
        if (size > 0) {
            found = true;
            break;
        }
    } while (FindNextFileA(handle, &fd));

    FindClose(handle);
    return found;
}

bool HasTensorRtEngineBinary() {
    return HasNonEmptyCacheFile("*.engine") || HasNonEmptyCacheFile("*.plan");
}

bool HasMatchingTensorRtManifest(const Config& cfg) {
    if (!HasTensorRtEngineBinary()) {
        return false;
    }

    const std::wstring model_path = NormalizeWidePath(ObjectDetector::ResolveModelPath(cfg));
    FileStamp stamp;
    if (!GetFileStamp(model_path, stamp)) {
        return false;
    }

    const auto values = ReadKeyValueFile(TensorRtManifestPath());
    auto get_value = [&values](const std::string& key) -> std::string {
        const auto it = values.find(key);
        return it == values.end() ? std::string{} : it->second;
    };

    return get_value("version") == "1" &&
        get_value("provider") == "tensorrt" &&
        get_value("model_path") == WideToUtf8(model_path) &&
        get_value("model_size") == std::to_string(stamp.size) &&
        get_value("model_write_time") == std::to_string(stamp.write_time) &&
        get_value("runtime_signature") == BuildTensorRtRuntimeSignature();
}

void WriteTensorRtManifest(const Config& cfg) {
    const std::wstring model_path = NormalizeWidePath(ObjectDetector::ResolveModelPath(cfg));
    FileStamp stamp;
    if (!GetFileStamp(model_path, stamp)) {
        throw std::runtime_error("YOLO model file is missing.");
    }

    CreateDirectoryA(TensorRtEngineCacheDir().c_str(), nullptr);
    std::ofstream file(TensorRtManifestPath(), std::ios::out | std::ios::trunc);
    if (!file) {
        throw std::runtime_error("Cannot write TensorRT engine manifest.");
    }

    file << "version=1\n";
    file << "provider=tensorrt\n";
    file << "model_path=" << WideToUtf8(model_path) << "\n";
    file << "model_size=" << stamp.size << "\n";
    file << "model_write_time=" << stamp.write_time << "\n";
    file << "runtime_signature=" << BuildTensorRtRuntimeSignature() << "\n";
}

int RunTensorRtEngineBuilderMode() {
    try {
        SetCurrentDirectoryA(model_paths::ExeDirA().c_str());

        Config cfg;
        cfg.Load();
        cfg.inference_provider = runtime_deps::BackendTensorRT;
        cfg.use_cpu_inference = false;

        std::string dependency_error;
        if (!runtime_deps::Configure(runtime_deps::BackendTensorRT, false, &dependency_error)) {
            throw std::runtime_error(dependency_error);
        }

        model_paths::EnsureModelDirs();
        CreateDirectoryA(TensorRtEngineCacheDir().c_str(), nullptr);

        std::cout << "[TRT] Engine builder started." << std::endl;
        {
            ObjectDetector detector(cfg);
            cv::Mat warmup_frame(cfg.crop_size, cfg.crop_size, CV_8UC3, cv::Scalar(0, 0, 0));
            std::vector<Detection> detections;
            TimingDetails timings;
            detector.Detect(warmup_frame, detections, cfg, timings);
            std::cout << "[TRT] Engine warmup finished." << std::endl;
        }
        if (!HasTensorRtEngineBinary()) {
            throw std::runtime_error("TensorRT engine cache file was not generated.");
        }
        WriteTensorRtManifest(cfg);
        std::cout << "[TRT] Engine builder finished." << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[TRT] Engine builder failed: " << e.what() << std::endl;
        return 2;
    } catch (...) {
        std::cerr << "[TRT] Engine builder failed: unknown error." << std::endl;
        return 3;
    }
}

LRESULT CALLBACK TensorRtWaitWndProc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
    switch (msg) {
        case WM_CLOSE:
            MessageBeep(MB_ICONINFORMATION);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc = {};
            GetClientRect(hwnd, &rc);

            HBRUSH background = CreateSolidBrush(RGB(244, 248, 255));
            FillRect(hdc, &rc, background);
            DeleteObject(background);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(18, 84, 160));
            HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            HGDIOBJ old_font = SelectObject(hdc, font);

            const ULONGLONG elapsed = (GetTickCount64() - g_tensor_rt_wait_start_ms) / 1000;
            std::wostringstream text;
            text << L"正在编译 TensorRT Engine\n\n"
                 << L"首次可能需要数分钟，期间 GPU/系统可能短暂卡顿。\n"
                 << L"完成后主程序会自动启动。\n\n"
                 << L"已等待 " << elapsed << L" 秒";

            RECT text_rc = rc;
            InflateRect(&text_rc, -28, -24);
            DrawTextW(hdc, text.str().c_str(), -1, &text_rc, DT_CENTER | DT_VCENTER | DT_WORDBREAK);

            SelectObject(hdc, old_font);
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, w_param, l_param);
    }
}

int WaitForTensorRtBuilder(PROCESS_INFORMATION& process_info) {
    const wchar_t* class_name = L"LozeeAimTensorRtWait";
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TensorRtWaitWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = class_name;
    RegisterClassExW(&wc);

    const int window_w = 520;
    const int window_h = 260;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - window_w) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - window_h) / 2;
    HWND hwnd = CreateWindowExW(
        0,
        class_name,
        L"LozeeAim - TensorRT Engine",
        WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x,
        y,
        window_w,
        window_h,
        nullptr,
        nullptr,
        instance,
        nullptr);

    g_tensor_rt_wait_start_ms = GetTickCount64();
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    MSG msg = {};
    ULONGLONG last_invalidate = 0;
    while (true) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        const DWORD wait = WaitForSingleObject(process_info.hProcess, 50);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
        if (wait != WAIT_TIMEOUT) {
            break;
        }

        const ULONGLONG now = GetTickCount64();
        if (hwnd && now - last_invalidate > 500) {
            InvalidateRect(hwnd, nullptr, FALSE);
            last_invalidate = now;
        }
    }

    DWORD exit_code = 1;
    GetExitCodeProcess(process_info.hProcess, &exit_code);
    if (hwnd) {
        DestroyWindow(hwnd);
    }
    UnregisterClassW(class_name, instance);
    return static_cast<int>(exit_code);
}

bool LaunchTensorRtBuilder(PROCESS_INFORMATION& process_info, std::string& error) {
    char exe_path[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exe_path, MAX_PATH) == 0) {
        error = "GetModuleFileNameA failed.";
        return false;
    }

    std::string command = QuoteArg(exe_path) + " " + kTensorRtBuildArg;
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    if (!CreateProcessA(
            nullptr,
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            model_paths::ExeDirA().c_str(),
            &startup,
            &process_info)) {
        error = "CreateProcessA failed: " + std::to_string(GetLastError());
        return false;
    }
    return true;
}

void CloseProcessHandles(PROCESS_INFORMATION& process_info) {
    if (process_info.hThread) {
        CloseHandle(process_info.hThread);
        process_info.hThread = nullptr;
    }
    if (process_info.hProcess) {
        CloseHandle(process_info.hProcess);
        process_info.hProcess = nullptr;
    }
}

void EnsureTensorRtEngineReady(const Config& cfg) {
    if (!IsTensorRtMode(cfg) || HasMatchingTensorRtManifest(cfg)) {
        return;
    }

    std::cout << "[TRT] Engine cache is missing or stale; launching helper." << std::endl;

    PROCESS_INFORMATION process_info = {};
    std::string launch_error;
    if (!LaunchTensorRtBuilder(process_info, launch_error)) {
        throw std::runtime_error("TensorRT Engine helper launch failed: " + launch_error);
    }

    const int exit_code = WaitForTensorRtBuilder(process_info);
    CloseProcessHandles(process_info);

    if (exit_code != 0) {
        throw std::runtime_error("TensorRT Engine helper failed. See lozeeaim.log for details.");
    }
    if (!HasMatchingTensorRtManifest(cfg)) {
        throw std::runtime_error("TensorRT Engine helper finished but cache manifest is invalid.");
    }
}
}  // namespace

int main(int argc, char* argv[]) {
    SetConsoleTitleA("LozeeAim");
    SetCurrentDirectoryA(model_paths::ExeDirA().c_str());

    g_logFile.open("lozeeaim.log", std::ios::out | std::ios::app);
    if (g_logFile) {
        std::cout.rdbuf(g_logFile.rdbuf());
        std::cerr.rdbuf(g_logFile.rdbuf());
    }
    FreeConsole();

    ComScope com;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--deps" || arg == "/deps" || arg == "--dependency-manager") {
            DependencyInstaller installer;
            return installer.Run();
        }
        if (arg == kTensorRtBuildArg) {
            return RunTensorRtEngineBuilderMode();
        }
    }

    // Pre-flight dependency check — runs BEFORE AimAssistant so any
    // non-dependency crash won't loop back into the installer.
    {
        DependencyInstaller checker;
        if (!checker.CheckAllReady()) {
            checker.Run();
            return 0;
        }
    }

    try {
        Config cfg;
        cfg.Load();
        EnsureTensorRtEngineReady(cfg);

        std::string dependency_error;
        if (!runtime_deps::ConfigureFromConfig(&dependency_error)) {
            throw std::runtime_error(dependency_error);
        }
        AimAssistant assistant;
        assistant.Run();
    }
    catch (const std::exception& e) {
        std::cerr << "\n\n[FATAL ERROR] " << e.what() << std::endl;
        MessageBoxA(nullptr, e.what(), "LozeeAim - Fatal Error", MB_OK | MB_ICONERROR);
        return -1;
    }

    std::cout << "\nProgram finished." << std::endl;
    return 0;
}

