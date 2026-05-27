#pragma once
#include <vector>
#include <opencv2/opencv.hpp>
#include <random>

class TrajectoryGenerator {
public:
    static std::vector<cv::Point2f> GenerateCubicBezier(cv::Point2f start, cv::Point2f end, int steps) {
        std::vector<cv::Point2f> path;

        static thread_local std::mt19937 gen(std::random_device{}());
        std::uniform_real_distribution<float> t_dist(0.30f, 0.70f);
        std::uniform_real_distribution<float> offset_dist(-15.0f, 15.0f);
        std::uniform_real_distribution<float> small_jitter(-2.0f, 2.0f);

        cv::Point2f dir = end - start;
        float dist = (float)cv::norm(dir);
        cv::Point2f perp(-dir.y, dir.x);
        if (cv::norm(perp) > 0.001f)
            perp = perp / cv::norm(perp) * dist * 0.15f;

        float t1 = t_dist(gen);
        float t2 = t_dist(gen);

        cv::Point2f p1 = start + dir * t1 + perp * (offset_dist(gen) / 30.0f);
        cv::Point2f p2 = start + dir * t2 - perp * (offset_dist(gen) / 30.0f);

        for (int i = 0; i <= steps; ++i) {
            float t = static_cast<float>(i) / steps;
            float u = 1.0f - t;
            float uu = u * u;
            float tt = t * t;

            cv::Point2f p = (uu * u * start) + (3.0f * uu * t * p1)
                          + (3.0f * u * tt * p2) + (tt * t * end);
            p.x += small_jitter(gen);
            p.y += small_jitter(gen);
            path.push_back(p);
        }
        return path;
    }
};
