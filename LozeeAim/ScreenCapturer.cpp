#include "ScreenCapturer.hpp"
#include "Common.hpp"
#include <stdexcept>
#include <iostream>

ScreenCapturer::ScreenCapturer(int crop_width, int crop_height) {
    std::cout << "--- Initializing D3D for screen capture... ---" << std::endl;
    HRESULT hr;

    auto cleanup = [&]() {
        for (auto& texture : m_pStagingTextures) {
            SafeRelease(&texture);
        }
        SafeRelease(&pDuplicator);
        SafeRelease(&pOutput1);
        SafeRelease(&pOutput);
        SafeRelease(&pAdapter);
        SafeRelease(&pFactory);
        SafeRelease(&pContext);
        SafeRelease(&pDevice);
    };

    try {
        hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory);
        if (FAILED(hr)) throw std::runtime_error("Failed to create DXGI Factory.");
        if (FAILED(pFactory->EnumAdapters1(0, &pAdapter))) throw std::runtime_error("Failed to enumerate adapters.");
        if (FAILED(pAdapter->EnumOutputs(0, &pOutput))) throw std::runtime_error("Failed to enumerate outputs.");

        DXGI_OUTPUT_DESC outputDesc;
        pOutput->GetDesc(&outputDesc);
        width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
        height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

        if (FAILED(D3D11CreateDevice(pAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &pDevice, nullptr, &pContext))) {
            throw std::runtime_error("Failed to create D3D11 device.");
        }
        if (FAILED(pOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&pOutput1))) {
            throw std::runtime_error("Failed to query IDXGIOutput1.");
        }
        if (FAILED(pOutput1->DuplicateOutput(pDevice, &pDuplicator))) {
            throw std::runtime_error("Failed to create output duplication.");
        }

        if (!CreateStagingTextures(crop_width, crop_height)) {
            throw std::runtime_error("Failed to create staging texture for cropping.");
        }
        std::cout << "--- Screen capture initialized (" << width << "x" << height << ") ---" << std::endl;
    }
    catch (...) {
        cleanup();
        throw;
    }
}

ScreenCapturer::~ScreenCapturer() {
    std::cout << "--- Cleaning up D3D resources... ---" << std::endl;
    for (auto& texture : m_pStagingTextures) {
        SafeRelease(&texture);
    }
    SafeRelease(&pDuplicator);
    SafeRelease(&pOutput1);
    SafeRelease(&pOutput);
    SafeRelease(&pAdapter);
    SafeRelease(&pFactory);
    SafeRelease(&pContext);
    SafeRelease(&pDevice);
}

bool ScreenCapturer::RecreateDuplication() {
    SafeRelease(&pDuplicator);
    ResetQueuedFrames();
    ResetFrameCache();
    if (!pOutput1 || !pDevice) return false;

    HRESULT hr = pOutput1->DuplicateOutput(pDevice, &pDuplicator);
    if (FAILED(hr)) {
        std::cerr << "[WARN] Failed to recreate output duplication: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    std::cout << "--- Output duplication recreated ---" << std::endl;
    return true;
}

bool ScreenCapturer::CreateStagingTextures(int crop_width, int crop_height) {
    staging_width = crop_width;
    staging_height = crop_height;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = crop_width;
    desc.Height = crop_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    for (auto& texture : m_pStagingTextures) {
        HRESULT hr = pDevice->CreateTexture2D(&desc, nullptr, &texture);
        if (FAILED(hr)) return false;
    }
    ResetQueuedFrames();
    return true;
}

void ScreenCapturer::ResetQueuedFrames() {
    for (bool& pending : m_stagingPending) {
        pending = false;
    }
    m_nextWrite = 0;
    m_nextRead = 0;
}

void ScreenCapturer::StoreFrameCache(const cv::Mat& frame) {
    if (frame.empty()) return;
    frame.copyTo(m_lastGoodFrame);
    m_hasLastGoodFrame = true;
    m_lastFrameWasStale = false;
    m_consecutiveStaleFrames = 0;
}

bool ScreenCapturer::TryUseCachedFrame(cv::Mat& frame) {
    if (!m_hasLastGoodFrame || m_lastGoodFrame.empty()) {
        return false;
    }
    if (m_consecutiveStaleFrames >= kMaxCachedFrameReuse) {
        return false;
    }

    m_lastGoodFrame.copyTo(frame);
    m_lastFrameWasStale = true;
    ++m_consecutiveStaleFrames;
    return true;
}

void ScreenCapturer::ResetFrameCache() {
    m_lastGoodFrame.release();
    m_hasLastGoodFrame = false;
    m_lastFrameWasStale = false;
    m_consecutiveStaleFrames = 0;
}

bool ScreenCapturer::QueueLatestFrame(const cv::Rect& crop_region) {
    if (!pDuplicator || !m_pStagingTextures[m_nextWrite] || m_stagingPending[m_nextWrite]) {
        return false;
    }

    IDXGIResource* pDesktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};

    HRESULT hr = pDuplicator->AcquireNextFrame(kAcquireTimeoutMs, &frameInfo, &pDesktopResource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_ACCESS_LOST ||
            hr == DXGI_ERROR_DEVICE_REMOVED ||
            hr == DXGI_ERROR_DEVICE_RESET) {
            RecreateDuplication();
        }
        return false;
    }

    ID3D11Texture2D* pAcquiredDesktopImage = nullptr;
    hr = pDesktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pAcquiredDesktopImage);
    SafeRelease(&pDesktopResource);
    if (FAILED(hr)) {
        pDuplicator->ReleaseFrame();
        return false;
    }

    D3D11_BOX sourceRegion;
    sourceRegion.left = crop_region.x;
    sourceRegion.right = crop_region.x + crop_region.width;
    sourceRegion.top = crop_region.y;
    sourceRegion.bottom = crop_region.y + crop_region.height;
    sourceRegion.front = 0;
    sourceRegion.back = 1;

    pContext->CopySubresourceRegion(
        m_pStagingTextures[m_nextWrite],
        0,
        0,
        0,
        0,
        pAcquiredDesktopImage,
        0,
        &sourceRegion);

    m_stagingPending[m_nextWrite] = true;
    m_nextWrite = (m_nextWrite + 1) % kStagingTextureCount;
    pContext->Flush();

    SafeRelease(&pAcquiredDesktopImage);
    pDuplicator->ReleaseFrame();
    return true;
}

bool ScreenCapturer::TryReadQueuedFrame(cv::Mat& frame) {
    for (int i = 0; i < kStagingTextureCount; ++i) {
        const int index = (m_nextRead + i) % kStagingTextureCount;
        if (!m_stagingPending[index]) continue;

        D3D11_MAPPED_SUBRESOURCE mappedResource;
        HRESULT hr = pContext->Map(
            m_pStagingTextures[index],
            0,
            D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT,
            &mappedResource);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            return false;
        }
        if (FAILED(hr)) {
            m_stagingPending[index] = false;
            m_nextRead = (index + 1) % kStagingTextureCount;
            return false;
        }

        cv::Mat bgra_frame(
            staging_height,
            staging_width,
            CV_8UC4,
            mappedResource.pData,
            mappedResource.RowPitch);
        cv::cvtColor(bgra_frame, frame, cv::COLOR_BGRA2BGR);

        pContext->Unmap(m_pStagingTextures[index], 0);
        m_stagingPending[index] = false;
        m_nextRead = (index + 1) % kStagingTextureCount;
        return true;
    }

    return false;
}

bool ScreenCapturer::CaptureFrame(cv::Mat& frame, const cv::Rect& crop_region) {
    m_lastFrameWasStale = false;
    QueueLatestFrame(crop_region);
    if (TryReadQueuedFrame(frame)) {
        StoreFrameCache(frame);
        return true;
    }
    return TryUseCachedFrame(frame);
}
