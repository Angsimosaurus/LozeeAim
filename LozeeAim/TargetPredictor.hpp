#pragma once

#include "Common.hpp"
#include "Config.hpp"

class TargetPredictor {
public:
    cv::Point Update(const Detection& target, const cv::Point& raw_point, const Config& cfg, double dt, float lead_scale = 1.0f);
    void Reset();

private:
    bool initialized = false;
    cv::Point2f last_point{};
    cv::Point2f velocity{};
    cv::Rect last_box{};
};
