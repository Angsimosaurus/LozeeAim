#include "TargetSelector.hpp"

#include <algorithm>
#include <cmath>

namespace {
cv::Point2f BoxCenter(const cv::Rect& box) {
    return cv::Point2f(
        box.x + box.width * 0.5f,
        box.y + box.height * 0.5f);
}

float Distance(cv::Point2f a, cv::Point2f b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

float IntersectionOverUnion(const cv::Rect& a, const cv::Rect& b) {
    const int x1 = std::max(a.x, b.x);
    const int y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + a.width, b.x + b.width);
    const int y2 = std::min(a.y + a.height, b.y + b.height);
    const int w = std::max(0, x2 - x1);
    const int h = std::max(0, y2 - y1);
    const int intersection = w * h;
    const int area_a = std::max(0, a.width) * std::max(0, a.height);
    const int area_b = std::max(0, b.width) * std::max(0, b.height);
    const int union_area = area_a + area_b - intersection;
    if (union_area <= 0) return 0.0f;
    return static_cast<float>(intersection) / static_cast<float>(union_area);
}

bool IsSameCandidate(const cv::Rect& a, const cv::Rect& b) {
    if (a.width <= 0 || a.height <= 0 || b.width <= 0 || b.height <= 0) {
        return false;
    }

    const float iou = IntersectionOverUnion(a, b);
    const float center_distance = Distance(BoxCenter(a), BoxCenter(b));
    const float target_scale = static_cast<float>(std::max(b.width, b.height));
    return iou >= 0.08f || center_distance <= std::max(36.0f, target_scale * 0.65f);
}
}

bool TargetSelector::Matches(const Config& cfg, int class_id) {
    const bool is_body = (class_id % 2 == 0);
    const bool is_t = (class_id >= 2);
    if (cfg.target_team == 0 && is_t) return false;
    if (cfg.target_team == 1 && !is_t) return false;
    if (cfg.target_class_id == 0 && is_body) return false;
    if (cfg.target_class_id == 1 && !is_body) return false;
    return true;
}

Detection* TargetSelector::Select(
    std::vector<Detection>& detections,
    const Config& cfg,
    const cv::Point& crop_center) {

    target_switched = false;
    crowd_count = 0;

    const cv::Point2f crosshair(
        static_cast<float>(crop_center.x),
        static_cast<float>(crop_center.y));

    Detection* best = nullptr;
    float best_score = cfg.max_lock_distance_pixels;

    Detection* tracked = nullptr;
    float tracked_score = 0.0f;
    float tracked_distance = 0.0f;
    float best_track_score = 1.0e9f;

    const cv::Point2f last_center = BoxCenter(last_target_box);
    const float fov = std::max(1.0f, cfg.max_lock_distance_pixels);

    for (auto& det : detections) {
        if (!Matches(cfg, det.class_id)) continue;

        const cv::Point2f center = BoxCenter(det.box);
        const float crosshair_distance = Distance(center, crosshair);
        if (crosshair_distance <= fov) {
            ++crowd_count;
            float score = crosshair_distance - det.confidence * 12.0f;
            if (score < best_score) {
                best_score = score;
                best = &det;
            }
        }

        if (has_last_target) {
            const float center_distance = Distance(center, last_center);
            const float iou = IntersectionOverUnion(det.box, last_target_box);
            const float target_scale = static_cast<float>(
                std::max(last_target_box.width, last_target_box.height));
            const float track_radius = std::max(45.0f, target_scale * 0.80f);
            const bool same_track = iou >= 0.05f || center_distance <= track_radius;
            if (same_track && crosshair_distance <= fov * 1.30f) {
                const float track_score = center_distance - iou * 80.0f;
                if (track_score < best_track_score) {
                    best_track_score = track_score;
                    tracked_score = crosshair_distance - det.confidence * 12.0f;
                    tracked_distance = crosshair_distance;
                    tracked = &det;
                }
            }
        }
    }

    Detection* selected = best;
    if (tracked) {
        if (!best) {
            selected = tracked;
        } else {
            const float lock_bonus = std::min(35.0f, static_cast<float>(lock_frames) * 2.5f);
            const float crowd_bonus = crowd_count > 1 ? std::max(20.0f, fov * 0.10f) : 0.0f;
            const float switch_margin = std::max(cfg.target_switch_margin, fov * 0.22f) + lock_bonus + crowd_bonus;
            const bool tracked_outside_lock = tracked_distance > fov * 1.18f;
            const bool challenger_is_much_better = best_score + switch_margin < tracked_score;
            if (tracked_outside_lock) {
                selected = best;
                challenger_frames = 0;
            } else if (challenger_is_much_better && best != tracked) {
                if (IsSameCandidate(best->box, challenger_box)) {
                    challenger_frames = std::min(challenger_frames + 1, 120);
                } else {
                    challenger_box = best->box;
                    challenger_frames = 1;
                }

                const int confirm_frames = std::max(1, cfg.target_switch_confirm_frames);
                selected = challenger_frames >= confirm_frames ? best : tracked;
            } else {
                selected = tracked;
                challenger_frames = 0;
            }
        }
    }

    if (selected) {
        if (selected == tracked) {
            lock_frames = std::min(lock_frames + 1, 120);
        } else {
            target_switched = has_last_target;
            if (target_switched) {
                ++lock_generation;
            }
            lock_frames = 1;
            challenger_frames = 0;
        }
        lost_frames = 0;
        last_target_box = selected->box;
        has_last_target = true;
    } else {
        if (++lost_frames > 3) {
            Reset();
        }
    }

    return selected;
}

void TargetSelector::Reset() {
    last_target_box = cv::Rect(0, 0, 0, 0);
    challenger_box = cv::Rect(0, 0, 0, 0);
    has_last_target = false;
    target_switched = false;
    lock_frames = 0;
    lost_frames = 0;
    crowd_count = 0;
    challenger_frames = 0;
    lock_generation = 0;
}
