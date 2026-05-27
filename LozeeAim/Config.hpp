#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <string>
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <exception>

class Config {
    size_t last_hash = 0;

    size_t ComputeHash() const {
        size_t h = 0;
        auto mix = [&](const void* p, size_t n) {
            const auto* b = (const unsigned char*)p;
            for (size_t i = 0; i < n; i++) h = (h * 31) ^ b[i];
        };
        mix(&confidence_threshold, sizeof(confidence_threshold));
        mix(yolo_model_paths, sizeof(yolo_model_paths));
        mix(&yolo_model_idx, sizeof(yolo_model_idx));
        mix(&inference_provider, sizeof(inference_provider));
        mix(&nms_threshold, sizeof(nms_threshold));
        mix(&max_lock_distance_pixels, sizeof(max_lock_distance_pixels));
        mix(&smooth_aim_key, sizeof(smooth_aim_key));
        mix(&single_shot_key, sizeof(single_shot_key));
        mix(&aim_algorithm, sizeof(aim_algorithm));
        mix(&aim_smooth_time, sizeof(aim_smooth_time));
        mix(&aim_step, sizeof(aim_step));
        mix(&aim_min_step, sizeof(aim_min_step));
        mix(&target_y_ratio, sizeof(target_y_ratio));
        mix(&aim_target_smooth, sizeof(aim_target_smooth));
        mix(&aim_snap_enable, sizeof(aim_snap_enable));
        mix(&aim_snap_range, sizeof(aim_snap_range));
        mix(&aim_deadlock, sizeof(aim_deadlock));
        mix(&target_class_id, sizeof(target_class_id));
        mix(&target_team, sizeof(target_team));
        mix(&pid_kp, sizeof(pid_kp));
        mix(&pid_ki, sizeof(pid_ki));
        mix(&pid_kd, sizeof(pid_kd));
        mix(&pid_deadzone, sizeof(pid_deadzone));
        mix(&pid_max_step, sizeof(pid_max_step));
        mix(&aim_curve, sizeof(aim_curve));
        mix(&aim_humanize, sizeof(aim_humanize));
        mix(&aim_accel, sizeof(aim_accel));
        mix(&aim_max_step_far, sizeof(aim_max_step_far));
        mix(&aim_max_step_near, sizeof(aim_max_step_near));
        mix(&aim_far_threshold, sizeof(aim_far_threshold));
        mix(&aim_diagnostics_enable, sizeof(aim_diagnostics_enable));
        mix(&aim_diagnostics_text_enable, sizeof(aim_diagnostics_text_enable));
        mix(&aim_diagnostics_points_enable, sizeof(aim_diagnostics_points_enable));
        mix(&target_lock_min_frames, sizeof(target_lock_min_frames));
        mix(&target_switch_confirm_frames, sizeof(target_switch_confirm_frames));
        mix(&target_switch_margin, sizeof(target_switch_margin));
        mix(&target_prediction_enable, sizeof(target_prediction_enable));
        mix(&target_prediction_lead_ms, sizeof(target_prediction_lead_ms));
        mix(&target_prediction_smooth, sizeof(target_prediction_smooth));
        mix(&target_prediction_max_offset, sizeof(target_prediction_max_offset));
        mix(&target_crowd_prediction_scale, sizeof(target_crowd_prediction_scale));
        mix(&nn_trajectory_enable, sizeof(nn_trajectory_enable));
        mix(&nn_main_aim_enable, sizeof(nn_main_aim_enable));
        mix(&nn_wp_max_step, sizeof(nn_wp_max_step));
        mix(&nn_train_lr, sizeof(nn_train_lr));
        mix(&nn_train_momentum, sizeof(nn_train_momentum));
        mix(&nn_train_epochs, sizeof(nn_train_epochs));
        mix(&nn_train_batch, sizeof(nn_train_batch));
        mix(&nn_train_smooth, sizeof(nn_train_smooth));
        mix(&nn_weight_decay, sizeof(nn_weight_decay));
        mix(&nn_grad_clip, sizeof(nn_grad_clip));
        mix(&nn_aug_count, sizeof(nn_aug_count));
        mix(&nn_norm_scale, sizeof(nn_norm_scale));
        mix(&nn_collection_total, sizeof(nn_collection_total));
        mix(&use_cpu_inference, sizeof(use_cpu_inference));
        mix(nn_model_path, sizeof(nn_model_path));
        mix(&triggerbot_toggle_key, sizeof(triggerbot_toggle_key));
        mix(&trigger_hold_key, sizeof(trigger_hold_key));
        mix(&trigger_click_duration_ms, sizeof(trigger_click_duration_ms));
        mix(&trigger_enable, sizeof(trigger_enable));
        mix(&trigger_fire_rate_ms, sizeof(trigger_fire_rate_ms));
        mix(&trigger_delay_ms, sizeof(trigger_delay_ms));
        mix(&trigger_scale_x, sizeof(trigger_scale_x));
        mix(&trigger_scale_y, sizeof(trigger_scale_y));
        mix(&trigger_offset_y, sizeof(trigger_offset_y));
        mix(&this->sensitivity, sizeof(sensitivity));
        mix(&mouse_backend, sizeof(mouse_backend));
        mix(&pixels_for_360_turn, sizeof(pixels_for_360_turn));
        mix(&horizontal_fov, sizeof(horizontal_fov));
        mix(&enable_visualization, sizeof(enable_visualization));
        mix(&show_menu, sizeof(show_menu));
        mix(&enable_esp, sizeof(enable_esp));
        mix(&esp_draw_teammates, sizeof(esp_draw_teammates));
        mix(&esp_draw_boxes, sizeof(esp_draw_boxes));
        mix(&esp_show_confidence, sizeof(esp_show_confidence));
        mix(&show_fov_circle, sizeof(show_fov_circle));
        mix(&prevent_screen_capture, sizeof(prevent_screen_capture));
        mix(&ui_language, sizeof(ui_language));
        mix(fov_circle_color, sizeof(fov_circle_color));
        return h;
    }

public:
    // --- Model ---
    const wchar_t* model_path = L"models\\yolo\\yolo11s_cs2.onnx";
    char yolo_model_paths[512] = "";
    int  yolo_model_idx = 0;
    int  inference_provider = 0;       // 0=DirectML, 1=TensorRT, 2=CPU
    bool use_end_to_end_onnx = false;
    bool use_cpu_inference = false;     // skip GPU providers, run YOLO on CPU
    const char* trt_cache_path = ".\\engine_cache";

    // --- Detection ---
    const int crop_size = 640;

    float confidence_threshold = 0.5f;
    float nms_threshold = 0.4f;

    // --- Aim ---
    float max_lock_distance_pixels = 200.0f;
    int smooth_aim_key = VK_XBUTTON2;
    int single_shot_key = VK_F8;
    int aim_algorithm = 0;             // 0=adaptive, 1=linear, 2=spring, 3=neural network

    float aim_smooth_time = 0.10f;     // Smooth time (lower=faster)
    float aim_step = 0.20f;            // Unused, kept for compat
    int aim_min_step = 3;              // Unused, kept for compat
    float aim_target_smooth = 0.2f;    // Lower=smoother
    bool aim_snap_enable = true;       // Stop zone
    int aim_snap_range = 5;            // Stop zone range in pixels
    bool aim_deadlock = false;         // Instant snap (no smoothing)
    float target_y_ratio = 0.5f;
    int target_class_id = 1;
    int target_team = 2;               // 0=CT, 1=T, 2=All

    float aim_curve = 0.7f;            // Speed curve: <1 aggressive, >1 gentle (0.3-2.0)
    float aim_humanize = 0.5f;         // Humanization strength (0-1)
    float aim_accel = 0.5f;            // Acceleration weight (0=instant vel, 1=full accel)
    int aim_max_step_far = 160;        // Max step when far from target
    int aim_max_step_near = 30;        // Max step when near target
    float aim_far_threshold = 100.0f;  // Distance threshold for "far" vs "near"
    bool aim_diagnostics_enable = true;
    bool aim_diagnostics_text_enable = true;
    bool aim_diagnostics_points_enable = true;
    int target_lock_min_frames = 3;
    int target_switch_confirm_frames = 3;
    float target_switch_margin = 45.0f;
    bool target_prediction_enable = true;
    float target_prediction_lead_ms = 45.0f;
    float target_prediction_smooth = 0.45f;
    float target_prediction_max_offset = 42.0f;
    float target_crowd_prediction_scale = 0.45f;

    bool nn_trajectory_enable = false; // Use NN trajectory instead of bezier
    bool nn_main_aim_enable = false;   // Compatibility mirror for aim_algorithm == neural network
    int nn_wp_max_step = 30;           // max crop-pixels per waypoint step (0=use aim_max_step_near)
    float nn_train_lr = 0.001f;       // NN training learning rate
    float nn_train_momentum = 0.9f;   // NN training momentum
    int nn_train_epochs = 200;        // NN training epochs
    int nn_train_batch = 32;          // NN training batch size
    float nn_train_smooth = 0.1f;     // smoothness loss weight (lambda)
    float nn_weight_decay = 1e-5f;    // L2 regularization strength
    float nn_grad_clip = 1.0f;        // gradient clipping threshold
    int nn_aug_count = 200;           // synthetic Bezier samples to generate
    float nn_norm_scale = 1000.0f;    // normalization scale (pixels)
    int nn_collection_total = 300;    // total samples to collect
    char nn_model_path[260] = "models\\nn\\nn_model.onnx";

    // --- PID ---
    float pid_kp = 0.25f;
    float pid_ki = 0.0f;
    float pid_kd = 0.0f;
    int pid_deadzone = 5;              // Dead zone in pixels
    int pid_max_step = 80;             // Max mouse count per frame

    // --- Triggerbot ---
    int triggerbot_toggle_key = VK_F7;
    int trigger_hold_key = 0;
    int trigger_click_duration_ms = 80;
    bool trigger_enable = false;

    int trigger_fire_rate_ms = 150;
    int trigger_delay_ms = 20;

    float trigger_scale_x = 0.35f;
    float trigger_scale_y = 0.70f;
    float trigger_offset_y = 0.0f;

    // --- Mouse ---
    int mouse_backend = 0;             // 0=Win32API
    float sensitivity = 1.0f;
    float pixels_for_360_turn = 16410.0f;
    float horizontal_fov = 120.0f;

    // --- Visualization ---
    bool enable_visualization = false;
    const std::string window_name = "YOLO AI Detection Preview";

    // --- System (ImGui) ---
    bool show_menu = true;
    bool enable_esp = true;
    bool esp_draw_teammates = false;
    bool esp_draw_boxes = true;
    bool esp_show_confidence = true;
    bool show_fov_circle = true;
    bool prevent_screen_capture = false;
    int ui_language = 0;               // 0=Chinese, 1=English
    float fov_circle_color[4] = { 1.0f, 1.0f, 1.0f, 0.4f };

    static std::string GetConfigPath() {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string s(path);
        return s.substr(0, s.rfind('\\') + 1) + "global.ini";
    }

    void Save() {
        size_t current = ComputeHash();
        if (current == last_hash) return;
        last_hash = current;

        std::string cfgPath = GetConfigPath();
        std::ofstream f(cfgPath);
        if (!f) return;
        f << "confidence_threshold=" << confidence_threshold << "\n";
        f << "yolo_model_paths=" << yolo_model_paths << "\n";
        f << "yolo_model_idx=" << yolo_model_idx << "\n";
        f << "inference_provider=" << inference_provider << "\n";
        f << "nms_threshold=" << nms_threshold << "\n";
        f << "max_lock_distance_pixels=" << max_lock_distance_pixels << "\n";
        f << "smooth_aim_key=" << smooth_aim_key << "\n";
        f << "single_shot_key=" << single_shot_key << "\n";
        f << "aim_algorithm=" << aim_algorithm << "\n";
        f << "aim_smooth_time=" << aim_smooth_time << "\n";
        f << "aim_step=" << aim_step << "\n";
        f << "aim_min_step=" << aim_min_step << "\n";
        f << "target_y_ratio=" << target_y_ratio << "\n";
        f << "aim_target_smooth=" << aim_target_smooth << "\n";
        f << "aim_snap_enable=" << aim_snap_enable << "\n";
        f << "aim_snap_range=" << aim_snap_range << "\n";
        f << "aim_deadlock=" << aim_deadlock << "\n";
        f << "target_class_id=" << target_class_id << "\n";
        f << "target_team=" << target_team << "\n";
        f << "pid_kp=" << pid_kp << "\n";
        f << "pid_ki=" << pid_ki << "\n";
        f << "pid_kd=" << pid_kd << "\n";
        f << "pid_deadzone=" << pid_deadzone << "\n";
        f << "pid_max_step=" << pid_max_step << "\n";
        f << "aim_curve=" << aim_curve << "\n";
        f << "aim_humanize=" << aim_humanize << "\n";
        f << "aim_accel=" << aim_accel << "\n";
        f << "aim_max_step_far=" << aim_max_step_far << "\n";
        f << "aim_max_step_near=" << aim_max_step_near << "\n";
        f << "aim_far_threshold=" << aim_far_threshold << "\n";
        f << "aim_diagnostics_enable=" << aim_diagnostics_enable << "\n";
        f << "aim_diagnostics_text_enable=" << aim_diagnostics_text_enable << "\n";
        f << "aim_diagnostics_points_enable=" << aim_diagnostics_points_enable << "\n";
        f << "target_lock_min_frames=" << target_lock_min_frames << "\n";
        f << "target_switch_confirm_frames=" << target_switch_confirm_frames << "\n";
        f << "target_switch_margin=" << target_switch_margin << "\n";
        f << "target_prediction_enable=" << target_prediction_enable << "\n";
        f << "target_prediction_lead_ms=" << target_prediction_lead_ms << "\n";
        f << "target_prediction_smooth=" << target_prediction_smooth << "\n";
        f << "target_prediction_max_offset=" << target_prediction_max_offset << "\n";
        f << "target_crowd_prediction_scale=" << target_crowd_prediction_scale << "\n";
        f << "nn_trajectory_enable=" << nn_trajectory_enable << "\n";
        f << "nn_main_aim_enable=" << nn_main_aim_enable << "\n";
        f << "nn_wp_max_step=" << nn_wp_max_step << "\n";
        f << "use_cpu_inference=" << use_cpu_inference << "\n";
        f << "nn_train_lr=" << nn_train_lr << "\n";
        f << "nn_train_momentum=" << nn_train_momentum << "\n";
        f << "nn_train_epochs=" << nn_train_epochs << "\n";
        f << "nn_train_batch=" << nn_train_batch << "\n";
        f << "nn_train_smooth=" << nn_train_smooth << "\n";
        f << "nn_weight_decay=" << nn_weight_decay << "\n";
        f << "nn_grad_clip=" << nn_grad_clip << "\n";
        f << "nn_aug_count=" << nn_aug_count << "\n";
        f << "nn_norm_scale=" << nn_norm_scale << "\n";
        f << "nn_collection_total=" << nn_collection_total << "\n";
        f << "nn_model_path=" << nn_model_path << "\n";
        f << "triggerbot_toggle_key=" << triggerbot_toggle_key << "\n";
        f << "trigger_hold_key=" << trigger_hold_key << "\n";
        f << "trigger_click_duration_ms=" << trigger_click_duration_ms << "\n";
        f << "trigger_enable=" << trigger_enable << "\n";
        f << "trigger_fire_rate_ms=" << trigger_fire_rate_ms << "\n";
        f << "trigger_delay_ms=" << trigger_delay_ms << "\n";
        f << "trigger_scale_x=" << trigger_scale_x << "\n";
        f << "trigger_scale_y=" << trigger_scale_y << "\n";
        f << "trigger_offset_y=" << trigger_offset_y << "\n";
        f << "sensitivity=" << this->sensitivity << "\n";
        f << "mouse_backend=" << mouse_backend << "\n";
        f << "pixels_for_360_turn=" << pixels_for_360_turn << "\n";
        f << "horizontal_fov=" << horizontal_fov << "\n";
        f << "enable_visualization=" << enable_visualization << "\n";
        f << "show_menu=" << show_menu << "\n";
        f << "enable_esp=" << enable_esp << "\n";
        f << "esp_draw_teammates=" << esp_draw_teammates << "\n";
        f << "esp_draw_boxes=" << esp_draw_boxes << "\n";
        f << "esp_show_confidence=" << esp_show_confidence << "\n";
        f << "show_fov_circle=" << show_fov_circle << "\n";
        f << "prevent_screen_capture=" << prevent_screen_capture << "\n";
        f << "ui_language=" << ui_language << "\n";
        f << "fov_circle_color=" << fov_circle_color[0] << "," << fov_circle_color[1] << "," << fov_circle_color[2] << "," << fov_circle_color[3] << "\n";
    }

    void Load() {
        std::string cfgPath = GetConfigPath();
        std::ifstream f(cfgPath);
        if (!f) return;

        std::string line;
        bool has_aim_algorithm = false;
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);

            try {
            if (key == "confidence_threshold") confidence_threshold = std::stof(val);
            else if (key == "yolo_model_paths") strncpy_s(yolo_model_paths, val.c_str(), sizeof(yolo_model_paths) - 1);
            else if (key == "yolo_model_idx") yolo_model_idx = std::stoi(val);
            else if (key == "inference_provider") inference_provider = std::stoi(val);
            else if (key == "nms_threshold") nms_threshold = std::stof(val);
            else if (key == "max_lock_distance_pixels") max_lock_distance_pixels = std::stof(val);
            else if (key == "smooth_aim_key") smooth_aim_key = std::stoi(val);
            else if (key == "single_shot_key") single_shot_key = std::stoi(val);
            else if (key == "aim_algorithm") {
                aim_algorithm = std::stoi(val);
                has_aim_algorithm = true;
            }
            else if (key == "aim_smooth_time") aim_smooth_time = std::stof(val);
            else if (key == "aim_step") aim_step = std::stof(val);
            else if (key == "aim_min_step") aim_min_step = std::stoi(val);
            else if (key == "target_y_ratio") target_y_ratio = std::stof(val);
            else if (key == "aim_target_smooth") aim_target_smooth = std::stof(val);
            else if (key == "aim_snap_enable") aim_snap_enable = (val == "1");
            else if (key == "aim_snap_range") aim_snap_range = std::stoi(val);
            else if (key == "aim_deadlock") aim_deadlock = (val == "1");
            else if (key == "target_class_id") target_class_id = std::stoi(val);
            else if (key == "target_team") target_team = std::stoi(val);
            else if (key == "pid_kp") pid_kp = std::stof(val);
            else if (key == "pid_ki") pid_ki = std::stof(val);
            else if (key == "pid_kd") pid_kd = std::stof(val);
            else if (key == "pid_deadzone") pid_deadzone = std::stoi(val);
            else if (key == "pid_max_step") pid_max_step = std::stoi(val);
            else if (key == "aim_curve") aim_curve = std::stof(val);
            else if (key == "aim_humanize") aim_humanize = std::stof(val);
            else if (key == "aim_accel") aim_accel = std::stof(val);
            else if (key == "aim_max_step_far") aim_max_step_far = std::stoi(val);
            else if (key == "aim_max_step_near") aim_max_step_near = std::stoi(val);
            else if (key == "aim_far_threshold") aim_far_threshold = std::stof(val);
            else if (key == "aim_diagnostics_enable") aim_diagnostics_enable = (val == "1");
            else if (key == "aim_diagnostics_text_enable") aim_diagnostics_text_enable = (val == "1");
            else if (key == "aim_diagnostics_points_enable") aim_diagnostics_points_enable = (val == "1");
            else if (key == "target_lock_min_frames") target_lock_min_frames = std::stoi(val);
            else if (key == "target_switch_confirm_frames") target_switch_confirm_frames = std::stoi(val);
            else if (key == "target_switch_margin") target_switch_margin = std::stof(val);
            else if (key == "target_prediction_enable") target_prediction_enable = (val == "1");
            else if (key == "target_prediction_lead_ms") target_prediction_lead_ms = std::stof(val);
            else if (key == "target_prediction_smooth") target_prediction_smooth = std::stof(val);
            else if (key == "target_prediction_max_offset") target_prediction_max_offset = std::stof(val);
            else if (key == "target_crowd_prediction_scale") target_crowd_prediction_scale = std::stof(val);
            else if (key == "nn_trajectory_enable") nn_trajectory_enable = (val == "1");
            else if (key == "nn_main_aim_enable") nn_main_aim_enable = (val == "1");
            else if (key == "nn_wp_max_step") nn_wp_max_step = std::stoi(val);
            else if (key == "use_cpu_inference") use_cpu_inference = (val == "1");
            else if (key == "nn_train_lr") nn_train_lr = std::stof(val);
            else if (key == "nn_train_momentum") nn_train_momentum = std::stof(val);
            else if (key == "nn_train_epochs") nn_train_epochs = std::stoi(val);
            else if (key == "nn_train_batch") nn_train_batch = std::stoi(val);
            else if (key == "nn_train_smooth") nn_train_smooth = std::stof(val);
            else if (key == "nn_weight_decay") nn_weight_decay = std::stof(val);
            else if (key == "nn_grad_clip") nn_grad_clip = std::stof(val);
            else if (key == "nn_aug_count") nn_aug_count = std::stoi(val);
            else if (key == "nn_norm_scale") nn_norm_scale = std::stof(val);
            else if (key == "nn_collection_total") nn_collection_total = std::stoi(val);
            else if (key == "nn_model_path") strncpy_s(nn_model_path, val.c_str(), sizeof(nn_model_path) - 1);
            else if (key == "triggerbot_toggle_key") triggerbot_toggle_key = std::stoi(val);
            else if (key == "trigger_hold_key") trigger_hold_key = std::stoi(val);
            else if (key == "trigger_click_duration_ms") trigger_click_duration_ms = std::stoi(val);
            else if (key == "trigger_enable") trigger_enable = (val == "1");
            else if (key == "trigger_fire_rate_ms") trigger_fire_rate_ms = std::stoi(val);
            else if (key == "trigger_delay_ms") trigger_delay_ms = std::stoi(val);
            else if (key == "trigger_scale_x") trigger_scale_x = std::stof(val);
            else if (key == "trigger_scale_y") trigger_scale_y = std::stof(val);
            else if (key == "trigger_offset_y") trigger_offset_y = std::stof(val);
            else if (key == "sensitivity") sensitivity = std::stof(val);
            else if (key == "mouse_backend") mouse_backend = std::stoi(val);
            else if (key == "pixels_for_360_turn") pixels_for_360_turn = std::stof(val);
            else if (key == "horizontal_fov") horizontal_fov = std::stof(val);
            else if (key == "enable_visualization") enable_visualization = (val == "1");
            else if (key == "show_menu") show_menu = (val == "1");
            else if (key == "enable_esp") enable_esp = (val == "1");
            else if (key == "esp_draw_teammates") esp_draw_teammates = (val == "1");
            else if (key == "esp_draw_boxes") esp_draw_boxes = (val == "1");
            else if (key == "esp_show_confidence") esp_show_confidence = (val == "1");
            else if (key == "show_fov_circle") show_fov_circle = (val == "1");
            else if (key == "prevent_screen_capture") prevent_screen_capture = (val == "1");
            else if (key == "ui_language") ui_language = std::stoi(val);
            else if (key == "fov_circle_color") {
                std::stringstream ss(val);
                std::string token;
                for (int i = 0; i < 4 && std::getline(ss, token, ','); i++)
                    fov_circle_color[i] = std::stof(token);
            }
            } catch (const std::exception&) {
                continue;
            }
        }
        if (!has_aim_algorithm && nn_main_aim_enable) aim_algorithm = 3;
        if (aim_algorithm < 0 || aim_algorithm > 3) aim_algorithm = 0;
        if (inference_provider == 2) {
            use_cpu_inference = true;
            inference_provider = 0;
        }
        if (inference_provider < 0 || inference_provider > 1) inference_provider = 0;
        if (use_cpu_inference) inference_provider = 0;
        nn_main_aim_enable = (aim_algorithm == 3);
        target_lock_min_frames = std::max(1, std::min(target_lock_min_frames, 10));
        target_switch_confirm_frames = std::max(1, std::min(target_switch_confirm_frames, 10));
        target_switch_margin = std::max(0.0f, std::min(target_switch_margin, 180.0f));
        target_crowd_prediction_scale = std::max(0.0f, std::min(target_crowd_prediction_scale, 1.0f));
        mouse_backend = 0;
        ui_language = std::max(0, std::min(ui_language, 1));
        last_hash = ComputeHash();
    }
};

