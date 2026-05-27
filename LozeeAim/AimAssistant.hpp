#pragma once
#include "Config.hpp"
#include "ScreenCapturer.hpp"
#include "ObjectDetector.hpp"
#include "MouseController.hpp"
#include "Overlay.hpp"
#include "Common.hpp"
#include "MouseTrajectoryNN.hpp"
#include "TargetSelector.hpp"
#include "AimController.hpp"
#include "TargetPredictor.hpp"
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>

// SysKeyHook — calls win32u!NtUserGetAsyncKeyState directly to bypass user-mode hooks
struct SysKeyHook {
    static void Init();
    static bool IsDown(int vk);
private:
    using NtGetKey_t = SHORT (WINAPI*)(int);
    static NtGetKey_t s_ntGetKey;
    static bool s_useNative;
};

class AimAssistant {
public:
    AimAssistant();
    ~AimAssistant();

    void Run();

private:
    void UpdateClickability();
    Detection* findBestTarget();
    void handleMouseInput(const Detection* target, double dt);
    void handleVisualization(const Detection* best_target);

    void DrawUI();
    void DrawTopBar();
    void DrawOverviewTab();
    void DrawAimTab();
    void DrawTriggerTab();
    void DrawVisualTab();
    void DrawModelTab();
    void DrawESP(const Detection* best_target);
    void DrawTrainingTab();
    void DrawAboutTab();
    void DrawCollectOverlay();
    void HandleCollectionInput();
    void DrawTestOverlay();
    void HandleTestInput();

    void handleTriggerbot();

    cv::Point getTargetPoint(const Detection* target) const;
    cv::Rect getTriggerHitbox(const Detection& det) const;
    bool isTriggerTarget(const Detection& det) const;
    bool isAimingAtTriggerTarget() const;

    void StartTraining();
    void RunNNTrajectory(const Detection* target);
    bool BeginNNTrajectory(float error_x, float error_y, bool single_shot);
    bool StepNNTrajectory(double dt);
    void SaveSamples();
    int  LoadSamples();
    void SetYoloReloadStatus(const std::string& status);
    std::string GetYoloReloadStatus() const;
    void StartDetectorLoad();
    void JoinFinishedDetectorLoad();
    void DrawDetectorStatusOverlay();
    void SetDetectorStatus(const std::string& status);
    std::string GetDetectorStatus() const;
    bool IsDetectorReady() const;
    static int VkToHidButton(int vk);
    bool IsKeyDown(int vk);

    bool exit_requested = false;

    Config cfg;
    ScreenCapturer capturer;
    std::unique_ptr<ObjectDetector> detector;
    MouseController mouse;
    TargetSelector target_selector;
    TargetPredictor target_predictor;
    AimController aim_controller;

    Overlay overlay;

    const cv::Rect crop_region;
    const cv::Point crop_center;
    double mouse_correction_factor = 1.0;

    cv::Mat captured_frame;
    std::vector<Detection> detections;
    TimingDetails timings;

    struct AimDebugInfo {
        bool has_target = false;
        bool prediction_used = false;
        bool prediction_gated = false;
        bool target_switched = false;
        bool crowded = false;
        bool stale_frame = false;
        int lock_frames = 0;
        int crowd_count = 0;
        int stale_frames = 0;
        int output_dx = 0;
        int output_dy = 0;
        cv::Point raw_point{0, 0};
        cv::Point aim_point{0, 0};
    } aim_debug;

    float target_smoothed_x = 0, target_smoothed_y = 0;
    bool target_smooth_init = false;

    float vel_x = 0.0f, vel_y = 0.0f;
    float humanize_ox = 0.0f, humanize_oy = 0.0f;
    float humanize_target_ox = 0.0f, humanize_target_oy = 0.0f;
    float humanize_timer = 0.0f;

    float nn_waypoints[MouseTrajectoryNN::OUTPUT_SIZE] = {};
    float nn_elapsed = 0.0f;
    float nn_sample_x = 0.0f, nn_sample_y = 0.0f;
    float nn_mouse_fractional_x = 0.0f, nn_mouse_fractional_y = 0.0f;
    bool nn_path_active = false;
    bool nn_path_single_shot = false;
    bool nn_debug_once = true;

    std::chrono::steady_clock::time_point last_shot_time;
    bool trigger_pending = false;
    std::chrono::steady_clock::time_point trigger_fire_time;

    MouseTrajectoryNN nn_model;
    std::vector<MouseTrajectoryNN::Sample> nn_samples;
    bool nn_collecting = false;

    // YOLO hot-reload
    std::atomic<bool> yolo_reload_pending{false};
    std::atomic<bool> yolo_reloading{false};
    std::string yolo_reload_status;
    std::wstring yolo_reload_path;
    std::thread yolo_reload_thread;
    mutable std::mutex yolo_mutex;
    std::atomic<bool> detector_loading{false};
    std::atomic<bool> detector_ready{false};
    std::atomic<bool> detector_failed{false};
    std::string detector_status;
    std::thread detector_load_thread;
    mutable std::mutex detector_mutex;
    int nn_collection_total = 300;
    std::string nn_status = "";
    std::string nn_diag = "";

    // FPS-style collection state
    bool nn_collect_waiting_start = true;
    bool nn_collect_recording = false;
    float nn_collect_target_x = 0, nn_collect_target_y = 0;
    float nn_collect_target_radius = 30.0f;
    std::vector<cv::Point2f> nn_collect_trail;
    std::chrono::steady_clock::time_point nn_collect_last_sample;
    cv::Point2f nn_collect_last_mouse;
    int nn_collect_hit_flash = 0;
    int nn_collect_miss_flash = 0;
    int nn_collect_reject_flash = 0;
    int nn_collect_session_hits = 0;
    int nn_collect_session_misses = 0;
    int nn_collect_session_rejected = 0;
    std::string nn_collect_feedback = "";
    std::vector<cv::Point2f> nn_collect_history;
    static constexpr int COLLECT_TRAIL_MAX = 800;

    // FPS camera rotation
    float nn_collect_cam_yaw = 0.0f;          // radians, horizontal
    float nn_collect_cam_pitch = 0.0f;        // radians, vertical
    float nn_collect_fov = 1.5708f;           // 90 deg in radians
    float nn_collect_mouse_sens = 0.002f;     // radians per pixel
    float nn_collect_world_dist = 500.0f;     // distance to target plane
    bool  nn_collect_cursor_locked = false;   // cursor hidden + warped
    float nn_collect_accum_dx = 0.0f;         // accumulated mouse delta during recording
    float nn_collect_accum_dy = 0.0f;
    std::atomic<bool> nn_training{false};
    float nn_train_progress = 0.0f;
    std::atomic<float> nn_train_progress_atomic{0.0f};
    std::string nn_train_status = "";
    std::string nn_train_status_internal = "";
    std::thread nn_train_thread;
    std::mutex nn_mutex;

    // Trajectory test mode
    bool nn_test_mode = false;
    std::vector<cv::Point2f> nn_test_targets;
    std::vector<std::vector<cv::Point2f>> nn_test_trajectories;
};
