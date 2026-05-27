#include "TargetPredictor.hpp"

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

#include <algorithm>
#include <cmath>

namespace {

float ClampFloat(float value, float min_value, float max_value) {
    return std::max(min_value, std::min(value, max_value));
}

cv::Point2f BoxCenter(const cv::Rect& box) {
    return cv::Point2f(
        static_cast<float>(box.x) + static_cast<float>(box.width) * 0.5f,
        static_cast<float>(box.y) + static_cast<float>(box.height) * 0.5f);
}

float Distance(const cv::Point2f& a, const cv::Point2f& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

float IntersectionOverUnion(const cv::Rect& a, const cv::Rect& b) {
    const int left = std::max(a.x, b.x);
    const int top = std::max(a.y, b.y);
    const int right = std::min(a.x + a.width, b.x + b.width);
    const int bottom = std::min(a.y + a.height, b.y + b.height);
    const int width = std::max(0, right - left);
    const int height = std::max(0, bottom - top);
    const float intersection = static_cast<float>(width * height);
    const float area = static_cast<float>(a.area() + b.area()) - intersection;
    return area > 0.0f ? intersection / area : 0.0f;
}

bool IsSameTarget(const cv::Rect& current, const cv::Rect& previous) {
    const float iou = IntersectionOverUnion(current, previous);
    const float center_distance = Distance(BoxCenter(current), BoxCenter(previous));
    const float target_scale = static_cast<float>(std::max(previous.width, previous.height));
    const float track_radius = std::max(48.0f, target_scale * 0.85f);
    return iou >= 0.04f || center_distance <= track_radius;
}

cv::Point2f ClampVector(cv::Point2f value, float max_length) {
    const float length = std::sqrt(value.x * value.x + value.y * value.y);
    if (length <= max_length || length <= 0.001f) {
        return value;
    }
    const float scale = max_length / length;
    return cv::Point2f(value.x * scale, value.y * scale);
}

} // namespace

cv::Point TargetPredictor::Update(
    const Detection& target,
    const cv::Point& raw_point,
    const Config& cfg,
    double dt,
    float lead_scale) {

    if (!cfg.target_prediction_enable) {
        Reset();
        return raw_point;
    }

    const float clamped_dt = ClampFloat(static_cast<float>(dt), 0.001f, 0.050f);
    const cv::Point2f current_point(
        static_cast<float>(raw_point.x),
        static_cast<float>(raw_point.y));

    if (!initialized || !IsSameTarget(target.box, last_box)) {
        initialized = true;
        last_point = current_point;
        velocity = cv::Point2f(0.0f, 0.0f);
        last_box = target.box;
        return raw_point;
    }

    const cv::Point2f measured_velocity(
        (current_point.x - last_point.x) / clamped_dt,
        (current_point.y - last_point.y) / clamped_dt);

    const float smooth = ClampFloat(cfg.target_prediction_smooth, 0.0f, 1.0f);
    const float tau = 0.010f + (1.0f - smooth) * 0.110f;
    const float alpha = ClampFloat(1.0f - std::exp(-clamped_dt / tau), 0.0f, 0.85f);
    velocity.x += (measured_velocity.x - velocity.x) * alpha;
    velocity.y += (measured_velocity.y - velocity.y) * alpha;

    last_point = current_point;
    last_box = target.box;

    const float lead_seconds = ClampFloat(cfg.target_prediction_lead_ms, 0.0f, 120.0f)
        * ClampFloat(lead_scale, 0.0f, 1.0f)
        / 1000.0f;
    cv::Point2f offset(velocity.x * lead_seconds, velocity.y * lead_seconds);

    const float target_bound = std::max(6.0f, static_cast<float>(target.box.width) * 0.65f);
    const float max_offset = std::max(0.0f, std::min(cfg.target_prediction_max_offset, target_bound));
    offset = ClampVector(offset, max_offset);

    return cv::Point(
        static_cast<int>(std::lround(current_point.x + offset.x)),
        static_cast<int>(std::lround(current_point.y + offset.y)));
}

void TargetPredictor::Reset() {
    initialized = false;
    last_point = cv::Point2f(0.0f, 0.0f);
    velocity = cv::Point2f(0.0f, 0.0f);
    last_box = cv::Rect();
}
