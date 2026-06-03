#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
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
    void SetSize(int w, int h);
    bool SetCaptureProtection(bool enabled);
    bool IsCaptureProtectionEnabled() const { return capture_protection_enabled; }

    bool menu_open = false;
    void SetMenuOpen(bool open) { menu_open = open; }

    void SetPassthrough(bool enabled) { passthrough = enabled; }
    bool GetPassthrough() const { return passthrough; }

    void SetMenuRect(float x, float y, float w, float h) {
        menu_x = x; menu_y = y; menu_w = w; menu_h = h;
    }
    bool HasMenuRect() const { return menu_w > 0.0f && menu_h > 0.0f; }
    bool IsPointInMenu(float x, float y) const {
        return x >= menu_x && x <= menu_x + menu_w &&
               y >= menu_y && y <= menu_y + menu_h;
    }

    bool PeekMessages();

    int GetWidth() const { return width; }
    int GetHeight() const { return height; }

private:
    bool CreateDeviceD3D(HWND hWnd);
    void CleanupDeviceD3D();
    void CreateRenderTarget();
    void CleanupRenderTarget();
    void InitializeBlendState();

    HWND hwnd = nullptr;
    WNDCLASSEXW wc = {};
    int width = 0;
    int height = 0;
    bool imgui_initialized = false;
    bool win32_backend_initialized = false;
    bool dx11_backend_initialized = false;
    bool capture_protection_enabled = false;

    bool passthrough = false;
    float menu_x = 0.0f, menu_y = 0.0f, menu_w = 0.0f, menu_h = 0.0f;

    ID3D11Device* g_pd3dDevice = nullptr;
    ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
    IDXGISwapChain1* g_pSwapChain = nullptr;
    ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
    ID3D11BlendState* g_pBlendState = nullptr;

    IDCompositionDevice* g_dcompDevice = nullptr;
    IDCompositionTarget* g_dcompTarget = nullptr;
    IDCompositionVisual* g_dcompVisual = nullptr;
};
