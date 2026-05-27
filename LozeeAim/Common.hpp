#pragma once

#include <opencv2/opencv.hpp>
#include <iostream>

template<class T>
void SafeRelease(T** ppT) {
    if (*ppT) {
        (*ppT)->Release();
        *ppT = nullptr;
    }
}

struct Detection {
    cv::Rect box;
    float confidence;
    int class_id;
};

struct TimingDetails {
    double capture_ms = 0;
    double preprocess_ms = 0;
    double inference_ms = 0;
    double postprocess_ms = 0;
    double total_loop_ms = 0;
    double mouse_ms = 0;
    double render_ms = 0;
    double full_loop_ms = 0;
    double real_fps = 0;
};
