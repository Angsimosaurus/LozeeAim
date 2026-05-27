// [关键] 强制编译器使用 UTF-8 处理字符串，解决中文乱码
#pragma execution_character_set("utf-8")

#include "Overlay.hpp"
#include <dwmapi.h>
#include <iostream>
#include <io.h> 

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib") 

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        break;  // let DefWindowProc track focus so keyboard input works

    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) {
            SetFocus(hWnd);
        }
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        SetFocus(hWnd);
        break;

    case WM_SIZE:
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

Overlay::Overlay() {
    width = GetSystemMetrics(SM_CXSCREEN);
    height = GetSystemMetrics(SM_CYSCREEN);
}

Overlay::~Overlay() {
    Cleanup();
}

void SetupStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.ScrollbarSize = 12.0f;
    ImGui::StyleColorsLight();
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.94f, 0.94f, 0.94f, 0.70f);
}

bool Overlay::Init() {
    wc = { sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"LozeeAim", nullptr };
    RegisterClassExW(&wc);

    hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"",
        WS_POPUP,
        0, 0, width, height,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (!hwnd) return false;

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    if (_access("font.ttc", 0) != -1) {
        io.Fonts->AddFontFromFileTTF("font.ttc", 18.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
        std::cout << "[INFO] Loaded local font.ttc" << std::endl;
    }
    else {
        char winDir[MAX_PATH];
        GetWindowsDirectoryA(winDir, MAX_PATH);
        std::string fontPath = std::string(winDir) + "\\Fonts\\msyh.ttc";
        io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 18.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
        std::cout << "[INFO] Loaded system msyh.ttc" << std::endl;
    }

    SetupStyle();

    ImGui_ImplWin32_Init(hwnd);
    win32_backend_initialized = true;
    ImGui_ImplWin32_EnableAlphaCompositing(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    dx11_backend_initialized = true;
    imgui_initialized = true;

    return true;
}

void Overlay::StartFrame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void Overlay::Render() {
    ImGui::Render();

    const float clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    HRESULT hr = g_pSwapChain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
    if (FAILED(hr)) {
        g_pSwapChain->Present(0, 0);
    }
}

bool Overlay::SetCaptureProtection(bool enabled) {
    if (!hwnd) return false;

    if (!enabled) {
        if (!SetWindowDisplayAffinity(hwnd, WDA_NONE)) return false;
        capture_protection_enabled = false;
        return true;
    }

    if (SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE) ||
        SetWindowDisplayAffinity(hwnd, WDA_MONITOR)) {
        capture_protection_enabled = true;
        return true;
    }

    capture_protection_enabled = false;
    return false;
}

bool Overlay::PeekMessages() {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (msg.message == WM_QUIT) return false;
    }
    return true;
}

void Overlay::Cleanup() {
    if (dx11_backend_initialized) {
        ImGui_ImplDX11_Shutdown();
        dx11_backend_initialized = false;
    }
    if (win32_backend_initialized) {
        ImGui_ImplWin32_Shutdown();
        win32_backend_initialized = false;
    }
    if (imgui_initialized) {
        ImGui::DestroyContext();
        imgui_initialized = false;
    }
    CleanupDeviceD3D();
    if (hwnd) {
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
    if (wc.lpszClassName && wc.hInstance) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        wc = {};
    }
}

bool Overlay::CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
        return false;
    CreateRenderTarget();
    return true;
}

void Overlay::CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void Overlay::CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void Overlay::CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}
