#include "MouseController.hpp"

MouseController::MouseController(MouseBackend backend)
    : m_backend(backend == MouseBackend::Win32API ? backend : MouseBackend::Win32API) {
    click_available = true;
}

MouseController::~MouseController() = default;

void MouseController::MoveRelative(int dx, int dy) {
    Win32MoveRelative(dx, dy);
}

void MouseController::ClickLeft(int duration_ms) {
    if (!click_available) return;
    Win32ClickLeft(duration_ms);
    click_pending = true;
    click_start_time = std::chrono::steady_clock::now();
    click_duration = duration_ms;
}

void MouseController::UpdateClick() {
    if (!click_pending) return;

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - click_start_time).count();

    if (elapsed >= click_duration) {
        Win32UpdateClick();
        click_pending = false;
    }
}

void MouseController::Win32MoveRelative(int dx, int dy) {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseController::Win32ClickLeft(int) {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseController::Win32UpdateClick() {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseController::PollCatch() {}

bool MouseController::IsHidButtonDown(int) const {
    return false;
}

bool MouseController::IsHidButtonPressed(int) {
    return false;
}

void MouseController::EnableHidCatch(int) {}

const char* MouseController::GetHidStatus() const {
    return "HID backend is not included in the open-source edition";
}
