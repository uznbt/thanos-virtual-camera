#ifdef _WIN32
#include "CameraIO.hpp"

#include <windows.h>
#include <winsock2.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mfobjects.h>
#include <dshow.h>

#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

uint8_t clamp8(int x) { return static_cast<uint8_t>(std::clamp(x, 0, 255)); }

struct FrameGeometry {
    int width = 0;
    int height = 0;
    GUID preferredSubtype = GUID_NULL;
};

bool getFrameGeometry(IMFMediaType* type, FrameGeometry& geometry) {
    BOOL selected = FALSE;
    HRESULT hr = type->GetGUID(MF_MT_SUBTYPE, &geometry.preferredSubtype);
    if (FAILED(hr)) return false;

    UINT64 size = 0;
    hr = type->GetUINT64(MF_MT_FRAME_SIZE, &size);
    if (FAILED(hr)) return false;
    geometry.width = static_cast<int>(size >> 32);
    geometry.height = static_cast<int>(size & 0xffffffffULL);
    return geometry.width > 0 && geometry.height > 0;
}

enum class MediaFormat { YUY2, RGB32, NV12 };

MediaFormat selectMediaFormat() { return MediaFormat::YUY2; }

void yuyvToBgr(const uint8_t* yuyv, int width, int height, int pitch, Frame& frame) {
    frame.pixels.assign(static_cast<size_t>(width * height * 3), 0);
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = yuyv + y * pitch;
        uint8_t* dst = frame.pixels.data() + y * width * 3;
        for (int x = 0; x < width; x += 2) {
            int y0 = src[0];
            int u = src[1] - 128;
            int y1 = src[2];
            int v = src[3] - 128;
            auto writePixel = [&](uint8_t intensity, uint8_t* out) {
                int c = intensity - 16;
                out[0] = clamp8((298 * c + 516 * u + 128) >> 8);
                out[1] = clamp8((298 * c - 100 * u - 208 * v + 128) >> 8);
                out[2] = clamp8((298 * c + 409 * v + 128) >> 8);
            };
            writePixel(static_cast<uint8_t>(y0), dst + x * 3);
            writePixel(static_cast<uint8_t>(y1), dst + (x + 1) * 3);
            src += 4;
        }
    }
}

void rgb32ToBgr(const uint8_t* bgra, int width, int height, int pitch, Frame& frame) {
    frame.pixels.assign(static_cast<size_t>(width * height * 3), 0);
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = bgra + y * pitch;
        uint8_t* dst = frame.pixels.data() + y * width * 3;
        for (int x = 0; x < width; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 0];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
        }
    }
}

void nv12ToBgr(const uint8_t* nv12, int width, int height, int pitch, Frame& frame) {
    frame.pixels.assign(static_cast<size_t>(width * height * 3), 0);
    const uint8_t* uvPlane = nv12 + width * height;
    for (int y = 0; y < height; ++y) {
        const uint8_t* yPlane = nv12 + y * pitch;
        uint8_t* dst = frame.pixels.data() + y * width * 3;
        for (int x = 0; x < width; ++x) {
            int yy = yPlane[x] - 16;
            int u = uvPlane[(y / 2) * pitch + (x & ~1)] - 128;
            int v = uvPlane[(y / 2) * pitch + (x & ~1) + 1] - 128;
            dst[x * 3 + 0] = clamp8((298 * yy + 516 * u + 128) >> 8);
            dst[x * 3 + 1] = clamp8((298 * yy - 100 * u - 208 * v + 128) >> 8);
            dst[x * 3 + 2] = clamp8((298 * yy + 409 * v + 128) >> 8);
        }
    }
}

class MediaFoundationInput : public CameraInput {
    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFPresentationDescriptor> presentation_;
    ComPtr<IMFSourceReader> reader_;
    ComPtr<IMFMediaType> mediaType_;
    GUID subtype_ = GUID_NULL;
    int width_ = 0, height_ = 0;
    bool coInitialized_ = false;
    bool mfStarted_ = false;

    bool enumerateDevice(ComPtr<IMFMediaSource>& selected) {
        ComPtr<IMFAttributes> attributes;
        HRESULT hr = MFCreateAttributes(&attributes, 1);
        if (FAILED(hr)) return false;
        hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                 MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (FAILED(hr)) return false;

        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        hr = MFEnumDeviceSources(attributes.Get(), &devices, &count);
        if (FAILED(hr) || count == 0 || !devices) return false;

        bool activated = false;
        for (UINT32 i = 0; i < count && !activated; ++i) {
            hr = devices[i]->ActivateObject(__uuidof(IMFMediaSource), reinterpret_cast<void**>(selected.GetAddressOf()));
            if (SUCCEEDED(hr) && selected) activated = true;
        }
        for (UINT32 i = 0; i < count; ++i) if (devices[i]) devices[i]->Release();
        CoTaskMemFree(devices);
        return activated;
    }

    bool selectMediaType() {
        ComPtr<IMFPresentationDescriptor> desc;
        HRESULT hr = source_->CreatePresentationDescriptor(&desc);
        if (FAILED(hr)) return false;

        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> descriptor;
        hr = desc->GetStreamDescriptorByIndex(0, &selected, &descriptor);
        if (FAILED(hr) || !selected) return false;

        ComPtr<IMFMediaTypeHandler> handler;
        hr = descriptor->GetMediaTypeHandler(&handler);
        if (FAILED(hr)) return false;

        DWORD count = 0;
        hr = handler->GetMediaTypeCount(&count);
        if (FAILED(hr)) return false;

        FrameGeometry chosen{};
        for (DWORD i = 0; i < count; ++i) {
            ComPtr<IMFMediaType> type;
            if (FAILED(handler->GetMediaType(i, &type))) continue;
            FrameGeometry geometry;
            if (!getFrameGeometry(type.Get(), geometry)) continue;
            if (!IsEqualGUID(geometry.preferredSubtype, MFVideoFormat_YUY2) &&
                !IsEqualGUID(geometry.preferredSubtype, MFVideoFormat_NV12) &&
                !IsEqualGUID(geometry.preferredSubtype, MFVideoFormat_RGB32)) continue;
            if (IsEqualGUID(geometry.preferredSubtype, MFVideoFormat_YUY2)) {
                chosen = geometry;
                mediaType_ = type;
                break;
            }
            if (chosen.preferredSubtype == GUID_NULL) {
                chosen = geometry;
                mediaType_ = type;
            }
        }

        if (mediaType_) {
            hr = handler->SetCurrentMediaType(mediaType_.Get());
            if (FAILED(hr)) return false;
            subtype_ = chosen.preferredSubtype;
            width_ = chosen.width;
            height_ = chosen.height;
            presentation_ = desc;
            return true;
        }
        return false;
    }

    bool chooseDirectReadPath(ComPtr<IMFSourceReader>& reader) {
        HRESULT hr = MFCreateSourceReaderFromMediaSource(source_.Get(), nullptr, &reader);
        if (FAILED(hr) || !reader) return false;
        hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mediaType_.Get());
        return SUCCEEDED(hr);
    }

public:
    bool open(const std::string&, int, int, double) override {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        coInitialized_ = SUCCEEDED(hr) || hr == S_FALSE;
        hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        mfStarted_ = SUCCEEDED(hr);
        if (!mfStarted_) return false;

        if (!enumerateDevice(source_)) return false;
        if (!selectMediaType()) return false;
        if (!chooseDirectReadPath(reader_)) return false;
        return true;
    }

    bool read(Frame& frame) override {
        if (!reader_) return false;
        DWORD streamIndex = 0, flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &timestamp, &sample);
        if (FAILED(hr) || !sample) return false;

        ComPtr<IMFMediaBuffer> buffer;
        hr = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(hr) || !buffer) return false;

        BYTE* data = nullptr;
        int pitch = 0;
        bool using2d = false;
        ComPtr<IMF2DBuffer> buffer2d;
        if (SUCCEEDED(buffer.As(&buffer2d)) && buffer2d) {
            LONG lockPitch = 0;
            if (SUCCEEDED(buffer2d->Lock2D(&data, &lockPitch)) && data && lockPitch > 0) {
                pitch = static_cast<int>(lockPitch);
                using2d = true;
            } else {
                data = nullptr;
            }
        }

        if (!data) {
            DWORD maxLength = 0;
            DWORD currentLength = 0;
            HRESULT lockHr = buffer->Lock(&data, &maxLength, &currentLength);
            if (FAILED(lockHr)) return false;
            if (IsEqualGUID(subtype_, MFVideoFormat_YUY2)) pitch = width_ * 2;
            else if (IsEqualGUID(subtype_, MFVideoFormat_NV12)) pitch = width_;
            else pitch = width_ * 4;
        }

        frame = Frame(width_, height_);
        if (IsEqualGUID(subtype_, MFVideoFormat_YUY2)) yuyvToBgr(data, width_, height_, pitch, frame);
        else if (IsEqualGUID(subtype_, MFVideoFormat_RGB32)) rgb32ToBgr(data, width_, height_, pitch, frame);
        else if (IsEqualGUID(subtype_, MFVideoFormat_NV12)) nv12ToBgr(data, width_, height_, pitch, frame);
        else { buffer->Unlock(); return false; }

        if (using2d) buffer2d->Unlock2D();
        else buffer->Unlock();
        return true;
    }

    void close() override {
        reader_.Reset();
        mediaType_.Reset();
        presentation_.Reset();
        if (source_) source_->Shutdown();
        source_.Reset();
        if (mfStarted_) MFShutdown();
        if (coInitialized_) CoUninitialize();
    }
};

void bgrToNv12(const Frame& frame, std::vector<uint8_t>& nv12) {
    const size_t pixels = static_cast<size_t>(frame.width * frame.height);
    nv12.assign(pixels + pixels / 2, 0);
    uint8_t* y_plane = nv12.data();
    uint8_t* uv_plane = y_plane + pixels;
    auto luma = [](const uint8_t* p) { return clamp8(((66 * p[2] + 129 * p[1] + 25 * p[0] + 128) >> 8) + 16); };
    auto chroma = [](const uint8_t* p, bool cb) {
        return cb ? clamp8(((-38 * p[2] - 74 * p[1] + 112 * p[0] + 128) >> 8) + 128)
                  : clamp8(((112 * p[2] - 94 * p[1] - 18 * p[0] + 128) >> 8) + 128);
    };
    for (int y = 0; y < frame.height; ++y)
        for (int x = 0; x < frame.width; ++x)
            y_plane[y * frame.width + x] = luma(frame.ptr(x, y));
    for (int y = 0; y < frame.height; y += 2) {
        for (int x = 0; x < frame.width; x += 2) {
            const uint8_t* p00 = frame.ptr(x, y);
            const uint8_t* p01 = frame.ptr(x + 1, y);
            const uint8_t* p10 = frame.ptr(x, y + 1);
            const uint8_t* p11 = frame.ptr(x + 1, y + 1);
            int u = (chroma(p00, true) + chroma(p01, true) + chroma(p10, true) + chroma(p11, true)) / 4;
            int v = (chroma(p00, false) + chroma(p01, false) + chroma(p10, false) + chroma(p11, false)) / 4;
            size_t index = (y / 2) * frame.width + x;
            uv_plane[index] = static_cast<uint8_t>(u);
            uv_plane[index + 1] = static_cast<uint8_t>(v);
        }
    }
}

struct QueueHeader {
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
    volatile uint32_t state;
    uint32_t offsets[3];
    uint32_t type;
    uint32_t cx;
    uint32_t cy;
    uint64_t interval;
    uint32_t reserved[8];
};

constexpr uint32_t kFrameHeaderSize = 32;
constexpr uint32_t kQueueVideo = 0;
constexpr uint32_t kQueueStarting = 1;
constexpr uint32_t kQueueReady = 2;
constexpr uint32_t kQueueStopping = 3;

class OBSVirtualCamera : public CameraOutput {
    HANDLE mapping_ = nullptr;
    QueueHeader* header_ = nullptr;
    uint8_t* ts_[3] = {};
    uint8_t* frame_[3] = {};
    uint64_t sequence_ = 0;
    int width_ = 0, height_ = 0;

    static DWORD align32(uint32_t value) { return (value + 31) & ~31U; }

    bool openExisting(int width, int height, double fps) {
        mapping_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"OBSVirtualCamVideo");
        if (!mapping_) return false;
        header_ = static_cast<QueueHeader*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!header_) { CloseHandle(mapping_); mapping_ = nullptr; return false; }
        width_ = static_cast<int>(header_->cx);
        height_ = static_cast<int>(header_->cy);
        if (width_ != width || height_ != height) return false;
        for (int i = 0; i < 3; ++i) {
            auto base = reinterpret_cast<uint8_t*>(header_);
            ts_[i] = base + header_->offsets[i];
            frame_[i] = base + header_->offsets[i] + kFrameHeaderSize;
        }
        return true;
    }

    bool createQueue(int width, int height, double fps) {
        size_t frameBytes = static_cast<size_t>(width) * height * 3 / 2;
        DWORD size = align32(static_cast<uint32_t>(sizeof(QueueHeader)));
        uint32_t offsets[3] = {};
        for (int i = 0; i < 3; ++i) {
            offsets[i] = size;
            size += static_cast<uint32_t>(frameBytes + kFrameHeaderSize);
            size = align32(size);
        }
        QueueHeader header{};
        header.state = kQueueStarting;
        header.offsets[0] = offsets[0]; header.offsets[1] = offsets[1]; header.offsets[2] = offsets[2];
        header.type = kQueueVideo;
        header.cx = static_cast<uint32_t>(width);
        header.cy = static_cast<uint32_t>(height);
        header.interval = static_cast<uint64_t>(10000000.0 / fps);

        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, L"OBSVirtualCamVideo");
        if (!mapping_) return false;
        header_ = static_cast<QueueHeader*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!header_) { CloseHandle(mapping_); mapping_ = nullptr; return false; }
        memcpy(header_, &header, sizeof(header));
        for (int i = 0; i < 3; ++i) {
            auto base = reinterpret_cast<uint8_t*>(header_);
            ts_[i] = base + offsets[i];
            frame_[i] = base + offsets[i] + kFrameHeaderSize;
        }
        width_ = width;
        height_ = height;
        return true;
    }

public:
    bool open(const std::string&, int width, int height, double fps) override {
        if (!openExisting(width, height, fps)) return createQueue(width, height, fps);
        return true;
    }

    bool write(const Frame& frame) override {
        if (!frame_[0]) return false;
        std::vector<uint8_t> nv12;
        bgrToNv12(frame, nv12);
        size_t nv12Y = static_cast<size_t>(frame.width * frame.height);
        unsigned long inc = InterlockedIncrement(reinterpret_cast<volatile long*>(&header_->write_idx));
        size_t index = inc % 3;
        *reinterpret_cast<uint64_t*>(ts_[index]) = static_cast<uint64_t>(GetTickCount64()) * 10000000ULL / 1000ULL;
        memcpy(frame_[index], nv12.data(), nv12Y);
        memcpy(frame_[index] + nv12Y, nv12.data() + nv12Y, nv12Y / 2);
        header_->read_idx = static_cast<uint32_t>(inc);
        header_->state = kQueueReady;
        return true;
    }

    void close() override {
        if (header_) header_->state = kQueueStopping;
        if (header_) UnmapViewOfFile(header_);
        if (mapping_) CloseHandle(mapping_);
        header_ = nullptr;
        mapping_ = nullptr;
    }
};

} // namespace

std::unique_ptr<CameraInput> createCameraInput() { return std::make_unique<MediaFoundationInput>(); }
std::unique_ptr<CameraOutput> createCameraOutput() { return std::make_unique<OBSVirtualCamera>(); }
#endif // _WIN32
