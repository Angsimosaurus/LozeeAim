#pragma once
#include <onnxruntime_cxx_api.h>
#include <vector>
#include <memory>
#include <string>
#include "Common.hpp"
#include "Config.hpp"

class ObjectDetector {
public:
    explicit ObjectDetector(const Config& cfg);

    ObjectDetector(const ObjectDetector&) = delete;
    ObjectDetector& operator=(const ObjectDetector&) = delete;

    static std::wstring ResolveModelPath(const Config& cfg);

    void Detect(const cv::Mat& image, std::vector<Detection>& detections, const Config& cfg, TimingDetails& timings);
    bool Reload(const wchar_t* path);
    bool IsEndToEndModel() const { return is_end_to_end_model; }

private:
    Ort::Env env;
    Ort::Session session;
    int provider = 0;  // 0=DirectML, 1=TensorRT, 2=CPU
    std::unique_ptr<Ort::AllocatorWithDefaultOptions> pAllocator;
    std::unique_ptr<Ort::AllocatedStringPtr> pInputName;
    std::unique_ptr<Ort::AllocatedStringPtr> pOutputName;
    std::unique_ptr<Ort::MemoryInfo> pMemoryInfo;
    std::vector<int64_t> cached_input_shape;
    cv::Mat canvas;
    cv::Mat preallocated_blob;
    bool is_end_to_end_model = false;
};
