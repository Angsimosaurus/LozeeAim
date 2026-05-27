#pragma once
#include <vector>
#include <cmath>
#include <fstream>
#include <random>
#include <algorithm>
#include <cstring>
#include <string>
#include <functional>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <opencv2/dnn.hpp>
#include "ONNXExporter.hpp"

class MouseTrajectoryNN {
public:
    static constexpr int INPUT_SIZE = 2;
    static constexpr int HIDDEN1_SIZE = 64;
    static constexpr int HIDDEN2_SIZE = 128;
    static constexpr int WAYPOINT_COUNT = 10;
    static constexpr int OUTPUT_SIZE = WAYPOINT_COUNT * 2;
    static constexpr int NETWORK_WAYPOINT_COUNT = WAYPOINT_COUNT - 2;
    static constexpr int NETWORK_OUTPUT_SIZE = NETWORK_WAYPOINT_COUNT * 2;
    static constexpr float DEFAULT_NORM_SCALE = 1000.0f;

    struct Sample {
        float dx, dy;
        float waypoints[OUTPUT_SIZE];
    };

    struct Point2D {
        float x, y;
    };

    struct TrainConfig {
        int epochs = 200;
        float lr = 0.001f;
        float momentum = 0.9f;
        int batch_size = 32;
        float lambda_smooth = 0.1f;
        float weight_decay = 1e-5f;
        float grad_clip = 1.0f;
        float norm_scale = DEFAULT_NORM_SCALE;
        int aug_count = 0;  // synthetic samples to append (0=off)
        std::atomic<float>* progress = nullptr;  // 0.0 ~ 1.0
        std::function<void(const std::string&)> status_callback;
    };

    MouseTrajectoryNN() { InitWeights(); }

    void InitWeights() {
        float s1 = sqrtf(6.0f / (INPUT_SIZE + HIDDEN1_SIZE));
        float s2 = sqrtf(6.0f / (HIDDEN1_SIZE + HIDDEN2_SIZE));
        float s3 = sqrtf(6.0f / (HIDDEN2_SIZE + NETWORK_OUTPUT_SIZE));

        std::mt19937 rng(42);
        std::uniform_real_distribution<float> d1(-s1, s1);
        std::uniform_real_distribution<float> d2(-s2, s2);
        std::uniform_real_distribution<float> d3(-s3, s3);

        for (int i = 0; i < HIDDEN1_SIZE; i++) {
            for (int j = 0; j < INPUT_SIZE; j++) W1[i][j] = d1(rng);
            b1[i] = 0.0f; mb1[i] = 0.0f;
            for (int j = 0; j < INPUT_SIZE; j++) mW1[i][j] = 0.0f;
        }
        for (int i = 0; i < HIDDEN2_SIZE; i++) {
            for (int j = 0; j < HIDDEN1_SIZE; j++) W2[i][j] = d2(rng);
            b2[i] = 0.0f; mb2[i] = 0.0f;
            for (int j = 0; j < HIDDEN1_SIZE; j++) mW2[i][j] = 0.0f;
        }
        for (int i = 0; i < NETWORK_OUTPUT_SIZE; i++) {
            for (int j = 0; j < HIDDEN2_SIZE; j++) W3[i][j] = d3(rng);
            b3[i] = 0.0f; mb3[i] = 0.0f;
            for (int j = 0; j < HIDDEN2_SIZE; j++) mW3[i][j] = 0.0f;
        }
        use_onnx = false;
        onnx_net = cv::dnn::Net();
        model_loaded = false;
        norm_scale = DEFAULT_NORM_SCALE;
    }

    // -----------------------------------------------------------------------
    //  Inference
    // -----------------------------------------------------------------------
    static void BuildStructuredWaypoints(float dx, float dy, const float* middle, float* out_waypoints) {
        out_waypoints[0] = 0.0f;
        out_waypoints[1] = 0.0f;
        for (int i = 0; i < NETWORK_WAYPOINT_COUNT; i++) {
            out_waypoints[(i + 1) * 2] = middle[i * 2];
            out_waypoints[(i + 1) * 2 + 1] = middle[i * 2 + 1];
        }
        out_waypoints[(WAYPOINT_COUNT - 1) * 2] = dx;
        out_waypoints[(WAYPOINT_COUNT - 1) * 2 + 1] = dy;
    }

    void Predict(float dx, float dy, float* out_waypoints) const {
        if (!std::isfinite(dx) || !std::isfinite(dy)) {
            for (int i = 0; i < OUTPUT_SIZE; i++) out_waypoints[i] = 0.0f;
            return;
        }

        if (use_onnx && !onnx_net.empty()) {
            cv::Mat blob(1, 2, CV_32F);
            blob.at<float>(0, 0) = dx / norm_scale;
            blob.at<float>(0, 1) = dy / norm_scale;
            onnx_net.setInput(blob, "input");
            cv::Mat output = onnx_net.forward("output");
            const float* data = output.ptr<float>(0);
            const int output_values = static_cast<int>(output.total());
            if (output_values >= OUTPUT_SIZE) {
                for (int i = 0; i < WAYPOINT_COUNT; i++) {
                    float vx = data[i * 2]     * norm_scale;
                    float vy = data[i * 2 + 1] * norm_scale;
                    out_waypoints[i * 2]     = std::isfinite(vx) ? vx : 0.0f;
                    out_waypoints[i * 2 + 1] = std::isfinite(vy) ? vy : 0.0f;
                }
                out_waypoints[0] = 0.0f;
                out_waypoints[1] = 0.0f;
                out_waypoints[(WAYPOINT_COUNT - 1) * 2] = dx;
                out_waypoints[(WAYPOINT_COUNT - 1) * 2 + 1] = dy;
            } else {
                float middle[NETWORK_OUTPUT_SIZE] = {};
                for (int i = 0; i < NETWORK_OUTPUT_SIZE && i < output_values; i++) {
                    float value = data[i] * norm_scale;
                    middle[i] = std::isfinite(value) ? value : 0.0f;
                }
                BuildStructuredWaypoints(dx, dy, middle, out_waypoints);
            }
            return;
        }

        float h1[HIDDEN1_SIZE], h2[HIDDEN2_SIZE];
        float nx = dx / norm_scale;
        float ny = dy / norm_scale;

        for (int i = 0; i < HIDDEN1_SIZE; i++) {
            float z = b1[i];
            z += W1[i][0] * nx;
            z += W1[i][1] * ny;
            h1[i] = tanhf(z);
        }
        for (int i = 0; i < HIDDEN2_SIZE; i++) {
            float z = b2[i];
            for (int j = 0; j < HIDDEN1_SIZE; j++) z += W2[i][j] * h1[j];
            h2[i] = tanhf(z);
        }
        float middle[NETWORK_OUTPUT_SIZE];
        for (int i = 0; i < NETWORK_OUTPUT_SIZE; i++) {
            float z = b3[i];
            for (int j = 0; j < HIDDEN2_SIZE; j++) z += W3[i][j] * h2[j];
            middle[i] = std::isfinite(z) ? z * norm_scale : 0.0f;
        }
        BuildStructuredWaypoints(dx, dy, middle, out_waypoints);
    }

    // -----------------------------------------------------------------------
    //  Trajectory smoothing  (from Mouse-Trajectory-Generator)
    // -----------------------------------------------------------------------
    static void SmoothTrajectory(float* waypoints, int wp_count,
                                  int passes = 3, float alpha = 0.4f) {
        if (wp_count < 3) return;
        for (int pass = 0; pass < passes; pass++) {
            for (int i = 1; i < wp_count - 1; i++) {
                float px = waypoints[(i - 1) * 2], py = waypoints[(i - 1) * 2 + 1];
                float nx = waypoints[(i + 1) * 2], ny = waypoints[(i + 1) * 2 + 1];
                waypoints[i * 2]     = waypoints[i * 2]     * (1.0f - alpha) + (px + nx) * alpha * 0.5f;
                waypoints[i * 2 + 1] = waypoints[i * 2 + 1] * (1.0f - alpha) + (py + ny) * alpha * 0.5f;
            }
        }
    }

    // -----------------------------------------------------------------------
    //  Synthetic Bezier trajectory augmentation (inspired by OxyMouse & bezmouse)
    //  Generates additional training samples with cubic Bezier curves to
    //  fill gaps in the input space and prevent overfitting.
    // -----------------------------------------------------------------------
    static std::vector<Sample> GenerateBezierSamples(int count, float max_range = 800.0f) {
        std::vector<Sample> out;
        out.reserve(count);

        std::mt19937 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
        std::uniform_real_distribution<float> pos_dist(-max_range, max_range);
        std::uniform_real_distribution<float> t_dist(0.25f, 0.75f);
        std::uniform_real_distribution<float> perp_dist(-0.3f, 0.3f);
        std::uniform_real_distribution<float> jitter(-3.0f, 3.0f);

        for (int k = 0; k < count; k++) {
            float sx = pos_dist(rng) * 0.3f;
            float sy = pos_dist(rng) * 0.3f;
            float ex = pos_dist(rng);
            float ey = pos_dist(rng);

            float dx = ex - sx;
            float dy = ey - sy;
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist < 5.0f) continue;

            // Generate cubic Bezier control points with random perpendicular offsets
            float perp_x = -dy / dist;
            float perp_y =  dx / dist;
            float offset1 = perp_dist(rng) * dist;
            float offset2 = perp_dist(rng) * dist;
            float t1 = t_dist(rng);
            float t2 = t_dist(rng);

            float cp1x = sx + dx * t1 + perp_x * offset1;
            float cp1y = sy + dy * t1 + perp_y * offset1;
            float cp2x = sx + dx * t2 + perp_x * offset2;
            float cp2y = sy + dy * t2 + perp_y * offset2;

            Sample s;
            s.dx = dx;
            s.dy = dy;

            for (int i = 0; i < WAYPOINT_COUNT; i++) {
                float t = (float)i / (WAYPOINT_COUNT - 1);
                float u = 1.0f - t;
                float uu = u * u;
                float tt = t * t;

                float wx = uu * u * sx + 3.0f * uu * t * cp1x
                         + 3.0f * u * tt * cp2x + tt * t * ex;
                float wy = uu * u * sy + 3.0f * uu * t * cp1y
                         + 3.0f * u * tt * cp2y + tt * t * ey;

                wx += jitter(rng);
                wy += jitter(rng);

                s.waypoints[i * 2]     = wx - sx;
                s.waypoints[i * 2 + 1] = wy - sy;
            }
            out.push_back(s);
        }
        return out;
    }

    static Sample NormalizeSample(const Sample& input) {
        Sample s = input;
        s.waypoints[0] = 0.0f;
        s.waypoints[1] = 0.0f;
        s.waypoints[(WAYPOINT_COUNT - 1) * 2] = s.dx;
        s.waypoints[(WAYPOINT_COUNT - 1) * 2 + 1] = s.dy;
        return s;
    }

    static bool ValidateSample(const Sample& input, float norm_scale, std::string* out_reason = nullptr) {
        const Sample s = NormalizeSample(input);
        if (!std::isfinite(s.dx) || !std::isfinite(s.dy)) {
            if (out_reason) *out_reason = "non-finite endpoint";
            return false;
        }

        const float distance = std::hypot(s.dx, s.dy);
        if (distance < 5.0f) {
            if (out_reason) *out_reason = "too short";
            return false;
        }
        if (distance > std::max(100.0f, norm_scale * 1.25f)) {
            if (out_reason) *out_reason = "outside norm range";
            return false;
        }

        float path_length = 0.0f;
        float max_step = 0.0f;
        int backwards_steps = 0;
        const float ux = s.dx / distance;
        const float uy = s.dy / distance;

        for (int i = 0; i < OUTPUT_SIZE; i++) {
            if (!std::isfinite(s.waypoints[i])) {
                if (out_reason) *out_reason = "non-finite waypoint";
                return false;
            }
        }

        for (int wp = 1; wp < WAYPOINT_COUNT; wp++) {
            const float x0 = s.waypoints[(wp - 1) * 2];
            const float y0 = s.waypoints[(wp - 1) * 2 + 1];
            const float x1 = s.waypoints[wp * 2];
            const float y1 = s.waypoints[wp * 2 + 1];
            const float step_x = x1 - x0;
            const float step_y = y1 - y0;
            const float step = std::hypot(step_x, step_y);
            path_length += step;
            max_step = std::max(max_step, step);
            if (step_x * ux + step_y * uy < -distance * 0.08f) {
                ++backwards_steps;
            }
        }

        if (path_length > distance * 3.5f) {
            if (out_reason) *out_reason = "too curved";
            return false;
        }
        if (max_step > std::max(160.0f, distance * 0.75f)) {
            if (out_reason) *out_reason = "step spike";
            return false;
        }
        if (backwards_steps > 2) {
            if (out_reason) *out_reason = "too many reversals";
            return false;
        }
        return true;
    }

    // -----------------------------------------------------------------------
    //  Training with multiple improvements over the original:
    //   - Random shuffle train/val split  (vs chronological)
    //   - Weight decay (L2 regularization)
    //   - Gradient clipping
    //   - Cosine learning rate decay
    //   - Progress callback
    //   - Configurable normalization scale
    // -----------------------------------------------------------------------
    bool Train(const std::vector<Sample>& samples, const TrainConfig& cfg,
               float* out_final_loss, std::string* out_status,
               float* out_val_loss = nullptr) {
        if (samples.empty()) {
            if (out_status) *out_status = "无训练数据";
            return false;
        }

        norm_scale = cfg.norm_scale;

        const size_t N = samples.size();

        // Shuffle indices before split (was: chronological split)
        std::vector<size_t> all_indices(N);
        for (size_t i = 0; i < N; i++) all_indices[i] = i;

        std::mt19937 shuffle_rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
        std::shuffle(all_indices.begin(), all_indices.end(), shuffle_rng);

        // Train / val split (80/20) on shuffled data
        size_t val_count = std::max(size_t(1), N / 5);
        size_t train_count = N - val_count;

        std::vector<size_t> train_indices(all_indices.begin(), all_indices.begin() + train_count);
        std::vector<size_t> val_indices(all_indices.begin() + train_count, all_indices.end());

        // ---- Gradient clipping helper ----
        auto clip_grad = [](float& g, float clip) {
            if (g >  clip) g =  clip;
            if (g < -clip) g = -clip;
        };

        float best_val_loss = 1e9f;
        int patience = 30;
        int no_improve = 0;

        // Cosine LR schedule
        float lr_initial = cfg.lr;
        float lr_final = cfg.lr * 0.01f;

        for (int ep = 0; ep < cfg.epochs; ep++) {
            // Cosine annealing LR
            float progress_ratio = (float)ep / (float)(cfg.epochs - 1);
            float lr = lr_final + 0.5f * (lr_initial - lr_final) * (1.0f + cosf(3.1415926535f * progress_ratio));

            // Shuffle training indices each epoch
            std::shuffle(train_indices.begin(), train_indices.end(), shuffle_rng);
            float epoch_loss = 0.0f;
            int batch_count = 0;

            for (size_t bi = 0; bi < train_count; bi += cfg.batch_size) {
                size_t bs = std::min((size_t)cfg.batch_size, train_count - bi);

                float dW1[HIDDEN1_SIZE][INPUT_SIZE] = {};
                float db1[HIDDEN1_SIZE] = {};
                float dW2[HIDDEN2_SIZE][HIDDEN1_SIZE] = {};
                float db2[HIDDEN2_SIZE] = {};
                float dW3[NETWORK_OUTPUT_SIZE][HIDDEN2_SIZE] = {};
                float db3[NETWORK_OUTPUT_SIZE] = {};

                for (size_t k = 0; k < bs; k++) {
                    const auto& s = samples[train_indices[bi + k]];
                    float nx = s.dx / norm_scale;
                    float ny = s.dy / norm_scale;

                    // ---- Forward pass ----
                    float z1[HIDDEN1_SIZE], a1[HIDDEN1_SIZE];
                    float z2[HIDDEN2_SIZE], a2[HIDDEN2_SIZE];
                    float y[NETWORK_OUTPUT_SIZE];

                    for (int i = 0; i < HIDDEN1_SIZE; i++) {
                        z1[i] = b1[i] + W1[i][0] * nx + W1[i][1] * ny;
                        a1[i] = tanhf(z1[i]);
                    }
                    for (int i = 0; i < HIDDEN2_SIZE; i++) {
                        z2[i] = b2[i];
                        for (int j = 0; j < HIDDEN1_SIZE; j++) z2[i] += W2[i][j] * a1[j];
                        a2[i] = tanhf(z2[i]);
                    }
                    for (int i = 0; i < NETWORK_OUTPUT_SIZE; i++) {
                        y[i] = b3[i];
                        for (int j = 0; j < HIDDEN2_SIZE; j++) y[i] += W3[i][j] * a2[j];
                    }

                    float target_norm[NETWORK_OUTPUT_SIZE];
                    for (int wp = 1; wp < WAYPOINT_COUNT - 1; wp++) {
                        const int out_idx = (wp - 1) * 2;
                        target_norm[out_idx] = s.waypoints[wp * 2] / norm_scale;
                        target_norm[out_idx + 1] = s.waypoints[wp * 2 + 1] / norm_scale;
                    }

                    // ---- MSE gradient ----
                    float dy[NETWORK_OUTPUT_SIZE];
                    float mse_loss = 0.0f;
                    for (int i = 0; i < NETWORK_OUTPUT_SIZE; i++) {
                        float err = y[i] - target_norm[i];
                        dy[i] = 2.0f * err;
                        mse_loss += err * err;
                    }

                    // ---- Structural smoothness and forward-progress constraints ----
                    float full_y[OUTPUT_SIZE] = {};
                    full_y[0] = 0.0f;
                    full_y[1] = 0.0f;
                    for (int wp = 1; wp < WAYPOINT_COUNT - 1; wp++) {
                        full_y[wp * 2] = y[(wp - 1) * 2];
                        full_y[wp * 2 + 1] = y[(wp - 1) * 2 + 1];
                    }
                    full_y[(WAYPOINT_COUNT - 1) * 2] = nx;
                    full_y[(WAYPOINT_COUNT - 1) * 2 + 1] = ny;

                    float smooth_grad[OUTPUT_SIZE] = {};
                    float smooth_loss = 0.0f;
                    if (cfg.lambda_smooth > 0.0f) {
                        for (int wp = 0; wp < WAYPOINT_COUNT; wp++) {
                            int i0 = wp * 2, i1 = wp * 2 + 1;
                            if (wp > 0)
                                smooth_grad[i0] += 2.0f * (full_y[i0] - full_y[(wp - 1) * 2]);
                            if (wp < WAYPOINT_COUNT - 1)
                                smooth_grad[i0] -= 2.0f * (full_y[(wp + 1) * 2] - full_y[i0]);
                            if (wp > 0)
                                smooth_grad[i1] += 2.0f * (full_y[i1] - full_y[(wp - 1) * 2 + 1]);
                            if (wp < WAYPOINT_COUNT - 1)
                                smooth_grad[i1] -= 2.0f * (full_y[(wp + 1) * 2 + 1] - full_y[i1]);
                        }
                        for (int wp = 1; wp < WAYPOINT_COUNT; wp++) {
                            float dx_wp = full_y[wp * 2] - full_y[(wp - 1) * 2];
                            float dy_wp = full_y[wp * 2 + 1] - full_y[(wp - 1) * 2 + 1];
                            smooth_loss += dx_wp * dx_wp + dy_wp * dy_wp;
                        }
                        smooth_loss /= (float)(WAYPOINT_COUNT - 1);
                    }

                    float progress_loss = 0.0f;
                    float progress_grad[OUTPUT_SIZE] = {};
                    const float target_len = sqrtf(nx * nx + ny * ny);
                    if (target_len > 0.001f) {
                        const float ux = nx / target_len;
                        const float uy = ny / target_len;
                        for (int wp = 1; wp < WAYPOINT_COUNT; wp++) {
                            const int cur = wp * 2;
                            const int prev = (wp - 1) * 2;
                            const float step_x = full_y[cur] - full_y[prev];
                            const float step_y = full_y[cur + 1] - full_y[prev + 1];
                            const float projection = step_x * ux + step_y * uy;
                            if (projection < 0.0f) {
                                progress_loss += projection * projection;
                                const float gx = 2.0f * projection * ux;
                                const float gy = 2.0f * projection * uy;
                                progress_grad[cur] += gx;
                                progress_grad[cur + 1] += gy;
                                progress_grad[prev] -= gx;
                                progress_grad[prev + 1] -= gy;
                            }
                        }
                    }

                    const float progress_weight = 0.08f;
                    float total_loss = mse_loss + cfg.lambda_smooth * smooth_loss + progress_weight * progress_loss;
                    epoch_loss += total_loss;

                    for (int wp = 1; wp < WAYPOINT_COUNT - 1; wp++) {
                        const int out_idx = (wp - 1) * 2;
                        const int full_idx = wp * 2;
                        dy[out_idx] += cfg.lambda_smooth * smooth_grad[full_idx]
                            + progress_weight * progress_grad[full_idx];
                        dy[out_idx + 1] += cfg.lambda_smooth * smooth_grad[full_idx + 1]
                            + progress_weight * progress_grad[full_idx + 1];
                    }

                    // ---- Backprop ----
                    float da2[HIDDEN2_SIZE] = {};
                    for (int j = 0; j < HIDDEN2_SIZE; j++)
                        for (int i = 0; i < NETWORK_OUTPUT_SIZE; i++)
                            da2[j] += W3[i][j] * dy[i];
                    float dz2[HIDDEN2_SIZE];
                    for (int j = 0; j < HIDDEN2_SIZE; j++)
                        dz2[j] = da2[j] * (1.0f - a2[j] * a2[j]);

                    float da1[HIDDEN1_SIZE] = {};
                    for (int j = 0; j < HIDDEN1_SIZE; j++)
                        for (int i = 0; i < HIDDEN2_SIZE; i++)
                            da1[j] += W2[i][j] * dz2[i];
                    float dz1[HIDDEN1_SIZE];
                    for (int j = 0; j < HIDDEN1_SIZE; j++)
                        dz1[j] = da1[j] * (1.0f - a1[j] * a1[j]);

                    for (int i = 0; i < NETWORK_OUTPUT_SIZE; i++) {
                        for (int j = 0; j < HIDDEN2_SIZE; j++)
                            dW3[i][j] += dy[i] * a2[j];
                        db3[i] += dy[i];
                    }
                    for (int i = 0; i < HIDDEN2_SIZE; i++) {
                        for (int j = 0; j < HIDDEN1_SIZE; j++)
                            dW2[i][j] += dz2[i] * a1[j];
                        db2[i] += dz2[i];
                    }
                    for (int i = 0; i < HIDDEN1_SIZE; i++) {
                        dW1[i][0] += dz1[i] * nx;
                        dW1[i][1] += dz1[i] * ny;
                        db1[i] += dz1[i];
                    }
                }

                // ---- Update weights (SGD + momentum + weight decay + gradient clip) ----
                float scale = 1.0f / (float)bs;

                for (int i = 0; i < HIDDEN1_SIZE; i++) {
                    for (int j = 0; j < INPUT_SIZE; j++) {
                        float g = dW1[i][j] * scale;
                        g += cfg.weight_decay * W1[i][j];  // L2 regularization
                        clip_grad(g, cfg.grad_clip);
                        mW1[i][j] = cfg.momentum * mW1[i][j] - lr * g;
                        W1[i][j] += mW1[i][j];
                    }
                    float gb = db1[i] * scale;
                    gb += cfg.weight_decay * b1[i];
                    clip_grad(gb, cfg.grad_clip);
                    mb1[i] = cfg.momentum * mb1[i] - lr * gb;
                    b1[i] += mb1[i];
                }
                for (int i = 0; i < HIDDEN2_SIZE; i++) {
                    for (int j = 0; j < HIDDEN1_SIZE; j++) {
                        float g = dW2[i][j] * scale;
                        g += cfg.weight_decay * W2[i][j];
                        clip_grad(g, cfg.grad_clip);
                        mW2[i][j] = cfg.momentum * mW2[i][j] - lr * g;
                        W2[i][j] += mW2[i][j];
                    }
                    float gb = db2[i] * scale;
                    gb += cfg.weight_decay * b2[i];
                    clip_grad(gb, cfg.grad_clip);
                    mb2[i] = cfg.momentum * mb2[i] - lr * gb;
                    b2[i] += mb2[i];
                }
                for (int i = 0; i < NETWORK_OUTPUT_SIZE; i++) {
                    for (int j = 0; j < HIDDEN2_SIZE; j++) {
                        float g = dW3[i][j] * scale;
                        g += cfg.weight_decay * W3[i][j];
                        clip_grad(g, cfg.grad_clip);
                        mW3[i][j] = cfg.momentum * mW3[i][j] - lr * g;
                        W3[i][j] += mW3[i][j];
                    }
                    float gb = db3[i] * scale;
                    gb += cfg.weight_decay * b3[i];
                    clip_grad(gb, cfg.grad_clip);
                    mb3[i] = cfg.momentum * mb3[i] - lr * gb;
                    b3[i] += mb3[i];
                }
                batch_count++;
            }

            // ---- Validation ----
            float val_loss = 0.0f;
            {
                float h1[HIDDEN1_SIZE], h2[HIDDEN2_SIZE], pred[NETWORK_OUTPUT_SIZE];
                for (size_t i = 0; i < val_indices.size(); i++) {
                    const auto& s = samples[val_indices[i]];
                    float nx = s.dx / norm_scale, ny = s.dy / norm_scale;
                    for (int j = 0; j < HIDDEN1_SIZE; j++) {
                        float z = b1[j] + W1[j][0] * nx + W1[j][1] * ny;
                        h1[j] = tanhf(z);
                    }
                    for (int j = 0; j < HIDDEN2_SIZE; j++) {
                        float z = b2[j];
                        for (int k = 0; k < HIDDEN1_SIZE; k++) z += W2[j][k] * h1[k];
                        h2[j] = tanhf(z);
                    }
                    for (int j = 0; j < NETWORK_OUTPUT_SIZE; j++) {
                        float z = b3[j];
                        for (int k = 0; k < HIDDEN2_SIZE; k++) z += W3[j][k] * h2[k];
                        pred[j] = z;
                        const int wp = j / 2 + 1;
                        const int coord = j % 2;
                        float err = pred[j] - s.waypoints[wp * 2 + coord] / norm_scale;
                        val_loss += err * err;
                    }
                }
                val_loss /= (float)val_count;
            }

            // ---- Progress reporting ----
            if (cfg.progress)
                cfg.progress->store(progress_ratio, std::memory_order_relaxed);
            if (cfg.status_callback) {
                char buf[64];
                snprintf(buf, sizeof(buf), "轮次 %d/%d loss=%.3f val=%.3f lr=%.5f",
                    ep + 1, cfg.epochs,
                    epoch_loss / std::max(1, batch_count), val_loss, lr);
                cfg.status_callback(buf);
            }

            // ---- Early stopping ----
            if (val_loss < best_val_loss) {
                best_val_loss = val_loss;
                no_improve = 0;
            } else {
                no_improve++;
                if (no_improve >= patience) {
                    if (out_status) *out_status = "提前停止";
                    if (out_final_loss) *out_final_loss = best_val_loss;
                    if (out_val_loss) *out_val_loss = val_loss;
                    if (cfg.progress)
                        cfg.progress->store(1.0f, std::memory_order_relaxed);
                    return true;
                }
            }
        }

        if (out_final_loss) *out_final_loss = best_val_loss;
        if (out_status) *out_status = "完成";
        if (cfg.progress)
            cfg.progress->store(1.0f, std::memory_order_relaxed);
        return true;
    }

    // -----------------------------------------------------------------------
    //  ONNX export (enabled - uses self-contained protobuf writer)
    // -----------------------------------------------------------------------
    bool ExportONNX(const char* path) const {
        return onnx_export::SaveFromFlatWeights(path,
            &W1[0][0], b1, HIDDEN1_SIZE, INPUT_SIZE,
            &W2[0][0], b2, HIDDEN2_SIZE,
            &W3[0][0], b3, NETWORK_OUTPUT_SIZE);
    }

    // -----------------------------------------------------------------------
    //  Binary save/load  (backward compatible, fast I/O)
    // -----------------------------------------------------------------------
    bool Save(const char* path) const {
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;

        const uint32_t magic = 0x324E5A4C;  // "LZN2"
        const uint32_t version = 2;
        const uint32_t network_output_size = NETWORK_OUTPUT_SIZE;
        const uint32_t waypoint_count = WAYPOINT_COUNT;
        f.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        f.write(reinterpret_cast<const char*>(&version), sizeof(version));
        f.write(reinterpret_cast<const char*>(&network_output_size), sizeof(network_output_size));
        f.write(reinterpret_cast<const char*>(&waypoint_count), sizeof(waypoint_count));

        auto wr = [&](const float* p, size_t n) {
            f.write(reinterpret_cast<const char*>(p), n * sizeof(float));
        };
        wr(&W1[0][0], HIDDEN1_SIZE * INPUT_SIZE);
        wr(b1, HIDDEN1_SIZE);
        wr(&W2[0][0], HIDDEN2_SIZE * HIDDEN1_SIZE);
        wr(b2, HIDDEN2_SIZE);
        wr(&W3[0][0], NETWORK_OUTPUT_SIZE * HIDDEN2_SIZE);
        wr(b3, NETWORK_OUTPUT_SIZE);
        wr(&norm_scale, 1);  // save normalization scale

        return f.good();
    }

    bool Load(const char* path) {
        std::string spath(path);
        if (spath.size() >= 5 && spath.substr(spath.size() - 5) == ".onnx") {
            try {
                onnx_net = cv::dnn::readNetFromONNX(path);
                if (!onnx_net.empty()) {
                    cv::Mat probe(1, 2, CV_32F, cv::Scalar(0.0f));
                    onnx_net.setInput(probe, "input");
                    cv::Mat output = onnx_net.forward("output");
                    if (static_cast<int>(output.total()) != NETWORK_OUTPUT_SIZE) {
                        onnx_net = cv::dnn::Net();
                        use_onnx = false;
                        model_loaded = false;
                        return false;
                    }
                    use_onnx = true;
                    model_loaded = true;
                    return true;
                }
            } catch (...) {}
            return false;
        }

        use_onnx = false;
        onnx_net = cv::dnn::Net();

        std::ifstream f(path, std::ios::binary);
        if (!f) return false;

        uint32_t magic = 0;
        uint32_t version = 0;
        uint32_t network_output_size = 0;
        uint32_t waypoint_count = 0;
        f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        f.read(reinterpret_cast<char*>(&version), sizeof(version));
        f.read(reinterpret_cast<char*>(&network_output_size), sizeof(network_output_size));
        f.read(reinterpret_cast<char*>(&waypoint_count), sizeof(waypoint_count));
        if (!f.good() ||
            magic != 0x324E5A4C ||
            version != 2 ||
            network_output_size != NETWORK_OUTPUT_SIZE ||
            waypoint_count != WAYPOINT_COUNT) {
            model_loaded = false;
            return false;
        }

        auto rd = [&](float* p, size_t n) {
            f.read(reinterpret_cast<char*>(p), n * sizeof(float));
        };
        rd(&W1[0][0], HIDDEN1_SIZE * INPUT_SIZE);
        rd(b1, HIDDEN1_SIZE);
        rd(&W2[0][0], HIDDEN2_SIZE * HIDDEN1_SIZE);
        rd(b2, HIDDEN2_SIZE);
        rd(&W3[0][0], NETWORK_OUTPUT_SIZE * HIDDEN2_SIZE);
        rd(b3, NETWORK_OUTPUT_SIZE);

        // Try to read norm_scale (backward compat: if file is old format, keep default)
        float saved_scale = DEFAULT_NORM_SCALE;
        f.read(reinterpret_cast<char*>(&saved_scale), sizeof(float));
        if (f.good() && std::isfinite(saved_scale) && saved_scale > 0.0f)
            norm_scale = saved_scale;

        model_loaded = f.good() || f.eof();
        return model_loaded;
    }

    bool IsModelLoaded() const { return model_loaded; }
    void SetModelLoaded(bool v) { model_loaded = v; }
    float GetNormScale() const { return norm_scale; }

    // -----------------------------------------------------------------------
    //  Validate weights for NaN / Inf
    // -----------------------------------------------------------------------
    bool ValidateWeights(std::string* out_diag = nullptr) const {
        auto check = [&](const float* p, int n, const char* name) -> bool {
            for (int i = 0; i < n; i++) {
                if (!std::isfinite(p[i])) {
                    if (out_diag) {
                        char buf[128]; snprintf(buf, sizeof(buf), "%s[%d]=%f", name, i, (double)p[i]);
                        *out_diag = buf;
                    }
                    return false;
                }
            }
            return true;
        };
        if (!check(&W1[0][0], HIDDEN1_SIZE * INPUT_SIZE, "W1")) return false;
        if (!check(b1, HIDDEN1_SIZE, "b1")) return false;
        if (!check(&W2[0][0], HIDDEN2_SIZE * HIDDEN1_SIZE, "W2")) return false;
        if (!check(b2, HIDDEN2_SIZE, "b2")) return false;
        if (!check(&W3[0][0], NETWORK_OUTPUT_SIZE * HIDDEN2_SIZE, "W3")) return false;
        if (!check(b3, NETWORK_OUTPUT_SIZE, "b3")) return false;
        return true;
    }

    // -----------------------------------------------------------------------
    //  Generate full trajectory path from start to end
    // -----------------------------------------------------------------------
    void GeneratePath(float start_x, float start_y, float end_x, float end_y,
                      int num_points, float* out_x, float* out_y,
                      bool apply_smooth = true) const {
        float dx = end_x - start_x;
        float dy = end_y - start_y;
        float waypoints[OUTPUT_SIZE];
        Predict(dx, dy, waypoints);

        if (apply_smooth)
            SmoothTrajectory(waypoints, WAYPOINT_COUNT);

        for (int i = 0; i < num_points; i++) {
            float t = (float)i / (float)(num_points - 1);
            float idx_f = t * (WAYPOINT_COUNT - 1);
            int idx0 = (int)idx_f;
            int idx1 = std::min(idx0 + 1, WAYPOINT_COUNT - 1);
            float frac = idx_f - idx0;

            float wx = waypoints[idx0 * 2]     * (1.0f - frac) + waypoints[idx1 * 2]     * frac;
            float wy = waypoints[idx0 * 2 + 1] * (1.0f - frac) + waypoints[idx1 * 2 + 1] * frac;

            out_x[i] = start_x + wx;
            out_y[i] = start_y + wy;
        }
        out_x[num_points - 1] = end_x;
        out_y[num_points - 1] = end_y;
    }

private:
    float W1[HIDDEN1_SIZE][INPUT_SIZE] = {};
    float b1[HIDDEN1_SIZE] = {};
    float W2[HIDDEN2_SIZE][HIDDEN1_SIZE] = {};
    float b2[HIDDEN2_SIZE] = {};
    float W3[NETWORK_OUTPUT_SIZE][HIDDEN2_SIZE] = {};
    float b3[NETWORK_OUTPUT_SIZE] = {};

    float mW1[HIDDEN1_SIZE][INPUT_SIZE] = {};
    float mb1[HIDDEN1_SIZE] = {};
    float mW2[HIDDEN2_SIZE][HIDDEN1_SIZE] = {};
    float mb2[HIDDEN2_SIZE] = {};
    float mW3[NETWORK_OUTPUT_SIZE][HIDDEN2_SIZE] = {};
    float mb3[NETWORK_OUTPUT_SIZE] = {};

    mutable cv::dnn::Net onnx_net;
    bool use_onnx = false;
    bool model_loaded = false;
    float norm_scale = DEFAULT_NORM_SCALE;
};
