#include "ObjectDetector.hpp"
#include "ModelPaths.hpp"
#include "RuntimeDependencyLoader.hpp"
#include <chrono>
#include <iostream>
#include <algorithm>
#include <cwctype>
#include <stdexcept>

namespace {
std::wstring Utf8ToWide(const char* value) {
    if (!value || value[0] == '\0') return {};

    const int size = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
    if (size <= 1) return {};

    std::vector<wchar_t> buffer(static_cast<size_t>(size));
    MultiByteToWideChar(CP_UTF8, 0, value, -1, buffer.data(), size);
    return std::wstring(buffer.data());
}

std::wstring ToLowerWide(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}

bool IsYoloModelName(const std::wstring& filename) {
    const std::wstring lower = ToLowerWide(filename);
    if (lower.size() < 6 || lower.substr(lower.size() - 5) != L".onnx") {
        return false;
    }
    return lower.find(L"nn_model") == std::wstring::npos;
}

std::wstring FindFirstLocalYoloModel() {
    model_paths::EnsureModelDirs();
    const std::wstring yolo_dir = model_paths::YoloDirW();
    std::vector<std::wstring> models;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((yolo_dir + L"*.onnx").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return {};
    }

    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const std::wstring filename = fd.cFileName;
        if (IsYoloModelName(filename)) {
            models.push_back(filename);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (models.empty()) {
        return {};
    }

    std::sort(models.begin(), models.end(), [](const std::wstring& a, const std::wstring& b) {
        return ToLowerWide(a) < ToLowerWide(b);
    });
    return yolo_dir + models.front();
}

std::wstring ResolveModelPathImpl(const Config& cfg) {
    std::wstring selected = Utf8ToWide(cfg.yolo_model_paths);
    std::wstring resolved = model_paths::ResolveExistingPathW(selected);
    if (!resolved.empty()) {
        return resolved;
    }

    resolved = FindFirstLocalYoloModel();
    if (!resolved.empty()) {
        return resolved;
    }

    return cfg.model_path;
}
}

std::wstring ObjectDetector::ResolveModelPath(const Config& cfg) {
    return ResolveModelPathImpl(cfg);
}

ObjectDetector::ObjectDetector(const Config& cfg)
    : env(ORT_LOGGING_LEVEL_WARNING, "Realtime_YOLO_Detector"),
      session(nullptr),
      provider(cfg.use_cpu_inference ? 2 : cfg.inference_provider) {

    std::cout << "--- Loading AI detection model (provider=" << provider << ") ---" << std::endl;
    Ort::SessionOptions session_options;
    session_options.SetIntraOpNumThreads(1);
    session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    if (provider == 1) {
        std::string preload_error;
        if (!runtime_deps::PreloadTensorRtProviderDependencies(&preload_error)) {
            throw std::runtime_error("TensorRT provider preload failed: " + preload_error);
        }
        OrtTensorRTProviderOptions trt{};
        trt.device_id = 0;
        trt.trt_fp16_enable = 1;
        trt.trt_engine_cache_enable = 1;
        if (cfg.trt_cache_path) trt.trt_engine_cache_path = cfg.trt_cache_path;
        session_options.AppendExecutionProvider_TensorRT(trt);
        std::cout << "--- using TensorRT ---" << std::endl;
    } else if (provider == 2) {
        std::cout << "--- using CPU ---" << std::endl;
    } else {
        session_options.AppendExecutionProvider("DML");
        std::cout << "--- using DirectML ---" << std::endl;
    }

    const std::wstring model_path = ResolveModelPath(cfg);
    session = Ort::Session(env, model_path.c_str(), session_options);

    auto output_shape = session.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    is_end_to_end_model = (output_shape.size() == 3 && output_shape.back() == 6);
    std::cout << "[Auto-detect] Output shape: [";
    for (size_t i = 0; i < output_shape.size(); ++i) {
        std::cout << output_shape[i] << (i + 1 < output_shape.size() ? "," : "");
    }
    std::cout << "] => " << (is_end_to_end_model ? "end-to-end (NMS-free)" : "standard (NMS required)") << std::endl;

    pAllocator = std::make_unique<Ort::AllocatorWithDefaultOptions>();
    pInputName = std::make_unique<Ort::AllocatedStringPtr>(session.GetInputNameAllocated(0, *pAllocator));
    pOutputName = std::make_unique<Ort::AllocatedStringPtr>(session.GetOutputNameAllocated(0, *pAllocator));
    pMemoryInfo = std::make_unique<Ort::MemoryInfo>("Cpu", OrtArenaAllocator, 0, OrtMemTypeDefault);

    auto input_shape = session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    int64_t h = (input_shape.size() > 2 && input_shape[2] > 0) ? input_shape[2] : 640;
    int64_t w = (input_shape.size() > 3 && input_shape[3] > 0) ? input_shape[3] : 640;
    cached_input_shape = { 1, 3, h, w };

    canvas = cv::Mat::ones(cv::Size(static_cast<int>(w), static_cast<int>(h)), CV_8UC3) * 114;

    size_t blob_size = cached_input_shape[0] * cached_input_shape[1] * cached_input_shape[2] * cached_input_shape[3];
    preallocated_blob = cv::Mat(1, static_cast<int>(blob_size), CV_32F);

    std::cout << "--- Model loaded! ---" << std::endl;
}

bool ObjectDetector::Reload(const wchar_t* path) {
    try {
        Ort::SessionOptions session_options;
        session_options.SetIntraOpNumThreads(1);
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        if (provider == 1) {
            std::string preload_error;
            if (!runtime_deps::PreloadTensorRtProviderDependencies(&preload_error)) {
                throw std::runtime_error("TensorRT provider preload failed: " + preload_error);
            }
            OrtTensorRTProviderOptions trt{};
            trt.device_id = 0;
            trt.trt_fp16_enable = 1;
            trt.trt_engine_cache_enable = 1;
            trt.trt_engine_cache_path = ".\\engine_cache";
            session_options.AppendExecutionProvider_TensorRT(trt);
        } else if (provider != 2) {
            session_options.AppendExecutionProvider("DML");
        }

        session = Ort::Session(env, path, session_options);

        auto output_shape = session.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        is_end_to_end_model = (output_shape.size() == 3 && output_shape.back() == 6);
        std::cout << "[Auto-detect] Output shape: [";
        for (size_t i = 0; i < output_shape.size(); ++i) {
            std::cout << output_shape[i] << (i + 1 < output_shape.size() ? "," : "");
        }
        std::cout << "] => " << (is_end_to_end_model ? "end-to-end (NMS-free)" : "standard (NMS required)") << std::endl;

        pInputName = std::make_unique<Ort::AllocatedStringPtr>(session.GetInputNameAllocated(0, *pAllocator));
        pOutputName = std::make_unique<Ort::AllocatedStringPtr>(session.GetOutputNameAllocated(0, *pAllocator));

        auto input_shape = session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        int64_t h = (input_shape.size() > 2 && input_shape[2] > 0) ? input_shape[2] : 640;
        int64_t w = (input_shape.size() > 3 && input_shape[3] > 0) ? input_shape[3] : 640;
        cached_input_shape = { 1, 3, h, w };

        canvas = cv::Mat::ones(cv::Size(static_cast<int>(w), static_cast<int>(h)), CV_8UC3) * 114;

        size_t blob_size = cached_input_shape[0] * cached_input_shape[1] * cached_input_shape[2] * cached_input_shape[3];
        preallocated_blob = cv::Mat(1, static_cast<int>(blob_size), CV_32F);

        char path_narrow[256];
        WideCharToMultiByte(CP_UTF8, 0, path, -1, path_narrow, sizeof(path_narrow), nullptr, nullptr);
        std::cout << "[OK] YOLO reloaded: " << path_narrow << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ERR] YOLO reload failed: " << e.what() << std::endl;
        return false;
    }
}

void ObjectDetector::Detect(const cv::Mat& image, std::vector<Detection>& detections, const Config& cfg, TimingDetails& timings) {
    detections.clear();
    if (image.empty()) return;
    auto stage_start = std::chrono::high_resolution_clock::now();

    int input_height = static_cast<int>(cached_input_shape[2]);
    int input_width = static_cast<int>(cached_input_shape[3]);

    float ratio = 1.0f;
    int paste_x = 0, paste_y = 0;

    if (image.cols == input_width && image.rows == input_height) {
        cv::dnn::blobFromImage(image, preallocated_blob, 1.0 / 255.0, cv::Size(input_width, input_height), cv::Scalar(), true, false, CV_32F);
    } else {
        float ratio_h = static_cast<float>(input_height) / image.rows;
        float ratio_w = static_cast<float>(input_width) / image.cols;
        ratio = std::min<float>(ratio_h, ratio_w);
        int new_w = static_cast<int>(image.cols * ratio);
        int new_h = static_cast<int>(image.rows * ratio);

        cv::Mat resized_img;
        cv::resize(image, resized_img, cv::Size(new_w, new_h));

        canvas.setTo(cv::Scalar(114, 114, 114));
        paste_x = (static_cast<int>(input_width) - new_w) / 2;
        paste_y = (static_cast<int>(input_height) - new_h) / 2;
        resized_img.copyTo(canvas(cv::Rect(paste_x, paste_y, new_w, new_h)));

        cv::dnn::blobFromImage(canvas, preallocated_blob, 1.0 / 255.0, cv::Size(input_width, input_height), cv::Scalar(), true, false, CV_32F);
    }

    auto stage_end_preprocess = std::chrono::high_resolution_clock::now();
    timings.preprocess_ms = std::chrono::duration_cast<std::chrono::microseconds>(stage_end_preprocess - stage_start).count() / 1000.0;

    const char* input_names[] = { pInputName->get() };
    const char* output_names[] = { pOutputName->get() };

    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        *pMemoryInfo, preallocated_blob.ptr<float>(), preallocated_blob.total(),
        cached_input_shape.data(), cached_input_shape.size()
    );

    auto output_tensors = session.Run(Ort::RunOptions{ nullptr }, input_names, &input_tensor, 1, output_names, 1);
    auto stage_end_inference = std::chrono::high_resolution_clock::now();
    timings.inference_ms = std::chrono::duration_cast<std::chrono::microseconds>(stage_end_inference - stage_end_preprocess).count() / 1000.0;

    const float* output_data = output_tensors.front().GetTensorData<float>();
    const auto& output_shape = output_tensors.front().GetTensorTypeAndShapeInfo().GetShape();

    if (is_end_to_end_model) {
        const int num_detections = static_cast<int>(output_shape[1]);
        for (int i = 0; i < num_detections; ++i) {
            const float confidence = output_data[i * 6 + 4];
            if (confidence >= cfg.confidence_threshold) {
                const float x1 = output_data[i * 6 + 0];
                const float y1 = output_data[i * 6 + 1];
                const float x2 = output_data[i * 6 + 2];
                const float y2 = output_data[i * 6 + 3];
                const int class_id = static_cast<int>(output_data[i * 6 + 5]);
                int left = static_cast<int>(std::round((x1 - paste_x) / ratio));
                int top = static_cast<int>(std::round((y1 - paste_y) / ratio));
                int width = static_cast<int>(std::round((x2 - x1) / ratio));
                int height = static_cast<int>(std::round((y2 - y1) / ratio));
                detections.emplace_back(Detection{ cv::Rect(left, top, width, height), confidence, class_id });
            }
        }
    }
    else {
        cv::Mat output_mat(
            static_cast<int>(output_shape[1]),
            static_cast<int>(output_shape[2]),
            CV_32F,
            (void*)output_data);
        output_mat = output_mat.t();

        std::vector<cv::Rect> boxes;
        std::vector<float> confidences;
        std::vector<int> class_ids;

        for (int i = 0; i < output_mat.rows; ++i) {
            // Manual max-score scan (avoid cv::minMaxLoc overhead)
            const float* row = output_mat.ptr<float>(i);
            float cx = row[0], cy = row[1], w = row[2], h = row[3];
            float max_score = 0.0f;
            int   max_cls   = 0;
            for (int c = 0; c < output_mat.cols - 4; ++c) {
                if (row[4 + c] > max_score) { max_score = row[4 + c]; max_cls = c; }
            }
            if (max_score > cfg.confidence_threshold) {
                int left   = static_cast<int>(std::round((cx - 0.5f * w - paste_x) / ratio));
                int top    = static_cast<int>(std::round((cy - 0.5f * h - paste_y) / ratio));
                int width  = static_cast<int>(std::round(w / ratio));
                int height = static_cast<int>(std::round(h / ratio));
                boxes.emplace_back(left, top, width, height);
                confidences.push_back(max_score);
                class_ids.push_back(max_cls);
            }
        }
        std::vector<int> nms_indices;
        cv::dnn::NMSBoxes(boxes, confidences, cfg.confidence_threshold, cfg.nms_threshold, nms_indices);
        for (int idx : nms_indices) {
            detections.emplace_back(Detection{ boxes[idx], confidences[idx], class_ids[idx] });
        }
    }
    auto stage_end_postprocess = std::chrono::high_resolution_clock::now();
    timings.postprocess_ms = std::chrono::duration_cast<std::chrono::microseconds>(stage_end_postprocess - stage_end_inference).count() / 1000.0;
    timings.total_loop_ms = std::chrono::duration_cast<std::chrono::microseconds>(stage_end_postprocess - stage_start).count() / 1000.0;
}
