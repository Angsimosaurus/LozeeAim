#pragma once
#include <windows.h>
#include <d3d11.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>

struct DepFile {
    std::string filename;
    std::string description;
    std::string download_url;
    std::string extract_from;  // pattern to match inside archive (empty = same as filename)
    std::string info_url;      // URL to open in browser (e.g. NVIDIA login page)
    bool required_dml;
    bool required_trt;
    bool required_cpu;
};

struct DownloadTask {
    std::string url;
    std::string filename;
    std::atomic<bool> completed{false};
    std::atomic<bool> failed{false};
    std::atomic<float> progress{0.0f};
    std::atomic<int64_t> downloaded_bytes{0};
    std::atomic<int64_t> total_bytes{0};
    std::string error_msg;
};

class DependencyInstaller {
public:
    DependencyInstaller();
    ~DependencyInstaller();

    static bool OpenManagerProcess();

    int Run();
    bool CheckAllReady();  // silent check, returns true if all deps present

private:
    bool InitWindow();
    void CleanupD3D();
    void Render();
    void CheckFiles();
    void SaveSelectedBackendToConfig();
    void StartDownload(const DepFile& file);
    void AddLog(const std::string& msg);
    bool FileExists(const std::string& filename) const;
    bool FindDependencyFile(const DepFile& file, std::string* resolved_path = nullptr) const;
    bool IsNeededForSelectedBackend(const DepFile& file) const;
    std::string ResolveDownloadUrl(const DepFile& file) const;
    std::string GetInstallDirForFile(const DepFile& file) const;
    int CountRequiredFiles() const;
    int CountMissingRequiredFiles() const;
    int CountRuntimeIssueRequiredFiles() const;
    int CountPresentUnusedKnownFiles() const;
    void ValidateBackendRuntime();
    std::vector<std::string> ListRootLegacyDependencies() const;
    std::vector<std::string> ListLocalYoloModels() const;
    bool CopyLocalFile(const std::string& src_path, const std::string& filename);
    bool CopyLocalFileToRunDir(const std::string& src_path);
    void BrowseAndCopy(const std::string& filename);
    void BrowseAndCopyYoloModel();
    void EnsureAutoReferenceDirs();
    void BrowseAndImportTensorRtSdk();
    void BrowseAndImportCudaBin();
    bool TryAutoImportInstalledCuda();
    void BrowseAndImportCudnnBin();
    bool TryAutoImportInstalledCudnn();
    bool ImportFilesFromDirectory(const std::string& src_dir, const std::vector<std::string>& patterns, const std::string& dest_dir);
    bool ExtractFromZip(const std::string& zip_path, const std::string& dest_dir, const std::string& output_dir, const std::string& pattern);
    bool ExtractFromOpenCvInstaller(const std::string& installer_path, const std::string& output_dir, const std::string& pattern);
    std::string GetExeDir() const;
    void LoadSelectedBackendFromConfig();

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND hwnd = nullptr;
    WNDCLASSEXW wc = {};
    ID3D11Device* d3d_device = nullptr;
    ID3D11DeviceContext* d3d_context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    int window_w = 1080;
    int window_h = 720;
    bool d3d_ok = false;

    int selected_backend = 0;  // 0=DirectML, 1=TensorRT, 2=CPU
    std::vector<DepFile> all_files;
    std::vector<bool> file_present;
    std::vector<bool> file_runtime_issue;
    std::vector<std::string> file_location;

    std::unique_ptr<DownloadTask> current_task;
    std::thread download_thread;
    std::mutex log_mtx;
    std::vector<std::string> log_lines;
    bool all_ready = false;
    bool window_should_close = false;

    // URLs can be edited below or overridden via dep_urls.ini
    std::string base_url;
};
