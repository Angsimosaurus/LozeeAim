#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Common.hpp"
#include "Config.hpp"
#include <vector>

class TargetSelector {
public:
    Detection* Select(std::vector<Detection>& detections, const Config& cfg, const cv::Point& crop_center);
    void Reset();

    bool WasTargetSwitched() const { return target_switched; }
    int GetLockFrames() const { return lock_frames; }
    int GetCrowdCount() const { return crowd_count; }
    int GetLockGeneration() const { return lock_generation; }

    static bool Matches(const Config& cfg, int class_id);

private:
    cv::Rect last_target_box{0, 0, 0, 0};
    cv::Rect challenger_box{0, 0, 0, 0};
    bool has_last_target = false;
    bool target_switched = false;
    int lock_frames = 0;
    int lost_frames = 0;
    int crowd_count = 0;
    int challenger_frames = 0;
    int lock_generation = 0;
};
