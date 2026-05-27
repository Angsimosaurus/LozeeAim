#pragma once
#include <windows.h>
#include <d3d11.h>
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"
#include <string>

class Overlay {
public:
    Overlay();
    ~Overlay();
    HWND GetHwnd() { return hwnd; }

    bool Init();
    void Cleanup();

    void StartFrame();
    void Render();
    bool SetCaptureProtection(bool enabled);
    bool IsCaptureProtectionEnabled() const { return capture_protection_enabled; }

    bool menu_open = false;
    void SetMenuOpen(bool open) { menu_open = open; }

    bool PeekMessages();

    int GetWidth() const { return width; }
    int GetHeight() const { return height; }

private:
    bool CreateDeviceD3D(HWND hWnd);
    void CleanupDeviceD3D();
    void CreateRenderTarget();
    void CleanupRenderTarget();

    HWND hwnd = nullptr;
    WNDCLASSEXW wc = {};
    int width = 0;
    int height = 0;
    bool imgui_initialized = false;
    bool win32_backend_initialized = false;
    bool dx11_backend_initialized = false;
    bool capture_protection_enabled = false;

    ID3D11Device* g_pd3dDevice = nullptr;
    ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
    IDXGISwapChain* g_pSwapChain = nullptr;
    ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
};
