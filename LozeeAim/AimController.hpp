#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Config.hpp"
#include <opencv2/opencv.hpp>
#include <optional>
#include <random>

class AimController {
public:
    enum Algorithm {
        AlgorithmAdaptive = 0,
        AlgorithmLinear = 1,
        AlgorithmSpring = 2,
        AlgorithmNeuralNetwork = 3,
        AlgorithmCount = 4
    };

    struct Input {
        cv::Point target_point;
        cv::Point center;
        int target_width = 0;
        double dt = 0.0;
        double mouse_correction = 1.0;
    };

    struct Output {
        int dx = 0;
        int dy = 0;
    };

    std::optional<Output> Update(const Config& cfg, const Input& input);
    void Reset();

private:
    void ResetMotionState();

    int last_algorithm = -1;
    bool initialized = false;
    float smoothed_x = 0.0f;
    float smoothed_y = 0.0f;
    float step_x = 0.0f;
    float step_y = 0.0f;
    float spring_vx = 0.0f;
    float spring_vy = 0.0f;
    float fractional_x = 0.0f;
    float fractional_y = 0.0f;
    float humanize_x = 0.0f;
    float humanize_y = 0.0f;
    float humanize_target_x = 0.0f;
    float humanize_target_y = 0.0f;
    float humanize_timer = 0.0f;
    std::mt19937 rng{std::random_device{}()};
};
