#pragma once
#include <windows.h>
#include <chrono>

enum class MouseBackend { Win32API = 0 };

class MouseController {
public:
    explicit MouseController(MouseBackend backend);
    ~MouseController();

    MouseController(const MouseController&) = delete;
    MouseController& operator=(const MouseController&) = delete;

    void MoveRelative(int dx, int dy);
    void ClickLeft(int duration_ms = 80);
    void UpdateClick();
    void PollCatch();

    bool HasClickFunction() const { return click_available; }
    bool IsClickPending() const { return click_pending; }
    MouseBackend GetBackend() const { return m_backend; }
    bool IsHidButtonDown(int button) const;
    bool IsHidButtonPressed(int button);
    void EnableHidCatch(int button);
    bool IsHidConnected() const { return false; }
    const char* GetHidStatus() const;

private:
    MouseBackend m_backend = MouseBackend::Win32API;
    bool click_available = true;
    bool click_pending = false;
    std::chrono::steady_clock::time_point click_start_time;
    int click_duration = 0;

    void Win32MoveRelative(int dx, int dy);
    void Win32ClickLeft(int duration_ms);
    void Win32UpdateClick();
};
