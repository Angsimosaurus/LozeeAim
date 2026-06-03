// [关键] 强制编译器使用 UTF-8 处理字符串，解决中文乱码
#pragma execution_character_set("utf-8")

#include "Overlay.hpp"
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <iostream>
#include <io.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dxgi.lib") 

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_NCCREATE:
    {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }

    case WM_NCHITTEST:
    {
        Overlay* ov = reinterpret_cast<Overlay*>(
            GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        if (ov && ov->GetPassthrough()) {
            POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
            ::ScreenToClient(hWnd, &pt);
            if (ov->HasMenuRect() && ov->IsPointInMenu((float)pt.x, (float)pt.y))
                return HTCLIENT;
            return HTTRANSPARENT;
        }
        return HTCLIENT;
    }

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
        nullptr, nullptr, wc.hInstance, this
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
    g_pSwapChain->Present(0, 0);
}

void Overlay::SetSize(int w, int h) {
    if (width == w && height == h) return;
    width = w;
    height = h;
    if (hwnd) {
        SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_pSwapChain) {
        CleanupRenderTarget();
        g_pSwapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        CreateRenderTarget();
        if (g_dcompDevice) g_dcompDevice->Commit();
    }
}

void Overlay::InitializeBlendState() {
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_pd3dDevice->CreateBlendState(&bd, &g_pBlendState);
    if (g_pBlendState) {
        g_pd3dDeviceContext->OMSetBlendState(g_pBlendState, nullptr, 0xffffffff);
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
    // 1. D3D11 device (BGRA support required for DComposition)
    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    if (D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
        return false;

    // 2. DXGI factory2
    IDXGIDevice* dxgiDev = nullptr;
    g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDev));

    IDXGIAdapter* adapter = nullptr;
    dxgiDev->GetAdapter(&adapter);

    IDXGIFactory2* factory2 = nullptr;
    {
        IDXGIFactory* baseFactory = nullptr;
        adapter->GetParent(IID_PPV_ARGS(&baseFactory));
        baseFactory->QueryInterface(IID_PPV_ARGS(&factory2));
        baseFactory->Release();
    }

    // 3. Composition swap chain
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = width;
    scd.Height = height;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    scd.Scaling = DXGI_SCALING_STRETCH;

    factory2->CreateSwapChainForComposition(g_pd3dDevice, &scd, nullptr, &g_pSwapChain);
    factory2->Release();
    adapter->Release();

    // 4. DComposition setup
    DCompositionCreateDevice(dxgiDev, IID_PPV_ARGS(&g_dcompDevice));
    dxgiDev->Release();

    g_dcompDevice->CreateTargetForHwnd(hWnd, TRUE, &g_dcompTarget);
    g_dcompDevice->CreateVisual(&g_dcompVisual);
    g_dcompVisual->SetContent(g_pSwapChain);
    g_dcompTarget->SetRoot(g_dcompVisual);
    g_dcompDevice->Commit();

    // 5. Alpha blend state
    InitializeBlendState();

    CreateRenderTarget();
    return true;
}

void Overlay::CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_dcompVisual) { g_dcompVisual->Release(); g_dcompVisual = nullptr; }
    if (g_dcompTarget) { g_dcompTarget->Release(); g_dcompTarget = nullptr; }
    if (g_dcompDevice) { g_dcompDevice->Release(); g_dcompDevice = nullptr; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
    if (g_pBlendState) { g_pBlendState->Release(); g_pBlendState = nullptr; }
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
