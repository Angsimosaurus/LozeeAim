#pragma once
#include <algorithm>
#include <cmath>

class PIDController {
public:
    PIDController(float kp, float ki, float kd, float max_output)
        : Kp(kp), Ki(ki), Kd(kd), max_output(max_output),
        prev_error(0.0f), integral(0.0f), first_run(true) {
    }

    void Reset() {
        prev_error = 0.0f;
        integral = 0.0f;
        first_run = true;
    }

    void UpdateParams(float kp, float ki, float kd) {
        Kp = kp;
        Ki = ki;
        Kd = kd;
    }

    float Compute(float error, float dt) {
        if (dt <= 0.0f) return 0.0f;

        float P = Kp * error;

        integral += error * dt;
        float i_clamp = max_output / (Ki + 0.0001f);
        integral = std::clamp(integral, -i_clamp, i_clamp);
        float I = Ki * integral;

        float D = 0.0f;
        if (!first_run) {
            float derivative = (error - prev_error) / dt;
            D = Kd * derivative;
        }
        first_run = false;

        prev_error = error;

        float output = P + I + D;

        if (output > max_output) output = max_output;
        if (output < -max_output) output = -max_output;

        return output;
    }

private:
    float Kp, Ki, Kd;
    float max_output;
    float prev_error;
    float integral;
    bool first_run;
};