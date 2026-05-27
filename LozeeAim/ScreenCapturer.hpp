#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <opencv2/opencv.hpp>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

class ScreenCapturer {
public:
    ScreenCapturer(int crop_width, int crop_height);
    ~ScreenCapturer();

    ScreenCapturer(const ScreenCapturer&) = delete;
    ScreenCapturer& operator=(const ScreenCapturer&) = delete;

    bool CaptureFrame(cv::Mat& frame, const cv::Rect& crop_region);
    bool LastFrameWasStale() const { return m_lastFrameWasStale; }
    int GetConsecutiveStaleFrames() const { return m_consecutiveStaleFrames; }

    int getWidth() const { return width; }
    int getHeight() const { return height; }

private:
    bool RecreateDuplication();
    bool CreateStagingTextures(int crop_width, int crop_height);
    bool QueueLatestFrame(const cv::Rect& crop_region);
    bool TryReadQueuedFrame(cv::Mat& frame);
    void ResetQueuedFrames();
    void StoreFrameCache(const cv::Mat& frame);
    bool TryUseCachedFrame(cv::Mat& frame);
    void ResetFrameCache();

    static constexpr int kStagingTextureCount = 2;
    static constexpr UINT kAcquireTimeoutMs = 1;
    static constexpr int kMaxCachedFrameReuse = 3;

    ID3D11Texture2D* m_pStagingTextures[kStagingTextureCount] = {};
    bool m_stagingPending[kStagingTextureCount] = {};
    int m_nextWrite = 0;
    int m_nextRead = 0;
    cv::Mat m_lastGoodFrame;
    bool m_hasLastGoodFrame = false;
    bool m_lastFrameWasStale = false;
    int m_consecutiveStaleFrames = 0;
    int staging_width = 0;
    int staging_height = 0;
    IDXGIFactory1* pFactory = nullptr;
    IDXGIAdapter1* pAdapter = nullptr;
    IDXGIOutput* pOutput = nullptr;
    IDXGIOutput1* pOutput1 = nullptr;
    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pContext = nullptr;
    IDXGIOutputDuplication* pDuplicator = nullptr;

    int width = 0;
    int height = 0;
};
