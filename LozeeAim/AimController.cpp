#include "AimController.hpp"

#include <algorithm>
#include <cmath>

namespace {
float Clamp(float value, float low, float high) {
    return std::max(low, std::min(value, high));
}

float SmoothStep(float edge0, float edge1, float value) {
    if (edge1 <= edge0) return value >= edge1 ? 1.0f : 0.0f;
    float t = Clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

bool ClampVector(float& x, float& y, float max_length) {
    const float length = std::hypot(x, y);
    if (length <= max_length || length <= 0.0001f) return false;
    const float scale = max_length / length;
    x *= scale;
    y *= scale;
    return true;
}

float SmoothDampDelta(float error, float& velocity, float smooth_time, float dt) {
    const float time = std::max(0.0001f, smooth_time);
    const float omega = 2.0f / time;
    const float x = omega * dt;
    const float exp = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    const float temp = (velocity + omega * error) * dt;
    velocity = (velocity - omega * temp) * exp;
    const float next_error = (error + temp) * exp;
    return error - next_error;
}
}

std::optional<AimController::Output> AimController::Update(const Config& cfg, const Input& input) {
    const float dt = Clamp(static_cast<float>(input.dt), 0.001f, 0.050f);
    const float raw_x = static_cast<float>(input.target_point.x);
    const float raw_y = static_cast<float>(input.target_point.y);
    int algorithm = std::max(0, std::min(cfg.aim_algorithm, AlgorithmCount - 1));
    if (algorithm == AlgorithmNeuralNetwork) {
        algorithm = AlgorithmAdaptive;
    }

    if (last_algorithm != algorithm) {
        ResetMotionState();
        last_algorithm = algorithm;
    }

    if (!initialized) {
        initialized = true;
        smoothed_x = raw_x;
        smoothed_y = raw_y;
        step_x = 0.0f;
        step_y = 0.0f;
        spring_vx = 0.0f;
        spring_vy = 0.0f;
        fractional_x = 0.0f;
        fractional_y = 0.0f;
    }

    const float target_smooth = Clamp(cfg.aim_target_smooth, 0.0f, 1.0f);
    const float target_tau = 0.015f + (1.0f - target_smooth) * 0.080f;
    const float base_alpha = 1.0f - std::exp(-dt / target_tau);
    const float raw_delta = std::hypot(raw_x - smoothed_x, raw_y - smoothed_y);
    const float speed_alpha = Clamp(raw_delta / 60.0f, 0.0f, 0.55f);
    const float alpha = Clamp(base_alpha + speed_alpha, base_alpha, 0.90f);
    smoothed_x += (raw_x - smoothed_x) * alpha;
    smoothed_y += (raw_y - smoothed_y) * alpha;

    const float humanize = Clamp(cfg.aim_humanize, 0.0f, 1.0f);
    if (humanize > 0.001f) {
        humanize_timer += dt;
        const float interval = 0.25f + 0.45f * humanize;
        if (humanize_timer >= interval) {
            humanize_timer = 0.0f;
            std::uniform_real_distribution<float> jitter(-1.0f, 1.0f);
            const float range = humanize * 3.0f;
            humanize_target_x = jitter(rng) * range;
            humanize_target_y = jitter(rng) * range;
        }
        const float humanize_alpha = 1.0f - std::exp(-dt / std::max(0.040f, interval * 0.45f));
        humanize_x += (humanize_target_x - humanize_x) * humanize_alpha;
        humanize_y += (humanize_target_y - humanize_y) * humanize_alpha;
    } else {
        humanize_x = 0.0f;
        humanize_y = 0.0f;
        humanize_target_x = 0.0f;
        humanize_target_y = 0.0f;
        humanize_timer = 0.0f;
    }

    const float error_x = smoothed_x + humanize_x - static_cast<float>(input.center.x);
    const float error_y = smoothed_y + humanize_y - static_cast<float>(input.center.y);
    const float distance = std::hypot(error_x, error_y);

    const float base_deadzone = cfg.aim_snap_enable
        ? static_cast<float>(cfg.aim_snap_range)
        : static_cast<float>(cfg.pid_deadzone);
    const float deadzone = std::max(0.75f, std::min(base_deadzone, input.target_width * 0.08f));
    if (distance <= deadzone) {
        step_x = 0.0f;
        step_y = 0.0f;
        spring_vx = 0.0f;
        spring_vy = 0.0f;
        fractional_x = 0.0f;
        fractional_y = 0.0f;
        return std::nullopt;
    }

    const float smooth_time = Clamp(cfg.aim_smooth_time, 0.015f, 0.350f);
    const float response = 1.0f - std::exp(-dt / smooth_time);
    const float fov = std::max(1.0f, cfg.max_lock_distance_pixels);
    const float normalized = Clamp((distance - deadzone) / std::max(1.0f, fov - deadzone), 0.0f, 1.0f);
    const float curve = Clamp(cfg.aim_curve, 0.25f, 2.50f);
    const float curve_gain = std::pow(normalized, curve);
    const float near_end = std::max(deadzone * 7.0f, deadzone + 28.0f);
    const float near_alpha = SmoothStep(deadzone, near_end, distance);
    const float near_gain = (0.50f + 0.50f * near_alpha) * (1.0f + (1.0f - near_alpha) * 0.35f);
    const float accel = Clamp(cfg.aim_accel, 0.0f, 1.0f);

    switch (algorithm) {
    case AlgorithmLinear: {
        const float gain = response * (0.80f + 0.55f * curve_gain) * near_gain;
        step_x = error_x * gain;
        step_y = error_y * gain;
        spring_vx = 0.0f;
        spring_vy = 0.0f;
        break;
    }
    case AlgorithmSpring: {
        const float response_scale = 0.80f + 0.75f * curve_gain;
        const float damping_scale = 0.70f + 0.90f * accel;
        const float spring_time = Clamp(smooth_time * damping_scale / response_scale, 0.018f, 0.450f);
        step_x = SmoothDampDelta(error_x, spring_vx, spring_time, dt);
        step_y = SmoothDampDelta(error_y, spring_vy, spring_time, dt);
        if (near_gain < 0.999f) {
            step_x *= near_gain;
            step_y *= near_gain;
            spring_vx *= near_gain;
            spring_vy *= near_gain;
        }
        break;
    }
    case AlgorithmAdaptive:
    default: {
        const float gain = response * (0.55f + 0.85f * curve_gain) * near_gain;
        const float desired_x = error_x * gain;
        const float desired_y = error_y * gain;
        const float accel_tau = 0.004f + 0.055f * accel;
        const float accel_alpha = 1.0f - std::exp(-dt / accel_tau);
        step_x += (desired_x - step_x) * accel_alpha;
        step_y += (desired_y - step_y) * accel_alpha;
        spring_vx = 0.0f;
        spring_vy = 0.0f;
        break;
    }
    }

    bool motion_clamped = ClampVector(
        step_x,
        step_y,
        std::max(0.0f, distance - deadzone * 0.50f));

    const float far_distance = std::max(10.0f, cfg.aim_far_threshold);
    const float range_fraction = Clamp(distance / far_distance, 0.0f, 1.0f);
    const float max_mouse_step = std::max(
        1.0f,
        cfg.aim_max_step_near +
            (cfg.aim_max_step_far - cfg.aim_max_step_near) * range_fraction);
    const float conversion = std::max(
        0.001f,
        static_cast<float>(input.mouse_correction) * cfg.sensitivity);
    const float max_crop_step = max_mouse_step / conversion;
    motion_clamped = ClampVector(step_x, step_y, max_crop_step) || motion_clamped;
    if (algorithm == AlgorithmSpring && motion_clamped) {
        spring_vx = -step_x / dt;
        spring_vy = -step_y / dt;
    }

    const float exact_x = step_x * conversion + fractional_x;
    const float exact_y = step_y * conversion + fractional_y;
    const int dx = static_cast<int>(std::lround(exact_x));
    const int dy = static_cast<int>(std::lround(exact_y));

    fractional_x = exact_x - static_cast<float>(dx);
    fractional_y = exact_y - static_cast<float>(dy);

    if (dx == 0 && dy == 0) {
        return std::nullopt;
    }

    return Output{dx, dy};
}

void AimController::ResetMotionState() {
    initialized = false;
    smoothed_x = 0.0f;
    smoothed_y = 0.0f;
    step_x = 0.0f;
    step_y = 0.0f;
    spring_vx = 0.0f;
    spring_vy = 0.0f;
    fractional_x = 0.0f;
    fractional_y = 0.0f;
    humanize_x = 0.0f;
    humanize_y = 0.0f;
    humanize_target_x = 0.0f;
    humanize_target_y = 0.0f;
    humanize_timer = 0.0f;
}

void AimController::Reset() {
    last_algorithm = -1;
    ResetMotionState();
}
