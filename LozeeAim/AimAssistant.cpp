// [关键] 强制编译器使用 UTF-8 处理字符串，解决中文乱码
#pragma execution_character_set("utf-8")

#include "AimAssistant.hpp"
#include "DependencyInstaller.hpp"
#include "ModelPaths.hpp"
#include <iostream>
#include "TrajectoryGenerator.hpp"
#include <thread>
#include <cmath>
#include <cstdlib>
#include <cfloat>
#include <algorithm>

// --- SysKeyHook implementation ---
SysKeyHook::NtGetKey_t SysKeyHook::s_ntGetKey = nullptr;
bool SysKeyHook::s_useNative = false;

void SysKeyHook::Init() {
    HMODULE h = GetModuleHandleW(L"win32u.dll");
    if (!h) h = LoadLibraryW(L"win32u.dll");
    if (h) {
        s_ntGetKey = (NtGetKey_t)GetProcAddress(h, "NtUserGetAsyncKeyState");
        if (s_ntGetKey) s_useNative = true;
    }
    if (!s_useNative) {
        s_ntGetKey = nullptr;
    }
}

bool SysKeyHook::IsDown(int vk) {
    if (s_useNative && s_ntGetKey) {
        return (s_ntGetKey(vk) & 0x8000) != 0;
    }
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static Config LoadConfigFile() {
    Config c;
    c.Load();
    return c;
}

static std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};

    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};

    std::vector<char> buffer(static_cast<size_t>(size));
    WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), -1, buffer.data(), size, nullptr, nullptr);
    return std::string(buffer.data());
}

static std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 1) return {};

    std::vector<wchar_t> buffer(static_cast<size_t>(size));
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, buffer.data(), size);
    return std::wstring(buffer.data());
}

static std::vector<std::string> ScanModelFiles(const std::string& dir, const std::vector<std::string>& patterns) {
    model_paths::EnsureModelDirs();
    std::vector<std::string> files;
    for (const auto& pattern : patterns) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                files.emplace_back(fd.cFileName);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

static std::string NormalizeNnConfigPath(const std::string& path) {
    if (path.empty()) {
        return model_paths::NnConfigPath("nn_model.onnx");
    }
    if (!model_paths::IsAbsoluteA(path) &&
        path.find('\\') == std::string::npos &&
        path.find('/') == std::string::npos) {
        return model_paths::NnConfigPath(path);
    }
    return path;
}

static std::string ResolveNnInputPath(const std::string& path) {
    return model_paths::ResolveExistingPathA(NormalizeNnConfigPath(path));
}

static std::string ResolveNnOutputPath(const std::string& path) {
    return model_paths::ResolveOutputPathA(
        NormalizeNnConfigPath(path),
        model_paths::NnDirA(),
        "nn_model.onnx");
}

static bool UiEnglish(const Config& cfg) {
    return cfg.ui_language == 1;
}

static const char* UiText(const Config& cfg, const char* zh, const char* en) {
    return UiEnglish(cfg) ? en : zh;
}

static void DrawHelpTooltip(const char* text) {
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", text);
    }
}

static void DrawSectionHeader(const char* title) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.7f, 0.4f, 0.9f, 1.0f), "%s", title);
    ImGui::Separator();
}

static void DrawStatusBadge(const char* label, bool ok) {
    ImGui::TextColored(ok ? ImVec4(0.1f, 0.8f, 0.2f, 1.0f) : ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%s", label);
}

AimAssistant::AimAssistant()
    : cfg(LoadConfigFile()),
    overlay(),
    capturer(cfg.crop_size, cfg.crop_size),
    mouse(static_cast<MouseBackend>(cfg.mouse_backend)),
    crop_region((capturer.getWidth() - cfg.crop_size) / 2, (capturer.getHeight() - cfg.crop_size) / 2, cfg.crop_size, cfg.crop_size),
    crop_center(cfg.crop_size / 2, cfg.crop_size / 2)
{
    model_paths::EnsureModelDirs();
    SysKeyHook::Init();

    const std::string initial_nn_model = ResolveNnInputPath(cfg.nn_model_path);
    if (!initial_nn_model.empty() && nn_model.Load(initial_nn_model.c_str())) {
        nn_model.SetModelLoaded(true);
        nn_debug_once = true;
        nn_diag.clear();
        std::cout << "[OK] NN model loaded: " << initial_nn_model << std::endl;
    }

    if (!overlay.Init()) {
        throw std::runtime_error("Failed to initialize Overlay!");
    }
    if (cfg.prevent_screen_capture && !overlay.SetCaptureProtection(true)) {
        cfg.prevent_screen_capture = false;
        std::cout << "[WARN] Screen capture protection is not available on this system." << std::endl;
    }
    if (capturer.getWidth() < cfg.crop_size || capturer.getHeight() < cfg.crop_size) {
        throw std::runtime_error("Screen resolution is smaller than configured crop_size.");
    }

    mouse_correction_factor = (cfg.horizontal_fov / static_cast<double>(capturer.getWidth()))
        * (cfg.pixels_for_360_turn / 360.0);

    std::cout << "--- Detection region: center " << cfg.crop_size << "x" << cfg.crop_size << " ---" << std::endl;
    StartDetectorLoad();
}

AimAssistant::~AimAssistant() {
    if (detector_load_thread.joinable()) {
        detector_load_thread.join();
    }
    if (yolo_reload_thread.joinable()) {
        yolo_reload_thread.join();
    }
    if (nn_train_thread.joinable()) {
        nn_train_thread.join();
    }
    if (nn_collect_cursor_locked) {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0);
    }
    cfg.Save();
}

void AimAssistant::SetYoloReloadStatus(const std::string& status) {
    std::lock_guard<std::mutex> lock(yolo_mutex);
    yolo_reload_status = status;
}

std::string AimAssistant::GetYoloReloadStatus() const {
    std::lock_guard<std::mutex> lock(yolo_mutex);
    return yolo_reload_status;
}

void AimAssistant::SetDetectorStatus(const std::string& status) {
    std::lock_guard<std::mutex> lock(detector_mutex);
    detector_status = status;
}

std::string AimAssistant::GetDetectorStatus() const {
    std::lock_guard<std::mutex> lock(detector_mutex);
    return detector_status;
}

bool AimAssistant::IsDetectorReady() const {
    return detector_ready.load(std::memory_order_acquire) && detector != nullptr;
}

void AimAssistant::StartDetectorLoad() {
    if (detector_loading.load(std::memory_order_acquire) ||
        detector_ready.load(std::memory_order_acquire)) {
        return;
    }

    if (detector_load_thread.joinable()) {
        detector_load_thread.join();
    }

    Config cfg_snapshot = cfg;
    detector_ready.store(false, std::memory_order_release);
    detector_failed.store(false, std::memory_order_release);
    detector_loading.store(true, std::memory_order_release);

    if (cfg_snapshot.inference_provider == 1 && !cfg_snapshot.use_cpu_inference) {
        SetDetectorStatus("TensorRT Engine 编译/加载中，首次可能需要数分钟...");
    } else {
        SetDetectorStatus("YOLO 模型加载中...");
    }

    detector_load_thread = std::thread([this, cfg_snapshot]() {
        try {
            auto loaded = std::make_unique<ObjectDetector>(cfg_snapshot);
            detector = std::move(loaded);
            detector_failed.store(false, std::memory_order_release);
            detector_ready.store(true, std::memory_order_release);
            SetDetectorStatus("YOLO 模型已就绪");
        } catch (const std::exception& e) {
            detector.reset();
            detector_failed.store(true, std::memory_order_release);
            detector_ready.store(false, std::memory_order_release);
            SetDetectorStatus(std::string("ERR: ") + e.what());
        } catch (...) {
            detector.reset();
            detector_failed.store(true, std::memory_order_release);
            detector_ready.store(false, std::memory_order_release);
            SetDetectorStatus("ERR: Unknown detector load error");
        }
        detector_loading.store(false, std::memory_order_release);
    });
}

void AimAssistant::JoinFinishedDetectorLoad() {
    if (!detector_loading.load(std::memory_order_acquire) && detector_load_thread.joinable()) {
        detector_load_thread.join();
    }
}

void AimAssistant::DrawDetectorStatusOverlay() {
    if (detector_ready.load(std::memory_order_acquire) &&
        !detector_failed.load(std::memory_order_acquire) &&
        !yolo_reloading.load(std::memory_order_acquire)) {
        return;
    }

    const std::string status = yolo_reloading.load(std::memory_order_acquire)
        ? GetYoloReloadStatus()
        : GetDetectorStatus();
    if (status.empty()) {
        return;
    }

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImVec2 viewport = ImGui::GetIO().DisplaySize;
    const ImVec2 padding(18.0f, 12.0f);
    const ImVec2 text_size = ImGui::CalcTextSize(status.c_str());
    const ImVec2 box_size(text_size.x + padding.x * 2.0f, text_size.y + padding.y * 2.0f);
    const ImVec2 box_min((viewport.x - box_size.x) * 0.5f, 44.0f);
    const ImVec2 box_max(box_min.x + box_size.x, box_min.y + box_size.y);
    const ImU32 bg = detector_failed.load(std::memory_order_acquire)
        ? IM_COL32(120, 24, 32, 220)
        : IM_COL32(12, 54, 110, 220);
    const ImU32 border = IM_COL32(96, 168, 255, 230);

    draw->AddRectFilled(box_min, box_max, bg, 10.0f);
    draw->AddRect(box_min, box_max, border, 10.0f, 0, 1.5f);
    draw->AddText(ImVec2(box_min.x + padding.x, box_min.y + padding.y), IM_COL32(245, 250, 255, 255), status.c_str());
}

int AimAssistant::VkToHidButton(int vk) {
    switch (vk) {
        case VK_LBUTTON:  return 1;
        case VK_RBUTTON:  return 2;
        case VK_MBUTTON:  return 3;
        case VK_XBUTTON1: return 4;
        case VK_XBUTTON2: return 5;
        default:          return 0;
    }
}

bool AimAssistant::IsKeyDown(int vk) {
    return SysKeyHook::IsDown(vk);
}

cv::Point AimAssistant::getTargetPoint(const Detection* target) const {
    int base_x = target->box.x + target->box.width / 2;
    int base_y = target->box.y + static_cast<int>(target->box.height * cfg.target_y_ratio);

    // [修复] 同步反转：当前目标是偶数才代表打的是身体
    bool is_body = (target->class_id % 2 == 0);
    if (is_body) {
        for (const auto& det : detections) {
            // [修复] 画面里的其他目标是奇数才代表是头
            bool is_head = (det.class_id % 2 == 1);
            if (is_head) {
                int head_cx = det.box.x + det.box.width / 2;
                int head_cy = det.box.y + det.box.height / 2;

                // 判定这个头是否属于当前身体
                bool in_x = (head_cx > target->box.x - det.box.width) &&
                    (head_cx < target->box.x + target->box.width + det.box.width);
                bool in_y = (head_cy > target->box.y - det.box.height * 1.5f) &&
                    (head_cy < target->box.y + target->box.height * 0.6f);

                if (in_x && in_y) {
                    base_x = head_cx;
                    break;
                }
            }
        }
    }

    return cv::Point(base_x, base_y);
}

cv::Rect AimAssistant::getTriggerHitbox(const Detection& det) const {
    int hit_w = static_cast<int>(det.box.width * cfg.trigger_scale_x);
    int hit_h = static_cast<int>(det.box.height * cfg.trigger_scale_y);
    int hit_x = det.box.x + (det.box.width - hit_w) / 2;
    int hit_y = det.box.y + (det.box.height - hit_h) / 2;
    hit_y += static_cast<int>(det.box.height * cfg.trigger_offset_y);
    return cv::Rect(hit_x, hit_y, hit_w, hit_h);
}

bool AimAssistant::isTriggerTarget(const Detection& det) const {
    bool is_body = (det.class_id % 2 == 0);
    bool is_t = (det.class_id >= 2);
    if (cfg.target_team == 0 && is_t) return false;
    if (cfg.target_team == 1 && !is_t) return false;
    if (cfg.target_class_id == 0 && is_body) return false;
    if (cfg.target_class_id == 1 && !is_body) return false;
    return true;
}

bool AimAssistant::isAimingAtTriggerTarget() const {
    for (const auto& det : detections) {
        if (!isTriggerTarget(det)) continue;
        if (getTriggerHitbox(det).contains(crop_center)) {
            return true;
        }
    }
    return false;
}

void AimAssistant::UpdateClickability() {
    HWND hwnd = overlay.GetHwnd();
    bool clickable = cfg.show_menu || nn_collecting || nn_test_mode;
    overlay.SetMenuOpen(cfg.show_menu);
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);

    if (clickable) {
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        exStyle &= ~WS_EX_TRANSPARENT;
        exStyle &= ~WS_EX_NOACTIVATE;
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);

        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = false;
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

        if (cfg.show_menu)
            std::cout << "[INFO] Menu ON" << std::endl;
    }
    else {
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        exStyle |= WS_EX_TRANSPARENT;
        exStyle |= WS_EX_NOACTIVATE;    // passthrough mode for ESP
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);

        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);

        ImGui::GetIO().MouseDrawCursor = false;

        std::cout << "[INFO] Menu OFF - ESP overlay mode" << std::endl;
    }
}

void AimAssistant::Run() {
    auto last_time = std::chrono::high_resolution_clock::now();
    UpdateClickability();

    while (!exit_requested) {
        if (!overlay.PeekMessages()) break;
        mouse.PollCatch();  // drain HID async .catch events

        auto current_time = std::chrono::high_resolution_clock::now();
        double dt = std::chrono::duration<double>(current_time - last_time).count();
        last_time = current_time;

        auto capture_start = std::chrono::high_resolution_clock::now();
        if (!capturer.CaptureFrame(captured_frame, crop_region)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        auto capture_end = std::chrono::high_resolution_clock::now();
        timings.capture_ms = std::chrono::duration<double, std::milli>(capture_end - capture_start).count();

        JoinFinishedDetectorLoad();

        if (!yolo_reloading.load(std::memory_order_acquire) && yolo_reload_thread.joinable()) {
            yolo_reload_thread.join();
        }

        if (IsDetectorReady() &&
            !yolo_reloading.load(std::memory_order_acquire) &&
            yolo_reload_pending.load(std::memory_order_relaxed)) {
            bool expected = true;
            if (yolo_reload_pending.compare_exchange_strong(expected, false, std::memory_order_relaxed)) {
                std::wstring path = yolo_reload_path;
                yolo_reloading.store(true, std::memory_order_release);
                SetYoloReloadStatus("loading " + WideToUtf8(path) + "...");
                yolo_reload_thread = std::thread([this, path]() {
                    try {
                        if (detector) {
                            detector->Reload(path.c_str());
                        }
                        SetYoloReloadStatus("OK");
                    } catch (const std::exception& e) {
                        SetYoloReloadStatus(std::string("ERR: ") + e.what());
                    } catch (...) {
                        SetYoloReloadStatus("错误: 未知");
                    }
                    yolo_reloading.store(false, std::memory_order_release);
                });
            }
        }

        Detection* best_target = nullptr;
        if (IsDetectorReady() && !yolo_reloading.load(std::memory_order_acquire)) {
            detector->Detect(captured_frame, detections, cfg, timings);
            best_target = findBestTarget();
            handleMouseInput(best_target, dt);
            handleTriggerbot();
        } else {
            detections.clear();
            timings.inference_ms = 0.0;
        }
        mouse.UpdateClick();

        overlay.StartFrame();

        // INSERT toggle (keyboard)
        static bool insert_pressed = false;
        if (!nn_collecting && !nn_test_mode && SysKeyHook::IsDown(VK_INSERT)) {
            if (!insert_pressed) {
                cfg.show_menu = !cfg.show_menu;
                overlay.SetMenuOpen(cfg.show_menu);
                UpdateClickability();
            }
            insert_pressed = true;
        } else {
            insert_pressed = false;
        }

        // Trigger toggle key
        static bool toggle_was_pressed = false;
        bool toggle_is_pressed = IsKeyDown(cfg.triggerbot_toggle_key);
        if (toggle_is_pressed && !toggle_was_pressed) {
            cfg.trigger_enable = !cfg.trigger_enable;
        }
        toggle_was_pressed = toggle_is_pressed;

        DrawDetectorStatusOverlay();
        if (cfg.show_menu && !nn_collecting && !nn_test_mode) DrawUI();
        if (cfg.enable_esp) DrawESP(best_target);
        if (nn_collecting) DrawCollectOverlay();
        if (nn_test_mode) DrawTestOverlay();

        if (nn_collecting) {
            HandleCollectionInput();
        }
        if (nn_test_mode) {
            HandleTestInput();
        }

        auto render_start = std::chrono::high_resolution_clock::now();
        overlay.Render();
        auto render_end = std::chrono::high_resolution_clock::now();
        timings.render_ms = std::chrono::duration<double, std::milli>(render_end - render_start).count();

        static int auto_save_counter = 0;
        if (++auto_save_counter >= 120) {
            auto_save_counter = 0;
            cfg.Save();
        }

        // Safety: unlock cursor if collection ended abnormally
        if (!nn_collecting && nn_collect_cursor_locked) {
            ClipCursor(nullptr);
            while (ShowCursor(TRUE) < 0);
            nn_collect_cursor_locked = false;
        }

        nn_train_progress = nn_train_progress_atomic.load(std::memory_order_relaxed);

        if (cfg.enable_visualization) {
            handleVisualization(best_target);
            cv::waitKey(1);
        }

        auto loop_end = std::chrono::high_resolution_clock::now();
        double loop_ms = std::chrono::duration<double, std::milli>(loop_end - current_time).count();
        timings.full_loop_ms = loop_ms;
        if (loop_ms > 0.001) {
            double instant_fps = 1000.0 / loop_ms;
            timings.real_fps = timings.real_fps * 0.9 + instant_fps * 0.1;
        }
    }
}

Detection* AimAssistant::findBestTarget() {
    return target_selector.Select(detections, cfg, crop_center);
}

void AimAssistant::handleMouseInput(const Detection* target, double dt) {
    bool smooth_key_down = IsKeyDown(cfg.smooth_aim_key);
    aim_debug.has_target = target != nullptr;
    aim_debug.prediction_used = false;
    aim_debug.prediction_gated = false;
    aim_debug.target_switched = target_selector.WasTargetSwitched();
    aim_debug.crowd_count = target_selector.GetCrowdCount();
    aim_debug.crowded = aim_debug.crowd_count > 1;
    aim_debug.lock_frames = target_selector.GetLockFrames();
    aim_debug.stale_frame = capturer.LastFrameWasStale();
    aim_debug.stale_frames = capturer.GetConsecutiveStaleFrames();
    aim_debug.output_dx = 0;
    aim_debug.output_dy = 0;

    static auto last_nn_done = std::chrono::steady_clock::now();
    static bool nn_just_finished = false;

    {
        static bool f8_was = false;
        const bool f8_is = IsKeyDown(cfg.single_shot_key);
        if (f8_is && !f8_was && target) {
            bool nn_loaded = false;
            {
                std::lock_guard<std::mutex> lock(nn_mutex);
                nn_loaded = nn_model.IsModelLoaded();
            }
            if (cfg.nn_trajectory_enable && nn_loaded) {
                RunNNTrajectory(target);
            } else {
                const cv::Point tp = getTargetPoint(target);
                std::vector<cv::Point2f> path = TrajectoryGenerator::GenerateCubicBezier(
                    cv::Point2f(static_cast<float>(crop_center.x), static_cast<float>(crop_center.y)),
                    cv::Point2f(static_cast<float>(tp.x), static_cast<float>(tp.y)), 10);
                cv::Point2f cur(static_cast<float>(crop_center.x), static_cast<float>(crop_center.y));
                for (const auto& np : path) {
                    const float mdx = np.x - cur.x;
                    const float mdy = np.y - cur.y;
                    mouse.MoveRelative(
                        static_cast<int>(mdx * mouse_correction_factor),
                        static_cast<int>(mdy * mouse_correction_factor));
                    cur = np;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
        }
        f8_was = f8_is;
    }

    if (nn_path_active && !nn_path_single_shot && !smooth_key_down) {
        nn_path_active = false;
        nn_elapsed = 0.0f;
        nn_mouse_fractional_x = 0.0f;
        nn_mouse_fractional_y = 0.0f;
    }
    if (nn_path_active && !nn_path_single_shot && target_selector.WasTargetSwitched()) {
        nn_path_active = false;
        nn_elapsed = 0.0f;
        nn_mouse_fractional_x = 0.0f;
        nn_mouse_fractional_y = 0.0f;
    }

    if (nn_path_active) {
        const bool was_single_shot = nn_path_single_shot;
        StepNNTrajectory(dt);
        if (nn_path_active || was_single_shot) {
            return;
        }
        nn_just_finished = true;
        last_nn_done = std::chrono::steady_clock::now();
        target_smooth_init = false;
        vel_x = 0.0f;
        vel_y = 0.0f;
    }

    if (cfg.aim_deadlock) {
        static bool deadlock_done = false;
        if (!smooth_key_down) { deadlock_done = false; }
        if (smooth_key_down && !deadlock_done && target) {
            cv::Point tp = getTargetPoint(target);
            float ex = static_cast<float>(tp.x - crop_center.x);
            float ey = static_cast<float>(tp.y - crop_center.y);
            int mx = static_cast<int>(ex * static_cast<float>(mouse_correction_factor) * cfg.sensitivity);
            int my = static_cast<int>(ey * static_cast<float>(mouse_correction_factor) * cfg.sensitivity);
            if (mx != 0 || my != 0) mouse.MoveRelative(mx, my);
            deadlock_done = true;
            aim_controller.Reset();
            target_predictor.Reset();
        }
        return;
    }

    if (!smooth_key_down || !target) {
        aim_controller.Reset();
        if (!smooth_key_down) {
            target_selector.Reset();
        }
        target_predictor.Reset();
        target_smooth_init = false;
        vel_x = 0.0f;
        vel_y = 0.0f;
        nn_just_finished = false;
        return;
    }

    if (dt < 0.001) dt = 0.001;

    cv::Point raw = getTargetPoint(target);
    aim_debug.raw_point = raw;

    cv::Point aim_point = raw;
    const bool can_use_prediction =
        cfg.target_prediction_enable &&
        !target_selector.WasTargetSwitched() &&
        target_selector.GetLockFrames() >= cfg.target_lock_min_frames;
    if (can_use_prediction) {
        const float lead_scale = target_selector.GetCrowdCount() > 1
            ? cfg.target_crowd_prediction_scale
            : 1.0f;
        aim_point = target_predictor.Update(*target, raw, cfg, dt, lead_scale);
        aim_debug.prediction_used = aim_point != raw;
    } else {
        target_predictor.Reset();
        aim_debug.prediction_gated = cfg.target_prediction_enable;
    }
    aim_debug.aim_point = aim_point;

    bool nn_main_ready = false;
    {
        std::lock_guard<std::mutex> lock(nn_mutex);
        nn_main_ready = nn_model.IsModelLoaded();
    }
    const bool use_nn_main =
        cfg.aim_algorithm == AimController::AlgorithmNeuralNetwork && nn_main_ready;

    if (!use_nn_main) {
        nn_just_finished = false;
        auto output = aim_controller.Update(cfg, AimController::Input{
            aim_point,
            crop_center,
            target->box.width,
            dt,
            mouse_correction_factor
        });
        if (output) {
            aim_debug.output_dx = output->dx;
            aim_debug.output_dy = output->dy;
            mouse.MoveRelative(output->dx, output->dy);
        }
        return;
    }

    if (!target_smooth_init) {
        target_smoothed_x = static_cast<float>(aim_point.x);
        target_smoothed_y = static_cast<float>(aim_point.y);
        target_smooth_init = true;
        vel_x = 0.0f;
        vel_y = 0.0f;
    } else {
        float raw_dx = aim_point.x - target_smoothed_x;
        float raw_dy = aim_point.y - target_smoothed_y;
        float raw_speed = sqrtf(raw_dx * raw_dx + raw_dy * raw_dy);

        float adaptive = 0.08f + 0.60f * std::min(1.0f, raw_speed / 30.0f);
        float sx = cfg.aim_target_smooth * 0.4f + adaptive * 0.6f;

        target_smoothed_x = sx * aim_point.x + (1.0f - sx) * target_smoothed_x;
        target_smoothed_y = sx * aim_point.y + (1.0f - sx) * target_smoothed_y;
    }

    humanize_timer += static_cast<float>(dt);
    float hu_interval = 0.3f + 0.5f * cfg.aim_humanize;
    if (humanize_timer >= hu_interval) {
        humanize_timer = 0.0f;
        float range = cfg.aim_humanize * 6.0f;
        humanize_target_ox = (static_cast<float>(rand()) / RAND_MAX - 0.5f) * range;
        humanize_target_oy = (static_cast<float>(rand()) / RAND_MAX - 0.5f) * range;
    }
    float hu_speed = 1.0f / std::max(0.05f, hu_interval * 0.7f);
    humanize_ox += (humanize_target_ox - humanize_ox) * std::min(1.0f, static_cast<float>(dt) * hu_speed);
    humanize_oy += (humanize_target_oy - humanize_oy) * std::min(1.0f, static_cast<float>(dt) * hu_speed);

    float target_x = target_smoothed_x + humanize_ox;
    float target_y = target_smoothed_y + humanize_oy;

    float error_x = target_x - crop_center.x;
    float error_y = target_y - crop_center.y;
    float dist = sqrtf(error_x * error_x + error_y * error_y);

    float base_deadzone = cfg.aim_snap_enable ? static_cast<float>(cfg.aim_snap_range) : static_cast<float>(cfg.pid_deadzone);
    float deadzone = std::max(1.0f, std::min(base_deadzone, static_cast<float>(target->box.width) * 0.1f));

    if (dist <= deadzone) {
        vel_x = 0.0f;
        vel_y = 0.0f;
        aim_controller.Reset();
        return;
    }

    const float restart_threshold = std::max(deadzone * 3.0f, cfg.max_lock_distance_pixels * 0.25f);
    if (dist > restart_threshold) {
        auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_nn_done).count();
        if (!nn_just_finished || since > 200) {
            nn_just_finished = false;
            if (BeginNNTrajectory(error_x, error_y, false)) {
                StepNNTrajectory(dt);
                return;
            }
        }
    }

    auto output = aim_controller.Update(cfg, AimController::Input{
        aim_point,
        crop_center,
        target->box.width,
        dt,
        mouse_correction_factor
    });
    if (output) {
        aim_debug.output_dx = output->dx;
        aim_debug.output_dy = output->dy;
        mouse.MoveRelative(output->dx, output->dy);
    }
}

void AimAssistant::handleTriggerbot() {
    if (!cfg.trigger_enable) {
        trigger_pending = false;
        return;
    }
    if (mouse.IsClickPending()) return;

    auto now = std::chrono::steady_clock::now();
    auto time_since_last_shot = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_shot_time).count();

    if (trigger_pending) {
        if (now < trigger_fire_time) return;
        trigger_pending = false;
        if (time_since_last_shot < cfg.trigger_fire_rate_ms) return;
        if (!isAimingAtTriggerTarget()) return;
        mouse.ClickLeft(cfg.trigger_click_duration_ms);
        last_shot_time = std::chrono::steady_clock::now();
        return;
    }

    if (time_since_last_shot < cfg.trigger_fire_rate_ms) return;
    if (!isAimingAtTriggerTarget()) return;

    if (cfg.trigger_delay_ms > 0) {
        trigger_pending = true;
        trigger_fire_time = now + std::chrono::milliseconds(cfg.trigger_delay_ms);
        return;
    }

    mouse.ClickLeft(cfg.trigger_click_duration_ms);
    last_shot_time = std::chrono::steady_clock::now();
}

void AimAssistant::handleVisualization(const Detection* best_target) {
    cv::circle(captured_frame, crop_center, static_cast<int>(cfg.max_lock_distance_pixels), cv::Scalar(255, 255, 0), 1);

    for (const auto& det : detections) {
        cv::Scalar color = (&det == best_target) ? cv::Scalar(0, 0, 255)
            : (det.class_id >= 2) ? cv::Scalar(0, 165, 255) : cv::Scalar(255, 0, 0);
        cv::rectangle(captured_frame, det.box, color, 2);

        std::string label = "ID:" + std::to_string(det.class_id) + " " + cv::format("%.2f", det.confidence);
        cv::putText(captured_frame, label, cv::Point(det.box.x, det.box.y - 5), cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1);

        if (cfg.trigger_enable) {
            cv::Rect hitbox = getTriggerHitbox(det);
            cv::rectangle(captured_frame, hitbox, cv::Scalar(255, 255, 0), 1);
        }
    }

    if (timings.total_loop_ms > 0) {
        double fps = 1000.0 / timings.total_loop_ms;
        std::string stats = cv::format("FPS: %.1f", fps);
        cv::putText(captured_frame, stats, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 1, cv::Scalar(0, 255, 255), 2);
    }

    std::string tb_status = cfg.trigger_enable ? "Trigger: ON" : "Trigger: OFF";
    cv::Scalar tb_color = cfg.trigger_enable ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);
    cv::putText(captured_frame, tb_status, cv::Point(10, 60), cv::FONT_HERSHEY_SIMPLEX, 0.7, tb_color, 2);

    cv::imshow(cfg.window_name, captured_frame);
}

void AimAssistant::DrawUI() {
    ImGui::SetNextWindowSize(ImVec2(600, 720), ImGuiCond_FirstUseEver);

    bool was_open = cfg.show_menu;
    if (!ImGui::Begin("LozeeAim", &cfg.show_menu)) {
        ImGui::End();
        if (was_open && !cfg.show_menu) {
            overlay.menu_open = false;
            UpdateClickability();
        }
        return;
    }

    DrawTopBar();
    ImGui::Separator();

    if (ImGui::BeginTabBar("MainTabs")) {
        if (ImGui::BeginTabItem(UiText(cfg, " 总览 ", " Overview "))) {
            DrawOverviewTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(UiText(cfg, " 瞄准 ", " Aim "))) {
            DrawAimTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(UiText(cfg, " 自动扳机 ", " Trigger "))) {
            DrawTriggerTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(UiText(cfg, " 视觉 ", " Visual "))) {
            DrawVisualTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(UiText(cfg, " 模型 ", " Model "))) {
            DrawModelTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(UiText(cfg, " NN轨迹 ", " NN Path "))) {
            DrawTrainingTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(UiText(cfg, " 关于 ", " About "))) {
            DrawAboutTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (ImGui::Button(UiText(cfg, "退出程序", "Exit"), ImVec2(-1, 0))) {
        cfg.Save();
        exit_requested = true;
    }
    ImGui::End();
}

void AimAssistant::DrawTopBar() {
    ImGui::TextColored(ImVec4(0, 1, 0, 1), "FPS: %.1f", timings.real_fps);
    ImGui::SameLine();
    ImGui::Text("| %s: %.1f ms", UiText(cfg, "延迟", "Latency"), timings.full_loop_ms);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", UiText(cfg, "按住标题栏可拖动", "Drag the title bar to move"));
    DrawHelpTooltip(UiText(cfg, "鼠标按住标题栏可拖动窗口", "Hold the title bar to move the window."));

    ImGui::TextDisabled(
        "%s %.1f | %s %.1f | %s %.1f | %s %.1f | %s %.1f ms%s",
        UiText(cfg, "采集", "Capture"),
        timings.capture_ms,
        UiText(cfg, "预处理", "Preprocess"),
        timings.preprocess_ms,
        UiText(cfg, "推理", "Inference"),
        timings.inference_ms,
        UiText(cfg, "后处理", "Postprocess"),
        timings.postprocess_ms,
        UiText(cfg, "渲染", "Render"),
        timings.render_ms,
        capturer.LastFrameWasStale() ? UiText(cfg, " | 缓存帧", " | stale frame") : "");
    DrawHelpTooltip(UiText(cfg, "用于定位全屏窗口模式下的卡顿来源。", "Use this to locate frame-time bottlenecks."));

    static std::string dependency_status;
    if (ImGui::Button(UiText(cfg, "依赖管理", "Dependency Manager"), ImVec2(150, 0))) {
        dependency_status = DependencyInstaller::OpenManagerProcess()
            ? UiText(cfg, "已打开", "Opened")
            : UiText(cfg, "打开失败", "Open failed");
    }
    if (!dependency_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", dependency_status.c_str());
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    const char* language_items[] = { "中文", "English" };
    ImGui::Combo(UiText(cfg, "语言##top_language", "Language##top_language"), &cfg.ui_language, language_items, IM_ARRAYSIZE(language_items));
}

void AimAssistant::DrawOverviewTab() {
    const ImVec2 child_size(0.0f, -ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y * 2.0f);
    if (!ImGui::BeginChild("OverviewScroll", child_size, false, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        ImGui::EndChild();
        return;
    }

    DrawSectionHeader(UiText(cfg, "运行状态", "Runtime Status"));
    if (ImGui::BeginTable("OverviewStatus", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn(UiText(cfg, "项目", "Item"), ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn(UiText(cfg, "状态", "Status"));
        ImGui::TableHeadersRow();

        auto row = [&](const char* label, const char* value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", value);
        };

        row(UiText(cfg, "检测器", "Detector"), GetDetectorStatus().c_str());
        row(UiText(cfg, "捕获帧", "Capture frame"), capturer.LastFrameWasStale() ? UiText(cfg, "缓存帧", "Stale") : UiText(cfg, "实时", "Live"));
        row(UiText(cfg, "鼠标后端", "Mouse backend"), "Win32API / SendInput");
        if (cfg.mouse_backend == 1) {
            row(UiText(cfg, "HID 状态", "HID status"), mouse.IsHidConnected() ? UiText(cfg, "已连接", "Connected") : mouse.GetHidStatus());
        }
        row(UiText(cfg, "防截屏", "Screen capture protection"), cfg.prevent_screen_capture ? UiText(cfg, "已开启", "Enabled") : UiText(cfg, "已关闭", "Disabled"));
        ImGui::EndTable();
    }

    DrawSectionHeader(UiText(cfg, "界面设置", "Interface"));
    const char* language_items[] = { "中文", "English" };
    ImGui::Combo(UiText(cfg, "语言##overview_language", "Language##overview_language"), &cfg.ui_language, language_items, IM_ARRAYSIZE(language_items));
    ImGui::TextDisabled("%s", UiText(cfg, "语言设置会保存到 global.ini。", "Language setting is saved to global.ini."));

    DrawSectionHeader(UiText(cfg, "快捷入口", "Shortcuts"));
    ImGui::TextDisabled("%s", UiText(cfg, "常用调参在“瞄准”和“视觉”；模型加载和推理后端在“模型”；数据采集和训练在“NN轨迹”。", "Aim and Visual contain daily tuning; Model contains inference settings; NN Path contains data collection and training."));

    ImGui::EndChild();
}

void AimAssistant::DrawAimTab() {
    const ImVec2 child_size(0.0f, -ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y * 2.0f);
    if (!ImGui::BeginChild("AimScroll", child_size, false, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        ImGui::EndChild();
        return;
    }

    DrawSectionHeader(UiText(cfg, "基础参数", "Basic"));
    ImGui::SliderFloat(UiText(cfg, "锁定范围 (FOV)", "Lock range (FOV)"), &cfg.max_lock_distance_pixels, 10, 640, "%.0f px");
    ImGui::SliderFloat(UiText(cfg, "平滑时间 (s)", "Smooth time (s)"), &cfg.aim_smooth_time, 0.01f, 0.5f, "%.2f");
    DrawHelpTooltip(UiText(cfg, "越小越快, 0.05=快 0.15=平滑", "Lower is faster; 0.05 is fast, 0.15 is smoother."));

    bool nn_model_loaded = false;
    {
        std::lock_guard<std::mutex> lock(nn_mutex);
        nn_model_loaded = nn_model.IsModelLoaded();
    }

    const char* aim_algorithm_items_zh[] = { "自适应阻尼", "线性响应", "弹簧阻尼", "神经网络" };
    const char* aim_algorithm_items_en[] = { "Adaptive damping", "Linear response", "Spring damping", "Neural network" };
    const char* aim_algorithm_hints_zh[] = {
        "默认平衡方案，结合目标平滑和加速度，稳定性较好。",
        "直接比例响应，延迟更低，但对检测噪声更敏感。",
        "二阶阻尼响应，起步更干脆，靠近目标时收敛更顺。",
        "使用已加载的 NN 轨迹模型主瞄准，再回退到阻尼微调。"
    };
    const char* aim_algorithm_hints_en[] = {
        "Balanced default with target smoothing and acceleration.",
        "Direct proportional response with lower latency and more noise sensitivity.",
        "Second-order damping with crisp start and smooth convergence.",
        "Use the loaded NN path model for main aim, then damped fine tuning."
    };
    const int aim_algorithm_count = nn_model_loaded ? 4 : AimController::AlgorithmNeuralNetwork;
    if (cfg.aim_algorithm < 0 || cfg.aim_algorithm >= aim_algorithm_count) {
        cfg.aim_algorithm = AimController::AlgorithmAdaptive;
    }
    ImGui::Combo(
        UiText(cfg, "瞄准算法", "Aim algorithm"),
        &cfg.aim_algorithm,
        UiEnglish(cfg) ? aim_algorithm_items_en : aim_algorithm_items_zh,
        aim_algorithm_count);
    cfg.nn_main_aim_enable = nn_model_loaded && cfg.aim_algorithm == AimController::AlgorithmNeuralNetwork;
    ImGui::TextDisabled("%s", UiEnglish(cfg) ? aim_algorithm_hints_en[cfg.aim_algorithm] : aim_algorithm_hints_zh[cfg.aim_algorithm]);
    if (!nn_model_loaded) {
        ImGui::TextDisabled("%s", UiText(cfg, "加载 NN 轨迹模型后可启用神经网络算法。", "Load an NN path model to enable the neural network algorithm."));
    }

    ImGui::Checkbox(UiText(cfg, "近距离锁死", "Near-target stop zone"), &cfg.aim_snap_enable);
    ImGui::SameLine();
    ImGui::SliderInt(UiText(cfg, "锁死范围", "Stop zone"), &cfg.aim_snap_range, 2, 20, "%d px");
    DrawHelpTooltip(UiText(cfg, "距离小于此值则停止移动, 消除转圈", "Stop moving inside this range to avoid circling."));
    ImGui::Checkbox(UiText(cfg, "暴力锁头 (无轨迹)", "Instant snap (no path)"), &cfg.aim_deadlock);
    DrawHelpTooltip(UiText(cfg, "不经过平滑, 直接移动到目标位置", "Move directly to the target without smoothing."));

    cfg.mouse_backend = 0;
    ImGui::Text("%s: Win32API / SendInput", UiText(cfg, "鼠标后端", "Mouse backend"));
    ImGui::TextColored(ImVec4(1, 0.6f, 0, 1), "%s", UiText(cfg, "开源版本仅保留 SendInput 后端。", "The open-source edition only keeps the SendInput backend."));
    const char* teams[] = { "CT", "T", UiText(cfg, "全部", "All") };
    ImGui::Combo(UiText(cfg, "阵营", "Team"), &cfg.target_team, teams, IM_ARRAYSIZE(teams));
    const char* parts_zh[] = { "头部", "身体" };
    const char* parts_en[] = { "Head", "Body" };
    int target_part = cfg.target_class_id;
    if (ImGui::Combo(UiText(cfg, "瞄准部位", "Aim part"), &target_part, UiEnglish(cfg) ? parts_en : parts_zh, 2)) {
        cfg.target_class_id = target_part;
        cfg.target_y_ratio = (target_part == 0) ? 0.15f : 0.5f;
    }
    ImGui::SliderFloat(UiText(cfg, "高度微调", "Vertical offset"), &cfg.target_y_ratio, 0.0f, 1.0f, "%.2f");

    DrawSectionHeader(UiText(cfg, "自瞄热键", "Aim Hotkey"));
    {
        static bool aim_waiting = false;
        static bool aim_waiting_release = false;

        auto get_key_name = [&](int vk) -> std::string {
            if (vk == 0) return UiText(cfg, "无", "None");
            switch (vk) {
            case VK_LBUTTON:  return UiText(cfg, "鼠标左键", "Mouse left");
            case VK_RBUTTON:  return UiText(cfg, "鼠标右键", "Mouse right");
            case VK_MBUTTON:  return UiText(cfg, "鼠标中键", "Mouse middle");
            case VK_XBUTTON1: return UiText(cfg, "鼠标侧键1", "Mouse side 1");
            case VK_XBUTTON2: return UiText(cfg, "鼠标侧键2", "Mouse side 2");
            case VK_SHIFT:    return "Shift";
            case VK_CONTROL:  return "Ctrl";
            case VK_MENU:     return "Alt";
            case VK_SPACE:    return UiText(cfg, "空格", "Space");
            }
            if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
            if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
            if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
            return std::string(UiText(cfg, "键码:", "VK:")) + std::to_string(vk);
        };

        if (aim_waiting) {
            if (aim_waiting_release) {
                bool any_down = false;
                for (int vk = 0x01; vk <= 0xFF; vk++) {
                    if (GetAsyncKeyState(vk) & 0x8000) { any_down = true; break; }
                }
                if (!any_down) aim_waiting_release = false;
                ImGui::TextDisabled("%s", UiText(cfg, "请松开所有按键...", "Release all keys..."));
            } else {
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "%s", UiText(cfg, "按下自瞄热键... (ESC=取消)", "Press aim hotkey... (ESC=cancel)"));
                for (int vk = 0x01; vk <= 0xFF; vk++) {
                    if (GetAsyncKeyState(vk) & 0x8000) {
                        if (vk != VK_ESCAPE) cfg.smooth_aim_key = vk;
                        aim_waiting = false;
                        break;
                    }
                }
            }
        } else {
            ImGui::Text("%s", UiText(cfg, "瞄准键:", "Aim key:"));
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "%s", get_key_name(cfg.smooth_aim_key).c_str());
            if (ImGui::Button(UiText(cfg, "设置热键", "Set hotkey"), ImVec2(100, 0))) {
                aim_waiting = true;
                aim_waiting_release = true;
            }
        }
    }

    DrawSectionHeader(UiText(cfg, "平滑参数", "Smoothing"));
    ImGui::SliderFloat(UiText(cfg, "目标平滑", "Target smoothing"), &cfg.aim_target_smooth, 0.05f, 1.0f, "%.2f");
    DrawHelpTooltip(UiText(cfg, "越低越防抖, 但会略慢", "Lower values reduce jitter but respond slower."));
    ImGui::SliderFloat(UiText(cfg, "速度曲线", "Speed curve"), &cfg.aim_curve, 0.3f, 2.0f, "%.2f");
    DrawHelpTooltip(UiText(cfg, "<1 更激进, 1=线性, >1 更温和", "<1 aggressive, 1=linear, >1 gentle."));
    ImGui::SliderFloat(UiText(cfg, "加速度平滑", "Acceleration smoothing"), &cfg.aim_accel, 0.0f, 1.0f, "%.2f");
    DrawHelpTooltip(UiText(cfg, "0=瞬时响应, 1=最大平滑加速", "0=instant response, 1=max acceleration smoothing."));
    ImGui::SliderFloat(UiText(cfg, "拟人化", "Humanization"), &cfg.aim_humanize, 0.0f, 1.0f, "%.2f");
    DrawHelpTooltip(UiText(cfg, "瞄准点轻微抖动模拟人手", "Adds a small hand-like aim offset."));

    DrawSectionHeader(UiText(cfg, "目标预测", "Target Prediction"));
    ImGui::Checkbox(UiText(cfg, "启用目标预测", "Enable prediction"), &cfg.target_prediction_enable);
    ImGui::SliderFloat(UiText(cfg, "预测提前量", "Lead time"), &cfg.target_prediction_lead_ms, 0.0f, 120.0f, "%.0f ms");
    ImGui::SliderFloat(UiText(cfg, "速度平滑", "Velocity smoothing"), &cfg.target_prediction_smooth, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat(UiText(cfg, "最大预测偏移", "Max prediction offset"), &cfg.target_prediction_max_offset, 0.0f, 80.0f, "%.0f px");
    ImGui::SliderInt(UiText(cfg, "稳定锁定帧数", "Stable lock frames"), &cfg.target_lock_min_frames, 1, 10);
    ImGui::SliderInt(UiText(cfg, "切换确认帧数", "Switch confirm frames"), &cfg.target_switch_confirm_frames, 1, 10);
    ImGui::SliderFloat(UiText(cfg, "切换分数门槛", "Switch score margin"), &cfg.target_switch_margin, 0.0f, 180.0f, "%.0f");
    ImGui::SliderFloat(UiText(cfg, "多人预测缩放", "Crowd prediction scale"), &cfg.target_crowd_prediction_scale, 0.0f, 1.0f, "%.2f");

    DrawSectionHeader(UiText(cfg, "动态步长限制", "Dynamic Step Limit"));
    ImGui::SliderInt(UiText(cfg, "远距最大步长", "Far max step"), &cfg.aim_max_step_far, 60, 500);
    ImGui::SliderInt(UiText(cfg, "近距最大步长", "Near max step"), &cfg.aim_max_step_near, 10, 120);
    ImGui::SliderFloat(UiText(cfg, "远近分界", "Far/near threshold"), &cfg.aim_far_threshold, 30.0f, 250.0f, "%.0f px");
    DrawHelpTooltip(UiText(cfg, "大于此距离用远距步长, 小于则线性插值到近距步长", "Use far step beyond this distance, then interpolate to near step."));
    ImGui::SliderInt(UiText(cfg, "死区 (像素)", "Deadzone (pixels)"), &cfg.pid_deadzone, 1, 15, "%d px");

    ImGui::EndChild();
}

void AimAssistant::DrawTriggerTab() {
    const ImVec2 child_size(0.0f, -ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y * 2.0f);
    if (!ImGui::BeginChild("TriggerScroll", child_size, false, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        ImGui::EndChild();
        return;
    }

    bool is_on = cfg.trigger_enable;
    if (is_on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0.7f, 0, 1));
        if (ImGui::Button(UiText(cfg, "状态: 已开启 (F7)", "Status: Enabled (F7)"), ImVec2(-1, 34))) cfg.trigger_enable = false;
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0, 0, 1));
        if (ImGui::Button(UiText(cfg, "状态: 已关闭 (F7)", "Status: Disabled (F7)"), ImVec2(-1, 34))) cfg.trigger_enable = true;
        ImGui::PopStyleColor();
    }

    DrawSectionHeader(UiText(cfg, "基础状态", "Status"));
    ImGui::TextDisabled("%s: %s", UiText(cfg, "鼠标后端", "Mouse backend"), "Win32API / SendInput");

    DrawSectionHeader(UiText(cfg, "触发判定范围", "Trigger Hitbox"));
    ImGui::SliderFloat(UiText(cfg, "水平缩放", "Horizontal scale"), &cfg.trigger_scale_x, 0.1f, 1.0f, "%.2f");
    ImGui::SliderFloat(UiText(cfg, "垂直缩放", "Vertical scale"), &cfg.trigger_scale_y, 0.1f, 1.0f, "%.2f");
    ImGui::SliderFloat(UiText(cfg, "垂直偏移", "Vertical offset"), &cfg.trigger_offset_y, -0.5f, 0.5f, "%.2f");

    DrawSectionHeader(UiText(cfg, "时间控制", "Timing"));
    ImGui::SliderInt(UiText(cfg, "反应延迟", "Reaction delay"), &cfg.trigger_delay_ms, 0, 200, "%d ms");
    ImGui::SliderInt(UiText(cfg, "射击间隔", "Fire interval"), &cfg.trigger_fire_rate_ms, 50, 500, "%d ms");
    ImGui::SliderInt(UiText(cfg, "按键时长", "Click duration"), &cfg.trigger_click_duration_ms, 10, 200, "%d ms");

    ImGui::EndChild();
}

void AimAssistant::DrawVisualTab() {
    const ImVec2 child_size(0.0f, -ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y * 2.0f);
    if (!ImGui::BeginChild("VisualScroll", child_size, false, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        ImGui::EndChild();
        return;
    }

    DrawSectionHeader(UiText(cfg, "覆盖层", "Overlay"));
    if (ImGui::BeginTable("VisualToggles", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn(); ImGui::Checkbox(UiText(cfg, "启用 ESP", "Enable ESP"), &cfg.enable_esp);
        ImGui::TableNextColumn(); ImGui::Checkbox(UiText(cfg, "绘制方框", "Draw boxes"), &cfg.esp_draw_boxes);
        ImGui::TableNextColumn(); ImGui::Checkbox(UiText(cfg, "绘制队友", "Draw teammates"), &cfg.esp_draw_teammates);
        ImGui::TableNextColumn(); ImGui::Checkbox(UiText(cfg, "FOV 圆圈", "FOV circle"), &cfg.show_fov_circle);
        ImGui::TableNextColumn(); ImGui::Checkbox(UiText(cfg, "显示百分比", "Show confidence"), &cfg.esp_show_confidence);
        ImGui::TableNextColumn(); ImGui::Checkbox(UiText(cfg, "OpenCV 调试窗口", "OpenCV debug window"), &cfg.enable_visualization);
        ImGui::EndTable();
    }

    DrawSectionHeader(UiText(cfg, "瞄准诊断", "Aim Diagnostics"));
    if (ImGui::BeginTable("DiagnosticToggles", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn(); ImGui::Checkbox(UiText(cfg, "显示瞄准诊断", "Show aim diagnostics"), &cfg.aim_diagnostics_enable);
        ImGui::TableNextColumn(); ImGui::Checkbox(UiText(cfg, "显示诊断文字", "Show debug text"), &cfg.aim_diagnostics_text_enable);
        ImGui::TableNextColumn(); ImGui::Checkbox(UiText(cfg, "显示预测点", "Show prediction point"), &cfg.aim_diagnostics_points_enable);
        ImGui::EndTable();
    }

    DrawSectionHeader(UiText(cfg, "隐私", "Privacy"));
    if (ImGui::Checkbox(UiText(cfg, "防截屏", "Screen capture protection"), &cfg.prevent_screen_capture)) {
        if (!overlay.SetCaptureProtection(cfg.prevent_screen_capture)) {
            cfg.prevent_screen_capture = overlay.IsCaptureProtectionEnabled();
        }
    }
    DrawHelpTooltip(UiText(cfg, "让系统截图/录屏忽略覆盖层窗口；对驱动层捕获不保证有效。", "Ask Windows capture APIs to exclude the overlay; driver-level capture may still see it."));

    DrawSectionHeader(UiText(cfg, "FOV 圈颜色", "FOV Circle Color"));
    ImGui::ColorEdit4("##fovcolor", cfg.fov_circle_color, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_Float);

    ImGui::EndChild();
}

void AimAssistant::DrawModelTab() {
    const ImVec2 child_size(0.0f, -ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y * 2.0f);
    if (!ImGui::BeginChild("ModelScroll", child_size, false, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        ImGui::EndChild();
        return;
    }

    DrawSectionHeader(UiText(cfg, "识别参数", "Detection"));
    ImGui::SliderFloat(UiText(cfg, "置信度阈值", "Confidence threshold"), &cfg.confidence_threshold, 0.1f, 0.9f, "%.2f");
    ImGui::SliderFloat(UiText(cfg, "NMS 阈值", "NMS threshold"), &cfg.nms_threshold, 0.1f, 0.8f, "%.2f");

    DrawSectionHeader(UiText(cfg, "YOLO模型", "YOLO Model"));
    static std::string yolo_current;
    static std::vector<std::string> yolo_models;
    if (!yolo_models.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1, 1), "%s", yolo_current.empty() ? "" : yolo_current.c_str());
    }
    static bool yolo_scanned = false;
    static int yolo_sel = -1;
    if (!yolo_scanned) {
        yolo_scanned = true;
        yolo_models.clear();
        for (const auto& name : ScanModelFiles(model_paths::YoloDirA(), { "*.onnx" })) {
            if (name.find("nn_model") == std::string::npos) {
                yolo_models.push_back(model_paths::YoloConfigPath(name));
            }
        }
        if (yolo_sel < 0 && cfg.yolo_model_paths[0] != '\0') {
            for (int i = 0; i < (int)yolo_models.size(); i++) {
                if (yolo_models[i] == cfg.yolo_model_paths ||
                    model_paths::BaseNameA(yolo_models[i]) == model_paths::BaseNameA(cfg.yolo_model_paths)) {
                    yolo_sel = i;
                    yolo_current = model_paths::BaseNameA(yolo_models[i]);
                    break;
                }
            }
        }
    }
    if (!yolo_models.empty()) {
        std::string yolo_preview = (yolo_sel >= 0 && yolo_sel < (int)yolo_models.size())
            ? model_paths::BaseNameA(yolo_models[yolo_sel]) : UiText(cfg, "选择...", "Select...");
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("##yolo_combo", yolo_preview.c_str())) {
            for (int i = 0; i < (int)yolo_models.size(); i++) {
                const std::string display_name = model_paths::BaseNameA(yolo_models[i]);
                if (ImGui::Selectable(display_name.c_str(), yolo_sel == i)) {
                    yolo_sel = i;
                    yolo_current = display_name;
                    cfg.yolo_model_idx = i;
                    strncpy_s(cfg.yolo_model_paths, yolo_models[i].c_str(), sizeof(cfg.yolo_model_paths) - 1);
                    const std::string resolved = model_paths::ResolveExistingPathA(yolo_models[i]);
                    yolo_reload_path = Utf8ToWide(resolved.empty() ? yolo_models[i] : resolved);
                    yolo_reload_pending.store(true, std::memory_order_release);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(UiText(cfg, "刷新##yolo", "Refresh##yolo"), ImVec2(80, 0))) {
            yolo_models.clear();
            yolo_scanned = false;
            yolo_sel = -1;
        }
        const std::string yolo_status = GetYoloReloadStatus();
        if (yolo_reloading.load(std::memory_order_acquire)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 1, 0, 1), " %s", yolo_status.c_str());
        } else if (!yolo_status.empty()) {
            ImGui::SameLine();
            bool ok = (yolo_status == "OK");
            ImGui::TextColored(ok ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0.3f, 0.3f, 1), " %s", yolo_status.c_str());
        }
    } else {
        ImGui::TextDisabled("%s", UiText(cfg, "未找到 YOLO ONNX 模型。", "No YOLO ONNX model found."));
    }

    DrawSectionHeader(UiText(cfg, "推理后端", "Inference Provider"));
    const char* providers_zh[] = { "DirectML (通用GPU)", "TensorRT (NVIDIA)", "CPU" };
    const char* providers_en[] = { "DirectML (generic GPU)", "TensorRT (NVIDIA)", "CPU" };
    int provider_selection = cfg.use_cpu_inference ? 2 : cfg.inference_provider;
    if (provider_selection < 0 || provider_selection > 2) provider_selection = 0;
    if (ImGui::Combo(UiText(cfg, "推理后端", "Inference provider"), &provider_selection, UiEnglish(cfg) ? providers_en : providers_zh, 3)) {
        cfg.use_cpu_inference = (provider_selection == 2);
        cfg.inference_provider = cfg.use_cpu_inference ? 0 : provider_selection;
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "%s", UiText(cfg, "需要重启生效，请使用依赖管理确认后端依赖", "Restart required; use Dependency Manager to verify runtime dependencies."));
    }

    ImGui::EndChild();
}

void AimAssistant::DrawAboutTab() {
    const ImVec2 child_size(0.0f, -ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y * 2.0f);
    if (!ImGui::BeginChild("AboutScroll", child_size, false, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        ImGui::EndChild();
        return;
    }

    DrawSectionHeader("LozeeAim");
    ImGui::TextWrapped("%s", UiText(cfg,
        "Windows C++20 计算机视觉叠加层与本地自动化研究项目。",
        "Windows C++20 computer-vision overlay and local automation research project."));
    ImGui::BulletText("%s", UiText(cfg, "项目代码许可证：MIT License", "Project code license: MIT License"));
    ImGui::BulletText("%s", UiText(cfg, "项目用途：授权测试、本地实验和研究环境", "Intended use: authorized testing, local experiments, and research environments"));
    ImGui::BulletText("%s", UiText(cfg, "鼠标后端：Win32 SendInput", "Mouse backend: Win32 SendInput"));
    ImGui::Spacing();

    DrawSectionHeader(UiText(cfg, "版权信息", "Copyright"));
    ImGui::BulletText("Copyright (c) 2026 Xiaoluozhi.");
    ImGui::BulletText("%s", UiText(cfg,
        "除非文件另有说明，本仓库源码按 MIT License 授权。",
        "Unless a file states otherwise, source code in this repository is licensed under the MIT License."));
    ImGui::BulletText("%s", UiText(cfg,
        "第三方组件保留其各自许可证和分发条款。",
        "Third-party components keep their own licenses and distribution terms."));

    DrawSectionHeader(UiText(cfg, "单文件发布", "Single-executable Distribution"));
    ImGui::TextWrapped("%s", UiText(cfg,
        "如果发布为单个主可执行文件，请确保第三方许可证、版权声明和模型授权信息仍然可被用户访问。",
        "If distributed as one main executable, keep third-party licenses, copyright notices, and model authorization information available to users."));
    ImGui::TextWrapped("%s", UiText(cfg,
        "模型文件和 GPU/运行时二进制文件可能仍需保持外置，除非你拥有相应的再分发权利。",
        "Model files and GPU/runtime binaries may still need to remain external unless you have redistribution rights."));

    DrawSectionHeader(UiText(cfg, "第三方组件", "Third-party Components"));
    ImGui::BulletText("Dear ImGui - MIT License - https://github.com/ocornut/imgui");
    ImGui::BulletText("OpenCV - Apache License 2.0 - https://github.com/opencv/opencv");
    ImGui::BulletText("ONNX Runtime - MIT License - https://github.com/microsoft/onnxruntime");
    ImGui::BulletText("DirectML - Microsoft package license terms");
    ImGui::BulletText("CUDA / cuDNN / TensorRT - NVIDIA license terms");
    ImGui::BulletText("Windows SDK / DirectX 11 - Microsoft SDK terms");

    DrawSectionHeader(UiText(cfg, "文档", "Documents"));
    ImGui::BulletText("README.md / README.zh-CN.md");
    ImGui::BulletText("LICENSE");
    ImGui::BulletText("SECURITY.md / SECURITY.zh-CN.md");
    ImGui::BulletText("THIRD_PARTY_NOTICES.md / THIRD_PARTY_NOTICES.zh-CN.md");

    ImGui::EndChild();
}

void AimAssistant::DrawESP(const Detection* best_target) {
    auto draw_list = ImGui::GetBackgroundDrawList();

    float screen_cx = overlay.GetWidth() / 2.0f;
    float screen_cy = overlay.GetHeight() / 2.0f;

    if (cfg.show_fov_circle) {
        bool on_target = false;
        if (cfg.trigger_enable) {
            for (const auto& det : detections) {
                if (getTriggerHitbox(det).contains(crop_center)) { on_target = true; break; }
            }
        }
        ImU32 fov_color = on_target
            ? IM_COL32(255, 30, 30, 180)
            : IM_COL32((int)(cfg.fov_circle_color[0] * 255), (int)(cfg.fov_circle_color[1] * 255), (int)(cfg.fov_circle_color[2] * 255), (int)(cfg.fov_circle_color[3] * 255));
        draw_list->AddCircle(ImVec2(screen_cx, screen_cy), (float)cfg.max_lock_distance_pixels, fov_color);
    }

    int crop_x = (overlay.GetWidth() - cfg.crop_size) / 2;
    int crop_y = (overlay.GetHeight() - cfg.crop_size) / 2;

    for (const auto& det : detections) {
        // ESP teammate filter: skip teammates if drawing them is off
        bool is_t = (det.class_id >= 2);
        bool is_teammate = (cfg.target_team == 0 && is_t) || (cfg.target_team == 1 && !is_t);
        if (is_teammate && !cfg.esp_draw_teammates && &det != best_target) continue;

        float x = static_cast<float>(crop_x + det.box.x);
        float y = static_cast<float>(crop_y + det.box.y);
        float w = static_cast<float>(det.box.width);
        float h = static_cast<float>(det.box.height);

        ImU32 color;
        if (&det == best_target) {
            color = IM_COL32(255, 0, 0, 255);       // red = locked target
        } else if (is_t) {
            color = IM_COL32(255, 180, 30, 255);     // orange = T
        } else {
            color = IM_COL32(50, 150, 255, 255);     // blue = CT
        }

        if (cfg.esp_draw_boxes)
            draw_list->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), color, 0.0f, 0, 2.0f);

        if (cfg.trigger_enable) {
            cv::Rect hitbox = getTriggerHitbox(det);
            float hx = x + (hitbox.x - det.box.x);
            float hy = y + (hitbox.y - det.box.y);
            draw_list->AddRect(ImVec2(hx, hy), ImVec2(hx + hitbox.width, hy + hitbox.height), IM_COL32(0, 255, 255, 150));
        }

        if (cfg.esp_show_confidence) {
            std::string label = std::to_string((int)(det.confidence * 100)) + "%";
            draw_list->AddText(ImVec2(x, y - 15), IM_COL32(255, 255, 255, 255), label.c_str());
        }
    }

    if (cfg.aim_diagnostics_enable && cfg.aim_diagnostics_text_enable) {
        const ImVec2 panel_pos(12.0f, 72.0f);
        char line1[192];
        snprintf(
            line1,
            sizeof(line1),
            "诊断 | 锁定 %d 帧 | 候选 %d | %s%s%s",
            aim_debug.lock_frames,
            aim_debug.crowd_count,
            aim_debug.target_switched ? "已切换 " : "",
            aim_debug.prediction_gated ? "预测等待 " : (aim_debug.prediction_used ? "预测启用 " : "预测关闭 "),
            aim_debug.stale_frame ? "缓存帧" : "");
        draw_list->AddText(panel_pos, IM_COL32(255, 255, 255, 230), line1);

        char line2[192];
        snprintf(
            line2,
            sizeof(line2),
            "原始=(%d,%d) 预测=(%d,%d) 移动=(%d,%d) 缓存=%d",
            aim_debug.raw_point.x,
            aim_debug.raw_point.y,
            aim_debug.aim_point.x,
            aim_debug.aim_point.y,
            aim_debug.output_dx,
            aim_debug.output_dy,
            aim_debug.stale_frames);
        draw_list->AddText(ImVec2(panel_pos.x, panel_pos.y + 18.0f), IM_COL32(190, 220, 255, 230), line2);
    }

    if (cfg.aim_diagnostics_enable && cfg.aim_diagnostics_points_enable && aim_debug.has_target) {
        const ImVec2 raw_pos(
            static_cast<float>(crop_x + aim_debug.raw_point.x),
            static_cast<float>(crop_y + aim_debug.raw_point.y));
        const ImVec2 aim_pos(
            static_cast<float>(crop_x + aim_debug.aim_point.x),
            static_cast<float>(crop_y + aim_debug.aim_point.y));
        draw_list->AddCircleFilled(raw_pos, 4.0f, IM_COL32(255, 230, 0, 230));
        draw_list->AddCircleFilled(aim_pos, 4.0f, IM_COL32(0, 255, 220, 230));
        draw_list->AddLine(raw_pos, aim_pos, IM_COL32(0, 255, 220, 180), 1.5f);
    }
}

void AimAssistant::DrawTrainingTab() {
    const ImVec2 child_size(0.0f, -ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y * 2.0f);
    if (!ImGui::BeginChild("TrainingScroll", child_size, false, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        ImGui::EndChild();
        return;
    }

    const bool is_training = nn_training.load(std::memory_order_acquire);
    std::string train_status_snapshot;
    std::string status_snapshot;
    std::string diag_snapshot;
    bool model_loaded_snapshot = false;
    {
        std::lock_guard<std::mutex> lock(nn_mutex);
        train_status_snapshot = nn_train_status_internal;
        status_snapshot = nn_status;
        diag_snapshot = nn_diag;
        model_loaded_snapshot = nn_model.IsModelLoaded();
    }

    if (ImGui::CollapsingHeader(UiText(cfg, "数据采集", "Data Collection"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const float collect_progress = cfg.nn_collection_total > 0
            ? std::min(1.0f, static_cast<float>(nn_samples.size()) / static_cast<float>(cfg.nn_collection_total))
            : 0.0f;
        ImGui::Text("%s: %zu / %d", UiText(cfg, "有效样本", "Valid samples"), nn_samples.size(), cfg.nn_collection_total);
        ImGui::ProgressBar(collect_progress, ImVec2(-1, 0), "");
        ImGui::TextDisabled("%s", UiText(cfg,
            "格式: 裁剪像素空间 / 仅记录命中且通过质量过滤的轨迹。旧样本文件需要重新采集。",
            "Format: crop pixel space / only hit samples passing quality filters are recorded. Old samples must be recollected."));
        ImGui::TextDisabled("%s", UiText(cfg,
            "建议: 先采集 150-300 条命中样本，再生成少量增强样本。",
            "Recommended: collect 150-300 hit samples before generating a small augmented set."));
        if (!nn_collect_feedback.empty()) {
            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1), "%s: %s", UiText(cfg, "最近反馈", "Last feedback"), nn_collect_feedback.c_str());
        }
        ImGui::TextDisabled(
            "%s: %s %d | %s %d | %s %d",
            UiText(cfg, "本轮", "Session"),
            UiText(cfg, "命中", "Hits"),
            nn_collect_session_hits,
            UiText(cfg, "未命中", "Misses"),
            nn_collect_session_misses,
            UiText(cfg, "已过滤", "Rejected"),
            nn_collect_session_rejected);

        if (ImGui::BeginTable("CollectActions", 5, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            if (!nn_collecting) {
                if (ImGui::Button(UiText(cfg, "开始采集", "Start"), ImVec2(-1, 30))) {
                    nn_collecting = true;
                    nn_collect_waiting_start = true;
                    nn_collect_trail.clear();
                    nn_collect_recording = false;
                    nn_collect_hit_flash = 0;
                    nn_collect_miss_flash = 0;
                    nn_collect_reject_flash = 0;
                    nn_collect_session_hits = 0;
                    nn_collect_session_misses = 0;
                    nn_collect_session_rejected = 0;
                    nn_collect_feedback = UiText(cfg, "左键开始，命中红色目标才会记录样本", "Left click to start; only hits on the red target are recorded");
                    nn_collect_history.clear();
                    nn_collection_total = cfg.nn_collection_total;
                    overlay.SetMenuOpen(false);
                    UpdateClickability();
                }
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0.7f, 0, 1));
                ImGui::Button(UiText(cfg, "采集中...", "Collecting..."), ImVec2(-1, 30));
                ImGui::PopStyleColor();
            }

            ImGui::TableNextColumn();
            if (ImGui::Button(UiText(cfg, "清除", "Clear"), ImVec2(-1, 30))) {
                nn_samples.clear();
                nn_collect_feedback = UiText(cfg, "样本已清空", "Samples cleared");
            }

            ImGui::TableNextColumn();
            ImGui::BeginDisabled(nn_samples.empty());
            if (ImGui::Button(UiText(cfg, "保存", "Save"), ImVec2(-1, 30))) {
                SaveSamples();
                nn_status = UiText(cfg, "样本已保存", "Samples saved");
            }
            ImGui::EndDisabled();

            ImGui::TableNextColumn();
            if (ImGui::Button(UiText(cfg, "加载", "Load"), ImVec2(-1, 30))) {
                int loaded = LoadSamples();
                if (loaded > 0) {
                    char buf[96];
                    snprintf(buf, sizeof(buf), "%s %d", UiText(cfg, "已加载样本", "Loaded samples"), loaded);
                    nn_status = buf;
                } else {
                    nn_status = UiText(cfg, "未找到样本文件", "Sample file not found");
                }
            }

            ImGui::TableNextColumn();
            ImGui::BeginDisabled(nn_samples.empty());
            if (ImGui::Button(UiText(cfg, "增强", "Augment"), ImVec2(-1, 30))) {
                auto aug = MouseTrajectoryNN::GenerateBezierSamples(cfg.nn_aug_count, cfg.nn_norm_scale * 0.8f);
                if (!aug.empty()) {
                    int accepted = 0;
                    for (const auto& sample : aug) {
                        MouseTrajectoryNN::Sample normalized = MouseTrajectoryNN::NormalizeSample(sample);
                        if (MouseTrajectoryNN::ValidateSample(normalized, cfg.nn_norm_scale)) {
                            nn_samples.push_back(normalized);
                            ++accepted;
                        }
                    }
                    nn_status = std::string(UiText(cfg, "已生成增强样本 +", "Augmented samples +")) + std::to_string(accepted);
                    nn_collect_feedback = UiText(cfg, "增强样本已过滤后加入", "Augmented samples were filtered before insertion");
                }
            }
            DrawHelpTooltip(UiText(cfg, "用 Bezier 曲线生成合成样本来增强数据集", "Generate synthetic Bezier samples to augment the dataset."));
            ImGui::EndDisabled();
            ImGui::EndTable();
        }

        ImGui::SliderInt(UiText(cfg, "采集目标数", "Collection target"), &cfg.nn_collection_total, 100, 1000, "%d");
        ImGui::SliderFloat(UiText(cfg, "归一化范围", "Normalization scale"), &cfg.nn_norm_scale, 300.0f, 3000.0f, "%.0f px");
        DrawHelpTooltip(UiText(cfg,
            "NORM_SCALE, 应 > 最大鼠标位移; 越大网络越不敏感, 越小越容易饱和",
            "NORM_SCALE should be larger than max mouse delta; too large is less sensitive, too small saturates."));
        const bool enough_to_train = nn_samples.size() >= 10;
        const bool recommended_samples = nn_samples.size() >= 150;
        ImGui::TextColored(
            enough_to_train ? (recommended_samples ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0.8f, 0, 1)) : ImVec4(1, 0.35f, 0.35f, 1),
            "%s: %s",
            UiText(cfg, "训练状态", "Training status"),
            enough_to_train
                ? (recommended_samples ? UiText(cfg, "样本量充足", "Enough samples") : UiText(cfg, "可训练，但建议继续采集", "Trainable, but keep collecting"))
                : UiText(cfg, "样本不足", "Not enough samples"));
    }

    if (ImGui::CollapsingHeader(UiText(cfg, "模型训练", "Model Training"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat(UiText(cfg, "学习率", "Learning rate"), &cfg.nn_train_lr, 0.0001f, 0.01f, "%.4f");
        ImGui::SliderFloat(UiText(cfg, "动量", "Momentum"), &cfg.nn_train_momentum, 0.5f, 0.99f, "%.2f");
        ImGui::SliderInt(UiText(cfg, "训练轮次", "Epochs"), &cfg.nn_train_epochs, 50, 500);
        ImGui::SliderInt(UiText(cfg, "批大小", "Batch size"), &cfg.nn_train_batch, 4, 128);
        ImGui::SliderFloat(UiText(cfg, "平滑度损失", "Smoothness loss"), &cfg.nn_train_smooth, 0.0f, 0.5f, "%.2f");
        DrawHelpTooltip(UiText(cfg, "惩罚相邻轨迹点跳变, 越大越平滑", "Penalizes adjacent waypoint jumps; larger is smoother."));
        ImGui::SliderFloat(UiText(cfg, "权重衰减", "Weight decay"), &cfg.nn_weight_decay, 0.0f, 0.001f, "%.6f");
        DrawHelpTooltip(UiText(cfg, "L2正则化, 防止过拟合; 数据少时可适当增大", "L2 regularization; increase slightly for small datasets."));
        ImGui::SliderFloat(UiText(cfg, "梯度裁剪", "Gradient clip"), &cfg.nn_grad_clip, 0.1f, 5.0f, "%.1f");
        DrawHelpTooltip(UiText(cfg, "限制梯度最大值, 防止训练爆炸产生 NaN", "Caps gradient magnitude to avoid NaN explosions."));
        ImGui::SliderInt(UiText(cfg, "增强样本数", "Augmented samples"), &cfg.nn_aug_count, 0, 1000, "%d");
        DrawHelpTooltip(UiText(cfg, "自动用 Bezier 曲线生成合成轨迹; 0=不使用; 建议 200-500", "Synthetic Bezier paths; 0 disables it; recommended 200-500."));

        if (is_training) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "%s %.1f%%", UiText(cfg, "训练中...", "Training..."), nn_train_progress * 100.0f);
            ImGui::ProgressBar(nn_train_progress);
            if (!train_status_snapshot.empty()) {
                ImGui::TextColored(ImVec4(0.6f, 0.8f, 0.6f, 1), "%s", train_status_snapshot.c_str());
            }
        } else {
            if (ImGui::Button(UiText(cfg, "开始训练", "Start training"), ImVec2(140, 30))) {
                StartTraining();
            }
            if (!status_snapshot.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "%s", status_snapshot.c_str());
            }
        }
    }

    if (ImGui::CollapsingHeader(UiText(cfg, "模型文件", "Model Files"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("%s: %s", UiText(cfg, "当前模型", "Current model"), model_loaded_snapshot ? model_paths::BaseNameA(cfg.nn_model_path).c_str() : UiText(cfg, "(无)", "(none)"));
        ImGui::SameLine();
        if (model_loaded_snapshot) {
            DrawStatusBadge(UiText(cfg, "[已加载]", "[loaded]"), true);
        } else {
            DrawStatusBadge(UiText(cfg, "[未加载]", "[not loaded]"), false);
        }

        ImGui::InputText(UiText(cfg, "模型名", "Model name"), cfg.nn_model_path, sizeof(cfg.nn_model_path));

        static int selected_model_idx = -1;
        static std::vector<std::string> model_list;
        static bool models_scanned = false;
        if (!models_scanned) {
            models_scanned = true;
            for (const auto& name : ScanModelFiles(model_paths::NnDirA(), { "*.bin", "*.onnx" })) {
                model_list.push_back(model_paths::NnConfigPath(name));
            }
        }

        if (!model_list.empty()) {
            ImGui::Text("%s", UiText(cfg, "已有模型:", "Existing models:"));
            ImGui::SameLine();
            std::string nn_preview = selected_model_idx >= 0 && selected_model_idx < (int)model_list.size()
                ? model_paths::BaseNameA(model_list[selected_model_idx]) : UiText(cfg, "选择...", "Select...");
            if (ImGui::BeginCombo("##model_select", nn_preview.c_str())) {
                for (int i = 0; i < (int)model_list.size(); i++) {
                    const std::string display_name = model_paths::BaseNameA(model_list[i]);
                    if (ImGui::Selectable(display_name.c_str(), selected_model_idx == i)) {
                        selected_model_idx = i;
                        strncpy_s(cfg.nn_model_path, model_list[i].c_str(), sizeof(cfg.nn_model_path) - 1);
                        std::lock_guard<std::mutex> lock(nn_mutex);
                        const std::string resolved = ResolveNnInputPath(cfg.nn_model_path);
                        if (!resolved.empty() && nn_model.Load(resolved.c_str())) {
                            nn_model.SetModelLoaded(true);
                            nn_debug_once = true;
                            nn_diag.clear();
                            nn_status = UiText(cfg, "已加载", "Loaded");
                        } else {
                            nn_status = UiText(cfg, "加载失败", "Load failed");
                        }
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button(UiText(cfg, "刷新", "Refresh"), ImVec2(70, 0))) {
                model_list.clear();
                models_scanned = false;
                selected_model_idx = -1;
            }
        }

        if (ImGui::Button(UiText(cfg, "加载模型", "Load model"), ImVec2(100, 0))) {
            std::lock_guard<std::mutex> lock(nn_mutex);
            const std::string normalized = NormalizeNnConfigPath(cfg.nn_model_path);
            strncpy_s(cfg.nn_model_path, normalized.c_str(), sizeof(cfg.nn_model_path) - 1);
            const std::string resolved = ResolveNnInputPath(cfg.nn_model_path);
            if (!resolved.empty() && nn_model.Load(resolved.c_str())) {
                nn_model.SetModelLoaded(true);
                nn_debug_once = true;
                nn_diag.clear();
                nn_status = UiText(cfg, "已加载", "Loaded");
                models_scanned = false;
            } else {
                nn_status = std::string(UiText(cfg, "加载失败: ", "Load failed: ")) + cfg.nn_model_path;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(UiText(cfg, "保存模型", "Save model"), ImVec2(100, 0))) {
            std::lock_guard<std::mutex> lock(nn_mutex);
            std::string output_path = ResolveNnOutputPath(cfg.nn_model_path);
            if (output_path.size() < 5 || _stricmp(output_path.c_str() + output_path.size() - 5, ".onnx") != 0) {
                output_path += ".onnx";
            }
            strncpy_s(cfg.nn_model_path, NormalizeNnConfigPath(model_paths::BaseNameA(output_path)).c_str(), sizeof(cfg.nn_model_path) - 1);
            if (nn_model.ExportONNX(output_path.c_str())) {
                nn_status = UiText(cfg, "ONNX 已保存", "ONNX saved");
            } else {
                nn_status = UiText(cfg, "ONNX 保存失败", "ONNX save failed");
            }
            models_scanned = false;
        }
    }

    if (ImGui::CollapsingHeader(UiText(cfg, "轨迹模式", "Path Mode"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox(UiText(cfg, "启用 NN 轨迹 (F8)", "Enable NN path (F8)"), &cfg.nn_trajectory_enable);
        DrawHelpTooltip(UiText(cfg, "F8 单发时使用神经网络生成的类人轨迹", "Use NN-generated human-like path for F8 single-shot."));
        const bool nn_main_selected = cfg.aim_algorithm == AimController::AlgorithmNeuralNetwork;
        cfg.nn_main_aim_enable = nn_main_selected && model_loaded_snapshot;
        ImGui::TextDisabled("%s: %s", UiText(cfg, "主瞄准", "Main aim"), nn_main_selected ? UiText(cfg, "神经网络", "Neural network") : UiText(cfg, "由瞄准算法菜单决定", "Controlled by Aim tab"));
        DrawHelpTooltip(UiText(cfg, "在瞄准算法菜单中选择是否使用 NN 主瞄准。", "Select NN main aim in the Aim tab."));
        if ((cfg.nn_trajectory_enable || nn_main_selected) && !model_loaded_snapshot) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "%s", UiText(cfg, "请先加载或训练模型。", "Load or train a model first."));
        }

        ImGui::TextDisabled("%s", UiText(cfg, "轨迹坐标: 裁剪像素空间，旧模型需要重新训练。", "Path coordinates: crop pixel space; old models need retraining."));
        ImGui::SliderInt(UiText(cfg, "轨迹点步长上限", "Waypoint max step"), &cfg.nn_wp_max_step, 0, 80, "%d");
        DrawHelpTooltip(UiText(cfg, "每步最大crop像素数, 0=使用aim_max_step_near", "Max crop pixels per step; 0 uses aim_max_step_near."));

        if (!diag_snapshot.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1), "%s: %s", UiText(cfg, "诊断", "Diagnostics"), diag_snapshot.c_str());
        }
    }

    if (ImGui::CollapsingHeader(UiText(cfg, "轨迹测试", "Path Test"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("%s", UiText(cfg, "点击屏幕放置红点，观察模型预测轨迹", "Click the screen to place red points and inspect predicted paths."));

        if (!nn_test_mode) {
            if (ImGui::Button(UiText(cfg, "进入测试模式", "Enter test mode"), ImVec2(150, 30))) {
                if (!model_loaded_snapshot) {
                    std::lock_guard<std::mutex> lock(nn_mutex);
                    nn_status = UiText(cfg, "请先加载或训练模型", "Load or train a model first");
                } else {
                    nn_test_targets.clear();
                    nn_test_trajectories.clear();
                    nn_test_mode = true;
                    overlay.SetMenuOpen(false);
                    UpdateClickability();
                }
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0.7f, 0, 1));
            ImGui::Button(UiText(cfg, "测试中... (ESC 退出, 左键放置目标, 右键撤销)", "Testing... (ESC exits, left click places target, right click undo)"), ImVec2(-1, 30));
            ImGui::PopStyleColor();
        }
    }

    ImGui::EndChild();
}

void AimAssistant::DrawCollectOverlay() {
    auto draw = ImGui::GetBackgroundDrawList();

    float sw = (float)overlay.GetWidth();
    float sh = (float)overlay.GetHeight();
    float cx = sw / 2.0f;
    float cy = sh / 2.0f;

    float cos_y = cosf(nn_collect_cam_yaw);
    float sin_y = sinf(nn_collect_cam_yaw);
    float cos_p = cosf(nn_collect_cam_pitch);
    float sin_p = sinf(nn_collect_cam_pitch);
    float focal = (sw / 2.0f) / tanf(nn_collect_fov / 2.0f);
    float D = nn_collect_world_dist;

    // World → screen projection
    auto proj = [&](float wx, float wy, float wz, float& sx, float& sy) -> bool {
        float rx = cos_y * wx - sin_y * wz;
        float rz = sin_y * wx + cos_y * wz;
        float ry = wy;
        float ry2 = cos_p * ry - sin_p * rz;
        float rz2 = sin_p * ry + cos_p * rz;
        if (rz2 <= 0.5f) return false;
        sx = cx + rx * focal / rz2;
        sy = cy + ry2 * focal / rz2;
        return true;
    };

    // ---- Full-screen dark base (prevents desktop bleed) ----
    draw->AddRectFilled(ImVec2(0, 0), ImVec2(sw, sh), IM_COL32(18, 22, 35, 255));

    // ---- Room: single front wall + simple side/floors ----
    const float RW = 500.0f;   // half-width
    const float RH = 350.0f;   // half-height

    // Front wall
    float f[4][2];  // tl, tr, br, bl
    int fc = (int)proj(-RW, -RH, D, f[0][0],f[0][1])
           + (int)proj( RW, -RH, D, f[1][0],f[1][1])
           + (int)proj( RW,  RH, D, f[2][0],f[2][1])
           + (int)proj(-RW,  RH, D, f[3][0],f[3][1]);
    if (fc == 4) {
        draw->AddQuadFilled(ImVec2(f[0][0],f[0][1]), ImVec2(f[1][0],f[1][1]),
            ImVec2(f[2][0],f[2][1]), ImVec2(f[3][0],f[3][1]), IM_COL32(42, 47, 58, 255));
        draw->AddQuad(ImVec2(f[0][0],f[0][1]), ImVec2(f[1][0],f[1][1]),
            ImVec2(f[2][0],f[2][1]), ImVec2(f[3][0],f[3][1]), IM_COL32(70, 75, 85, 200), 2.0f);

        // Side walls: extend from front wall edges to screen edges
        draw->AddQuadFilled(ImVec2(0, f[0][1]), ImVec2(f[0][0], f[0][1]),
            ImVec2(f[3][0], f[3][1]), ImVec2(0, f[3][1]),
            IM_COL32(36, 40, 50, 255));
        draw->AddQuadFilled(ImVec2(f[1][0], f[1][1]), ImVec2(sw, f[1][1]),
            ImVec2(sw, f[2][1]), ImVec2(f[2][0], f[2][1]),
            IM_COL32(36, 40, 50, 255));
        // Ceiling
        draw->AddQuadFilled(ImVec2(0, 0), ImVec2(sw, 0),
            ImVec2(f[1][0], f[1][1]), ImVec2(f[0][0], f[0][1]),
            IM_COL32(28, 32, 42, 255));
        // Floor
        draw->AddQuadFilled(ImVec2(f[3][0], f[3][1]), ImVec2(f[2][0], f[2][1]),
            ImVec2(sw, sh), ImVec2(0, sh),
            IM_COL32(50, 55, 65, 255));

        // Grid lines on front wall
        ImU32 gc = IM_COL32(65, 70, 82, 55);
        auto lerp_pt = [](float* a, float* b, float t, float& ox, float& oy) {
            ox = a[0] + (b[0] - a[0]) * t;
            oy = a[1] + (b[1] - a[1]) * t;
        };
        for (int yi = -5; yi <= 5; yi++) {
            float t = (yi + 5.0f) / 10.0f;
            float sx1, sy1, sx2, sy2;
            lerp_pt(f[0], f[1], t, sx1, sy1);
            lerp_pt(f[3], f[2], t, sx2, sy2);
            draw->AddLine(ImVec2(sx1, sy1), ImVec2(sx2, sy2), gc, 1.0f);
        }
        for (int xi = -7; xi <= 7; xi++) {
            float t = (xi + 7.0f) / 14.0f;
            float sx1, sy1, sx2, sy2;
            lerp_pt(f[0], f[3], t, sx1, sy1);
            lerp_pt(f[1], f[2], t, sx2, sy2);
            draw->AddLine(ImVec2(sx1, sy1), ImVec2(sx2, sy2), gc, 1.0f);
        }

        // Decorations on the front wall
        struct Deco { float tx, ty; float tw, th; ImU32 col; };
        static const Deco decos[] = {
            {0.30f, 0.25f, 0.18f, 0.12f, IM_COL32(48, 53, 65, 230)},
            {0.70f, 0.30f, 0.14f, 0.18f, IM_COL32(45, 50, 62, 230)},
            {0.50f, 0.65f, 0.22f, 0.14f, IM_COL32(50, 55, 67, 230)},
            {0.20f, 0.70f, 0.16f, 0.16f, IM_COL32(42, 47, 58, 230)},
            {0.80f, 0.60f, 0.15f, 0.13f, IM_COL32(46, 51, 63, 230)},
        };
        for (auto& d : decos) {
            float sx1, sy1, sx4, sy4;
            lerp_pt(f[0], f[1], d.tx, sx1, sy1);
            lerp_pt(f[3], f[2], d.tx, sx4, sy4);
            float mid_x = sx1 + (sx4 - sx1) * d.ty;
            float mid_y = sy1 + (sy4 - sy1) * d.ty;
            float hw = (sx4 - sx1) * d.tw * 0.5f;
            float hh = (sy4 - sy1) * d.th * 0.5f;
            float tx1 = mid_x - hw, ty1 = mid_y - hh;
            float tx2 = mid_x + hw, ty2 = mid_y + hh;
            draw->AddRectFilled(ImVec2(tx1, ty1), ImVec2(tx2, ty2), d.col);
            draw->AddRect(ImVec2(tx1, ty1), ImVec2(tx2, ty2), IM_COL32(85, 90, 100, 160), 0, 0, 1.0f);
        }
    }

    // ---- Target ----
    if (!nn_collect_waiting_start) {
        float tx = nn_collect_target_x;
        float ty = nn_collect_target_y;
        float tr = nn_collect_target_radius;
        float sx, sy;
        if (proj(tx, ty, D, sx, sy)) {
            float rx = cos_y * tx - sin_y * D;
            float rz = sin_y * tx + cos_y * D;
            float rz2 = sin_p * ty + cos_p * rz;
            float sr = tr * focal / rz2;
            bool hit_f = nn_collect_hit_flash > 0;
            bool miss_f = nn_collect_miss_flash > 0;
            bool reject_f = nn_collect_reject_flash > 0;

            int rings = (int)(sr / 2.0f);
            if (rings < 4) rings = 4;
            if (rings > 18) rings = 18;
            for (int i = rings; i >= 0; i--) {
                float frac = (float)i / (float)rings;
                float rr = sr * frac;
                int a = (int)(80 + frac * 100);
                ImU32 col = hit_f ? IM_COL32(50, 255, 50, a)
                          : reject_f ? IM_COL32(255, 190, 60, a)
                          : miss_f ? IM_COL32(255, 80, 80, a)
                          : IM_COL32((int)(220 + frac * 35), (int)(30 + frac * 40), (int)(30 + frac * 40), a);
                draw->AddCircleFilled(ImVec2(sx, sy), rr, col);
            }
            draw->AddCircleFilled(ImVec2(sx - sr*0.2f, sy - sr*0.25f), sr*0.35f, IM_COL32(255, 180, 160, 70));
            float rw = std::max(1.5f, sr*0.04f);
            draw->AddCircle(ImVec2(sx, sy), sr,
                hit_f ? IM_COL32(0, 255, 0, 255)
                : reject_f ? IM_COL32(255, 190, 60, 255)
                : miss_f ? IM_COL32(255, 100, 100, 255)
                : IM_COL32(255, 60, 40, 240), 0, rw);
            if (hit_f) {
                float xl = sr * 0.6f;
                draw->AddLine(ImVec2(sx-xl,sy-xl), ImVec2(sx+xl,sy+xl), IM_COL32(255,255,255,255), 2.5f);
                draw->AddLine(ImVec2(sx+xl,sy-xl), ImVec2(sx-xl,sy+xl), IM_COL32(255,255,255,255), 2.5f);
            }
            draw->AddCircle(ImVec2(sx, sy), sr + 4.0f, IM_COL32(255, 40, 30, 60), 0, 3.0f);
        }
    }

    // ---- Mouse trail ----
    if (!nn_collect_waiting_start && nn_collect_trail.size() >= 2) {
        for (size_t i = 0; i < nn_collect_trail.size() - 1; i++) {
            float alpha = 0.3f + 0.7f * (float)i / (float)(nn_collect_trail.size() - 1);
            draw->AddLine(ImVec2(cx + nn_collect_trail[i].x, cy + nn_collect_trail[i].y),
                          ImVec2(cx + nn_collect_trail[i + 1].x, cy + nn_collect_trail[i + 1].y),
                          IM_COL32(100, 200, 255, (int)(alpha * 180)), 2.0f);
        }
        cv::Point2f last = nn_collect_trail.back();
        draw->AddCircleFilled(ImVec2(cx + last.x, cy + last.y), 3.5f, IM_COL32(100, 200, 255, 200));
    }

    // ---- Ghost markers ----
    for (size_t t = 0; t < nn_collect_history.size(); t++) {
        float sx, sy;
        if (proj(nn_collect_history[t].x, nn_collect_history[t].y, D, sx, sy)) {
            float alpha = 0.15f + 0.25f * (float)t / (float)nn_collect_history.size();
            draw->AddCircleFilled(ImVec2(sx, sy), 5.0f, IM_COL32(0, 255, 100, (int)(alpha * 180)));
        }
    }

    // ---- Crosshair ----
    float ch_len = 18.0f, ch_gap = 6.0f;
    ImU32 ch_col = IM_COL32(0, 255, 0, 250);
    draw->AddLine(ImVec2(cx, cy - ch_gap - ch_len), ImVec2(cx, cy - ch_gap), ch_col, 2.5f);
    draw->AddLine(ImVec2(cx, cy + ch_gap), ImVec2(cx, cy + ch_gap + ch_len), ch_col, 2.5f);
    draw->AddLine(ImVec2(cx - ch_gap - ch_len, cy), ImVec2(cx - ch_gap, cy), ch_col, 2.5f);
    draw->AddLine(ImVec2(cx + ch_gap, cy), ImVec2(cx + ch_gap + ch_len, cy), ch_col, 2.5f);
    draw->AddCircleFilled(ImVec2(cx, cy), 2.0f, IM_COL32(0, 255, 0, 220));

    // ---- HUD ----
    draw->AddRectFilled(ImVec2(10, 8), ImVec2(360, 116), IM_COL32(12, 16, 24, 185), 8.0f);
    draw->AddRect(ImVec2(10, 8), ImVec2(360, 116), IM_COL32(90, 120, 150, 120), 8.0f);
    char buf[128];
    snprintf(buf, sizeof(buf), "轨迹采集  %zu / %d", nn_samples.size(), cfg.nn_collection_total);
    draw->AddText(ImVec2(20, 16), IM_COL32(255, 255, 255, 230), buf);

    float bar_w = 320.0f;
    float bar_x = 20.0f;
    draw->AddRectFilled(ImVec2(bar_x, 40.0f), ImVec2(bar_x + bar_w, 48.0f), IM_COL32(60, 60, 60, 200), 4.0f);
    float prog = (float)nn_samples.size() / (float)cfg.nn_collection_total;
    draw->AddRectFilled(ImVec2(bar_x, 40.0f),
        ImVec2(bar_x + bar_w * std::min(1.0f, prog), 48.0f), IM_COL32(0, 200, 100, 230), 4.0f);

    snprintf(
        buf,
        sizeof(buf),
        "命中 %d  未命中 %d  过滤 %d",
        nn_collect_session_hits,
        nn_collect_session_misses,
        nn_collect_session_rejected);
    draw->AddText(ImVec2(20, 58), IM_COL32(180, 220, 255, 220), buf);
    if (!nn_collect_feedback.empty()) {
        ImU32 feedback_color = nn_collect_hit_flash > 0
            ? IM_COL32(100, 255, 140, 235)
            : nn_collect_reject_flash > 0
                ? IM_COL32(255, 210, 90, 235)
                : nn_collect_miss_flash > 0
                    ? IM_COL32(255, 120, 120, 235)
                    : IM_COL32(220, 220, 220, 210);
        draw->AddText(ImVec2(20, 80), feedback_color, nn_collect_feedback.c_str());
    }

    float heading_deg = nn_collect_cam_yaw * 57.29578f;
    if (heading_deg < 0) heading_deg += 360.0f;
    snprintf(buf, sizeof(buf), "朝向: %.0f°", heading_deg);
    draw->AddText(ImVec2(sw - 100, 12), IM_COL32(150, 150, 150, 180), buf);

    if (nn_collect_waiting_start) {
        draw->AddRectFilled(ImVec2(cx - 170, cy + 50), ImVec2(cx + 170, cy + 92), IM_COL32(10, 10, 10, 150), 8.0f);
        draw->AddText(ImVec2(cx - 150, cy + 62), IM_COL32(255, 255, 100, 230), "左键点击开始，命中红色目标才会记录");
    } else {
        snprintf(buf, sizeof(buf), "轨迹 %zu 点  |  左键开枪", nn_collect_trail.size());
        draw->AddText(ImVec2(10, 30), IM_COL32(100, 220, 255, 220), buf);
    }
    draw->AddText(ImVec2(10, sh - 24), IM_COL32(120, 120, 120, 180), "ESC 退出");
}

void AimAssistant::HandleCollectionInput() {
    float cx = overlay.GetWidth() / 2.0f;
    float cy = overlay.GetHeight() / 2.0f;
    float scl_x = (float)overlay.GetWidth() / (float)cfg.crop_size;
    float scl_y = (float)overlay.GetHeight() / (float)cfg.crop_size;

    static bool lbtn_was = false;
    bool lbtn = GetAsyncKeyState(VK_LBUTTON) & 0x8000;

    // ESC to exit
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        nn_collecting = false;
        nn_collect_recording = false;
        nn_collect_trail.clear();
        if (nn_collect_cursor_locked) {
            ClipCursor(nullptr);
            while (ShowCursor(TRUE) < 0);
            nn_collect_cursor_locked = false;
        }
        UpdateClickability();
        return;
    }

    // ---- Waiting start ----
    if (nn_collect_waiting_start) {
        if (lbtn && !lbtn_was) {
            nn_collect_waiting_start = false;
            nn_collect_trail.clear();
            nn_collect_cam_yaw = 0.0f;
            nn_collect_cam_pitch = 0.0f;
            nn_collect_accum_dx = 0.0f;
            nn_collect_accum_dy = 0.0f;

            // Lock cursor
            nn_collect_cursor_locked = true;
            RECT clip;
            GetWindowRect(overlay.GetHwnd(), &clip);
            ClipCursor(&clip);
            while (ShowCursor(FALSE) >= 0);

            // Spawn first target
            nn_collect_target_x = -300.0f + (float)(rand() % 601);
            nn_collect_target_y = -180.0f + (float)(rand() % 361);
            nn_collect_target_radius = 10.0f + (float)(rand() % 30);
        }
        lbtn_was = lbtn;
        return;
    }

    // ---- Cursor lock + camera rotation ----
    if (nn_collect_cursor_locked) {
        POINT cp;
        GetCursorPos(&cp);
        float mdx = (float)(cp.x - (LONG)cx);
        float mdy = (float)(cp.y - (LONG)cy);

        if (fabsf(mdx) > 0.5f || fabsf(mdy) > 0.5f) {
            nn_collect_cam_yaw   += mdx * nn_collect_mouse_sens;
            nn_collect_cam_pitch += mdy * nn_collect_mouse_sens;
            const float max_yaw = 1.1345f;
            if (nn_collect_cam_yaw >  max_yaw) nn_collect_cam_yaw =  max_yaw;
            if (nn_collect_cam_yaw < -max_yaw) nn_collect_cam_yaw = -max_yaw;
            const float max_pitch = 1.0f;
            if (nn_collect_cam_pitch >  max_pitch) nn_collect_cam_pitch =  max_pitch;
            if (nn_collect_cam_pitch < -max_pitch) nn_collect_cam_pitch = -max_pitch;
            while (nn_collect_cam_yaw >  3.14159265f) nn_collect_cam_yaw -= 6.2831853f;
            while (nn_collect_cam_yaw < -3.14159265f) nn_collect_cam_yaw += 6.2831853f;

            POINT center_pt = { (LONG)cx, (LONG)cy };
            ClientToScreen(overlay.GetHwnd(), &center_pt);
            SetCursorPos(center_pt.x, center_pt.y);

            // Always accumulate trail while target is active
            nn_collect_accum_dx += mdx;
            nn_collect_accum_dy += mdy;
            nn_collect_trail.push_back(cv::Point2f(nn_collect_accum_dx, nn_collect_accum_dy));
            if (nn_collect_trail.size() > COLLECT_TRAIL_MAX)
                nn_collect_trail.erase(nn_collect_trail.begin(),
                    nn_collect_trail.begin() + COLLECT_TRAIL_MAX / 4);
        }
    }

    // ---- Flash decay ----
    if (nn_collect_hit_flash > 0) nn_collect_hit_flash--;
    if (nn_collect_miss_flash > 0) nn_collect_miss_flash--;
    if (nn_collect_reject_flash > 0) nn_collect_reject_flash--;

    // ---- Click to shoot (FPS style) ----
    if (lbtn && !lbtn_was) {
        float focal = (cx * 2.0f) / tanf(nn_collect_fov / 2.0f);

        // Project target to screen
        float cos_y = cosf(nn_collect_cam_yaw), sin_y = sinf(nn_collect_cam_yaw);
        float cos_p = cosf(nn_collect_cam_pitch), sin_p = sinf(nn_collect_cam_pitch);
        float rx = cos_y * nn_collect_target_x - sin_y * nn_collect_world_dist;
        float rz = sin_y * nn_collect_target_x + cos_y * nn_collect_world_dist;
        float ry2 = cos_p * nn_collect_target_y - sin_p * rz;
        float rz2 = sin_p * nn_collect_target_y + cos_p * rz;
        float sx = cx + rx * focal / rz2;
        float sy = cy + ry2 * focal / rz2;
        float sr = nn_collect_target_radius * focal / rz2;

        float dist = sqrtf((sx - cx) * (sx - cx) + (sy - cy) * (sy - cy));
        bool hit = (rz2 > 1.0f) && (dist <= sr * 1.5f);

        if (hit && nn_collect_trail.size() >= 3) {
            MouseTrajectoryNN::Sample sample;
            sample.dx = nn_collect_accum_dx / scl_x;
            sample.dy = nn_collect_accum_dy / scl_y;

            int count = (int)nn_collect_trail.size();
            for (int i = 0; i < MouseTrajectoryNN::WAYPOINT_COUNT; i++) {
                float t = (float)i / (MouseTrajectoryNN::WAYPOINT_COUNT - 1);
                float idx_f = t * (count - 1);
                int idx0 = (int)idx_f;
                int idx1 = std::min(idx0 + 1, count - 1);
                float frac = idx_f - idx0;
                float wx = nn_collect_trail[idx0].x + (nn_collect_trail[idx1].x - nn_collect_trail[idx0].x) * frac;
                float wy = nn_collect_trail[idx0].y + (nn_collect_trail[idx1].y - nn_collect_trail[idx0].y) * frac;
                sample.waypoints[i * 2]     = wx / scl_x;
                sample.waypoints[i * 2 + 1] = wy / scl_y;
            }
            sample = MouseTrajectoryNN::NormalizeSample(sample);
            if (MouseTrajectoryNN::ValidateSample(sample, cfg.nn_norm_scale)) {
                nn_samples.push_back(sample);
                nn_collect_hit_flash = 12;
                ++nn_collect_session_hits;
                nn_collect_feedback = "命中，样本已记录";
            } else {
                nn_collect_reject_flash = 12;
                ++nn_collect_session_rejected;
                nn_collect_feedback = "命中但轨迹质量不合格，已过滤";
            }

            if (nn_samples.size() >= (size_t)cfg.nn_collection_total) {
                nn_collecting = false;
                if (nn_collect_cursor_locked) {
                    ClipCursor(nullptr);
                    while (ShowCursor(TRUE) < 0);
                    nn_collect_cursor_locked = false;
                }
                UpdateClickability();
                lbtn_was = lbtn;
                return;
            }
        } else if (nn_collect_trail.size() >= 3) {
            nn_collect_miss_flash = 12;
            ++nn_collect_session_misses;
            nn_collect_feedback = "未命中，不记录样本";
        }

        // Spawn new target
        nn_collect_history.push_back(cv::Point2f(nn_collect_target_x, nn_collect_target_y));
        if (nn_collect_history.size() > 15)
            nn_collect_history.erase(nn_collect_history.begin());
        nn_collect_target_x = -300.0f + (float)(rand() % 601);
        nn_collect_target_y = -180.0f + (float)(rand() % 361);
        nn_collect_target_radius = 10.0f + (float)(rand() % 30);
        nn_collect_trail.clear();
        nn_collect_accum_dx = 0.0f;
        nn_collect_accum_dy = 0.0f;
    }
    lbtn_was = lbtn;
}

void AimAssistant::StartTraining() {
    if (nn_samples.size() < 10) {
        nn_status = "至少需要 10 条样本";
        return;
    }
    if (nn_training.load(std::memory_order_acquire)) return;

    nn_training.store(true, std::memory_order_release);
    nn_train_progress = 0.0f;
    nn_train_progress_atomic.store(0.0f, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(nn_mutex);
        nn_train_status_internal = "准备训练...";
    }

    std::vector<MouseTrajectoryNN::Sample> samples_copy = nn_samples;

    if (cfg.nn_aug_count > 0) {
        {
            std::lock_guard<std::mutex> lock(nn_mutex);
            nn_train_status_internal = "生成增强样本...";
        }
        auto aug = MouseTrajectoryNN::GenerateBezierSamples(cfg.nn_aug_count, cfg.nn_norm_scale * 0.8f);
        if (!aug.empty()) {
            samples_copy.insert(samples_copy.end(), aug.begin(), aug.end());
        }
    }

    const size_t sample_count_before_filter = samples_copy.size();
    std::vector<MouseTrajectoryNN::Sample> filtered_samples;
    filtered_samples.reserve(samples_copy.size());
    for (const auto& sample : samples_copy) {
        MouseTrajectoryNN::Sample normalized = MouseTrajectoryNN::NormalizeSample(sample);
        if (MouseTrajectoryNN::ValidateSample(normalized, cfg.nn_norm_scale)) {
            filtered_samples.push_back(normalized);
        }
    }
    const size_t filtered_count = sample_count_before_filter - filtered_samples.size();
    if (filtered_samples.size() < 10) {
        nn_training.store(false, std::memory_order_release);
        nn_status = "有效样本不足，无法训练";
        return;
    }
    samples_copy = std::move(filtered_samples);

    if (nn_train_thread.joinable()) nn_train_thread.join();

    const int train_epochs = cfg.nn_train_epochs;
    const float train_lr = cfg.nn_train_lr;
    const float train_momentum = cfg.nn_train_momentum;
    const int train_batch = cfg.nn_train_batch;
    const float train_smooth = cfg.nn_train_smooth;
    const float weight_decay = cfg.nn_weight_decay;
    const float grad_clip = cfg.nn_grad_clip;
    const float norm_scale = cfg.nn_norm_scale;
    const std::string model_path = NormalizeNnConfigPath(cfg.nn_model_path);
    strncpy_s(cfg.nn_model_path, model_path.c_str(), sizeof(cfg.nn_model_path) - 1);

    nn_train_thread = std::thread([this, samples_copy, filtered_count, train_epochs, train_lr, train_momentum,
                                   train_batch, train_smooth, weight_decay, grad_clip,
                                   norm_scale, model_path]() {
        MouseTrajectoryNN local_nn;
        float final_loss = 0.0f;
        float val_loss = 0.0f;
        std::string status;

        MouseTrajectoryNN::TrainConfig tc;
        tc.epochs        = train_epochs;
        tc.lr            = train_lr;
        tc.momentum      = train_momentum;
        tc.batch_size    = train_batch;
        tc.lambda_smooth = train_smooth;
        tc.weight_decay  = weight_decay;
        tc.grad_clip     = grad_clip;
        tc.norm_scale    = norm_scale;
        tc.progress      = &nn_train_progress_atomic;
        tc.status_callback = [this](const std::string& text) {
            std::lock_guard<std::mutex> lock(nn_mutex);
            nn_train_status_internal = text;
        };

        bool ok = local_nn.Train(samples_copy, tc, &final_loss, &status, &val_loss);

        {
            std::lock_guard<std::mutex> lock(nn_mutex);
            if (ok) {
                std::string wdiag;
                if (!local_nn.ValidateWeights(&wdiag)) {
                    nn_status = "训练出现无效权重: " + wdiag;
                    nn_train_status_internal = "训练失败：请降低学习率或增加样本";
                    nn_train_progress_atomic.store(1.0f, std::memory_order_relaxed);
                    nn_training.store(false, std::memory_order_release);
                    return;
                }

                nn_model = local_nn;
                nn_model.SetModelLoaded(true);
                nn_debug_once = true;

                float test_wp[MouseTrajectoryNN::OUTPUT_SIZE];
                nn_model.Predict(100.0f, 50.0f, test_wp);
                float last_x = test_wp[(MouseTrajectoryNN::WAYPOINT_COUNT-1)*2];
                float last_y = test_wp[(MouseTrajectoryNN::WAYPOINT_COUNT-1)*2+1];
                char diag[256];
                snprintf(diag, sizeof(diag),
                    "样本=%zu 过滤=%zu loss=%.3f val=%.3f lr=%.4f wd=%.6f smooth=%.2f norm=%d test(100,50)->wp9=(%.0f,%.0f)",
                    samples_copy.size(), filtered_count, final_loss, val_loss,
                    train_lr, weight_decay, train_smooth, (int)norm_scale,
                    last_x, last_y);
                nn_diag = diag;

                std::string onnx_path = ResolveNnOutputPath(model_path);
                if (onnx_path.size() < 5 || _stricmp(onnx_path.c_str() + onnx_path.size() - 5, ".onnx") != 0) {
                    onnx_path += ".onnx";
                }
                if (nn_model.ExportONNX(onnx_path.c_str())) {
                    char msg[160];
                    snprintf(msg, sizeof(msg), "  |  ONNX 已导出: %s", onnx_path.c_str());
                    nn_diag += msg;
                } else {
                    nn_diag += "  |  ONNX 导出失败，请检查磁盘空间";
                }

                // Also save binary for fast local load
                const std::string binary_path = model_paths::ReplaceExtension(onnx_path, ".bin");
                nn_model.Save(binary_path.c_str());
            } else {
                nn_train_status_internal = "训练失败: " + status;
            }
        }

        nn_train_progress_atomic.store(1.0f, std::memory_order_relaxed);
        nn_training.store(false, std::memory_order_release);
    });
}

bool AimAssistant::BeginNNTrajectory(float error_x, float error_y, bool single_shot) {
    if (!std::isfinite(error_x) || !std::isfinite(error_y)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(nn_mutex);
        if (!nn_model.IsModelLoaded()) {
            return false;
        }
        nn_model.Predict(error_x, error_y, nn_waypoints);
    }

    bool wp_valid = true;
    for (int i = 0; i < MouseTrajectoryNN::OUTPUT_SIZE && wp_valid; i++) {
        wp_valid = std::isfinite(nn_waypoints[i]);
    }
    if (!wp_valid) {
        std::lock_guard<std::mutex> lock(nn_mutex);
        nn_model.SetModelLoaded(false);
        nn_status = "模型输出无效，请重新训练";
        return false;
    }

    MouseTrajectoryNN::SmoothTrajectory(nn_waypoints, MouseTrajectoryNN::WAYPOINT_COUNT, 3, 0.35f);
    nn_waypoints[0] = 0.0f;
    nn_waypoints[1] = 0.0f;
    nn_waypoints[(MouseTrajectoryNN::WAYPOINT_COUNT - 1) * 2] = error_x;
    nn_waypoints[(MouseTrajectoryNN::WAYPOINT_COUNT - 1) * 2 + 1] = error_y;

    if (nn_debug_once) {
        nn_debug_once = false;
        char buf[192];
        const float lx = nn_waypoints[(MouseTrajectoryNN::WAYPOINT_COUNT - 1) * 2];
        const float ly = nn_waypoints[(MouseTrajectoryNN::WAYPOINT_COUNT - 1) * 2 + 1];
        snprintf(
            buf,
            sizeof(buf),
            "input=(%.0f,%.0f) wp0=(%.0f,%.0f) wp%d=(%.0f,%.0f)",
            error_x,
            error_y,
            nn_waypoints[0],
            nn_waypoints[1],
            MouseTrajectoryNN::WAYPOINT_COUNT - 1,
            lx,
            ly);
        nn_diag = buf;
    }

    nn_elapsed = 0.0f;
    nn_sample_x = 0.0f;
    nn_sample_y = 0.0f;
    nn_mouse_fractional_x = 0.0f;
    nn_mouse_fractional_y = 0.0f;
    nn_path_single_shot = single_shot;
    nn_path_active = true;
    return true;
}

bool AimAssistant::StepNNTrajectory(double dt) {
    if (!nn_path_active) {
        return false;
    }

    const float step_dt = std::max(0.001f, std::min(static_cast<float>(dt), 0.050f));
    const float duration = std::max(0.005f, cfg.aim_smooth_time);
    nn_elapsed += step_dt;

    float progress = nn_elapsed / duration;
    if (progress > 1.0f) progress = 1.0f;

    const float idx_f = progress * (MouseTrajectoryNN::WAYPOINT_COUNT - 1);
    const int idx0 = static_cast<int>(idx_f);
    const int idx1 = (idx0 + 1 < MouseTrajectoryNN::WAYPOINT_COUNT) ? idx0 + 1 : idx0;
    const float frac = idx_f - static_cast<float>(idx0);
    const float wx = nn_waypoints[idx0 * 2] * (1.0f - frac) + nn_waypoints[idx1 * 2] * frac;
    const float wy = nn_waypoints[idx0 * 2 + 1] * (1.0f - frac) + nn_waypoints[idx1 * 2 + 1] * frac;

    const float mdx = wx - nn_sample_x;
    const float mdy = wy - nn_sample_y;
    const float conversion = static_cast<float>(mouse_correction_factor * cfg.sensitivity);
    if (conversion <= 0.0001f) {
        nn_path_active = false;
        return false;
    }

    float exact_x = mdx * conversion + nn_mouse_fractional_x;
    float exact_y = mdy * conversion + nn_mouse_fractional_y;
    int move_x = static_cast<int>(std::lround(exact_x));
    int move_y = static_cast<int>(std::lround(exact_y));

    int max_step = cfg.nn_wp_max_step > 0
        ? static_cast<int>(cfg.nn_wp_max_step * conversion)
        : static_cast<int>(cfg.aim_max_step_near * conversion);
    if (max_step < 1) max_step = 1;

    const bool clamped_x = move_x > max_step || move_x < -max_step;
    const bool clamped_y = move_y > max_step || move_y < -max_step;
    if (move_x > max_step) move_x = max_step;
    if (move_x < -max_step) move_x = -max_step;
    if (move_y > max_step) move_y = max_step;
    if (move_y < -max_step) move_y = -max_step;

    nn_mouse_fractional_x = clamped_x ? 0.0f : exact_x - static_cast<float>(move_x);
    nn_mouse_fractional_y = clamped_y ? 0.0f : exact_y - static_cast<float>(move_y);

    if (move_x != 0 || move_y != 0) {
        mouse.MoveRelative(move_x, move_y);
        aim_debug.output_dx = move_x;
        aim_debug.output_dy = move_y;
    }

    nn_sample_x += static_cast<float>(move_x) / conversion;
    nn_sample_y += static_cast<float>(move_y) / conversion;

    const float final_x = nn_waypoints[(MouseTrajectoryNN::WAYPOINT_COUNT - 1) * 2];
    const float final_y = nn_waypoints[(MouseTrajectoryNN::WAYPOINT_COUNT - 1) * 2 + 1];
    const float residual = std::hypot(final_x - nn_sample_x, final_y - nn_sample_y);
    if ((progress >= 1.0f && residual <= 1.0f) || nn_elapsed > duration * 1.8f) {
        nn_path_active = false;
        nn_path_single_shot = false;
        nn_elapsed = 0.0f;
        nn_mouse_fractional_x = 0.0f;
        nn_mouse_fractional_y = 0.0f;
    }

    return true;
}

void AimAssistant::RunNNTrajectory(const Detection* target) {
    if (!target) return;

    const cv::Point tp = getTargetPoint(target);
    const float dx = static_cast<float>(tp.x - crop_center.x);
    const float dy = static_cast<float>(tp.y - crop_center.y);
    BeginNNTrajectory(dx, dy, true);
}

// ---------------------------------------------------------------------------
//  Trajectory test mode
// ---------------------------------------------------------------------------
void AimAssistant::DrawTestOverlay() {
    auto draw = ImGui::GetBackgroundDrawList();

    float cx = overlay.GetWidth() / 2.0f;
    float cy = overlay.GetHeight() / 2.0f;
    float scl_x = (float)overlay.GetWidth() / (float)cfg.crop_size;
    float scl_y = (float)overlay.GetHeight() / (float)cfg.crop_size;

    // Dim background
    draw->AddRectFilled(ImVec2(0, 0), ImVec2((float)overlay.GetWidth(), (float)overlay.GetHeight()),
        IM_COL32(0, 0, 0, 100));

    // Center crosshair (green)
    float cross_len = 15.0f;
    draw->AddLine(ImVec2(cx - cross_len, cy), ImVec2(cx + cross_len, cy), IM_COL32(0, 255, 0, 200), 2.0f);
    draw->AddLine(ImVec2(cx, cy - cross_len), ImVec2(cx, cy + cross_len), IM_COL32(0, 255, 0, 200), 2.0f);
    draw->AddCircle(ImVec2(cx, cy), 4.0f, IM_COL32(0, 255, 0, 200), 0, 2.0f);

    // Draw placed targets and predicted trajectories
    for (size_t t = 0; t < nn_test_targets.size(); t++) {
        const auto& target = nn_test_targets[t];

        // Red target dot
        draw->AddCircleFilled(ImVec2(target.x, target.y), 10.0f, IM_COL32(255, 50, 50, 220));
        draw->AddCircle(ImVec2(target.x, target.y), 10.0f, IM_COL32(255, 255, 255, 200), 0, 1.5f);

        // Predicted trajectory (blue curved line)
        if (t < nn_test_trajectories.size() && nn_test_trajectories[t].size() >= 2) {
            const auto& traj = nn_test_trajectories[t];
            for (size_t i = 0; i < traj.size() - 1; i++) {
                draw->AddLine(
                    ImVec2(traj[i].x, traj[i].y),
                    ImVec2(traj[i + 1].x, traj[i + 1].y),
                    IM_COL32(50, 150, 255, 200), 2.5f);
            }

            // Draw waypoint dots along the trajectory
            for (size_t i = 0; i < traj.size(); i++) {
                float alpha = 0.3f + 0.7f * (float)i / (float)(traj.size() - 1);
                int a = (int)(alpha * 180);
                draw->AddCircleFilled(ImVec2(traj[i].x, traj[i].y), 3.0f,
                    IM_COL32(50, 150, 255, a));
            }
        }
    }

    // HUD text
    char buf[128];
    snprintf(buf, sizeof(buf), "轨迹测试 | 已放置 %zu 个目标 | 左键=放置 右键=撤销 ESC=退出",
        nn_test_targets.size());
    draw->AddText(ImVec2(10, 10), IM_COL32(255, 255, 255, 220), buf);

    float scale_info = (scl_x + scl_y) * 0.5f;
    if (nn_test_targets.size() > 0) {
        snprintf(buf, sizeof(buf), "缩放比: %.2fx  → 已归一化坐标显示", scale_info);
    } else {
        snprintf(buf, sizeof(buf), "缩放比: %.2fx  → 点击任意位置放置目标", scale_info);
    }
    draw->AddText(ImVec2(10, 30), IM_COL32(180, 180, 180, 200), buf);
}

void AimAssistant::HandleTestInput() {
    // ESC to exit
    static bool esc_was_down = false;
    bool esc_down = GetAsyncKeyState(VK_ESCAPE) & 0x8000;
    if (esc_down && !esc_was_down) {
        nn_test_mode = false;
        nn_test_targets.clear();
        nn_test_trajectories.clear();
        UpdateClickability();
        esc_was_down = true;
        return;
    }
    esc_was_down = esc_down;

    // Get cursor position relative to overlay
    POINT p;
    GetCursorPos(&p);
    ScreenToClient(overlay.GetHwnd(), &p);
    float mx = (float)p.x;
    float my = (float)p.y;

    float cx = overlay.GetWidth() / 2.0f;
    float cy = overlay.GetHeight() / 2.0f;
    float scl_x = (float)overlay.GetWidth() / (float)cfg.crop_size;
    float scl_y = (float)overlay.GetHeight() / (float)cfg.crop_size;

    // Left click: place target
    static bool lbtn_was_down = false;
    bool lbtn_down = GetAsyncKeyState(VK_LBUTTON) & 0x8000;
    if (lbtn_down && !lbtn_was_down) {
        // Compute dx, dy from center -> cursor in crop-pixel space
        float dx = (mx - cx) / scl_x;
        float dy = (my - cy) / scl_y;

        if (nn_test_targets.size() < 20) {
            nn_test_targets.push_back(cv::Point2f(mx, my));

            // Predict trajectory: match training data coordinate space
            float waypoints[MouseTrajectoryNN::OUTPUT_SIZE];
            {
                std::lock_guard<std::mutex> lock(nn_mutex);
                nn_model.Predict(dx, dy, waypoints);
            }

            // Check for NaN
            bool wp_ok = true;
            for (int i = 0; i < MouseTrajectoryNN::OUTPUT_SIZE && wp_ok; i++)
                if (!std::isfinite(waypoints[i])) wp_ok = false;

            if (!wp_ok) {
                nn_status = "模型输出无效，请重新训练";
                nn_test_targets.pop_back();  // remove invalid target
            } else {
                MouseTrajectoryNN::SmoothTrajectory(waypoints, MouseTrajectoryNN::WAYPOINT_COUNT, 3, 0.4f);
                waypoints[0] = 0.0f;
                waypoints[1] = 0.0f;
                waypoints[(MouseTrajectoryNN::WAYPOINT_COUNT - 1) * 2] = dx;
                waypoints[(MouseTrajectoryNN::WAYPOINT_COUNT - 1) * 2 + 1] = dy;

                // Convert back to overlay coordinates
                std::vector<cv::Point2f> traj;
                for (int i = 0; i < MouseTrajectoryNN::WAYPOINT_COUNT; i++) {
                    float wx = cx + waypoints[i * 2] * scl_x;
                    float wy = cy + waypoints[i * 2 + 1] * scl_y;
                    traj.push_back(cv::Point2f(wx, wy));
                }
                traj.back() = cv::Point2f(mx, my);
                nn_test_trajectories.push_back(traj);
            }
        }
    }
    lbtn_was_down = lbtn_down;

    // Right click: undo last target
    static bool rbtn_was_down = false;
    bool rbtn_down = GetAsyncKeyState(VK_RBUTTON) & 0x8000;
    if (rbtn_down && !rbtn_was_down) {
        if (!nn_test_targets.empty()) {
            nn_test_targets.pop_back();
            nn_test_trajectories.pop_back();
        }
    }
    rbtn_was_down = rbtn_down;
}

// ---------------------------------------------------------------------------
//  Sample save/load
// ---------------------------------------------------------------------------
void AimAssistant::SaveSamples() {
    std::string path = Config::GetConfigPath();
    path = path.substr(0, path.rfind('\\') + 1) + "nn_samples.bin";

    std::ofstream f(path, std::ios::binary);
    if (!f) return;

    uint32_t magic = 0x32504D53;  // "SMP2"
    uint32_t count = (uint32_t)nn_samples.size();
    f.write(reinterpret_cast<const char*>(&magic), 4);
    f.write(reinterpret_cast<const char*>(&count), 4);

    for (const auto& s : nn_samples) {
        f.write(reinterpret_cast<const char*>(&s), sizeof(s));
    }
}

int AimAssistant::LoadSamples() {
    std::string path = Config::GetConfigPath();
    path = path.substr(0, path.rfind('\\') + 1) + "nn_samples.bin";

    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;

    uint32_t magic = 0, count = 0;
    f.read(reinterpret_cast<char*>(&magic), 4);
    f.read(reinterpret_cast<char*>(&count), 4);
    if (magic != 0x32504D53 || count == 0) return 0;

    nn_samples.clear();
    nn_samples.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        MouseTrajectoryNN::Sample s;
        f.read(reinterpret_cast<char*>(&s), sizeof(s));
        if (f.good()) {
            s = MouseTrajectoryNN::NormalizeSample(s);
            if (MouseTrajectoryNN::ValidateSample(s, cfg.nn_norm_scale)) {
                nn_samples.push_back(s);
            }
        }
    }
    return (int)nn_samples.size();
}


