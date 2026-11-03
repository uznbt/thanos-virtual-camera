#include "CameraIO.hpp"

#ifndef _WIN32
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>

namespace {

inline uint8_t clampByte(int value) { return static_cast<uint8_t>(std::clamp(value, 0, 255)); }

void yuyvToBgr(const uint8_t* yuyv, Frame& frame) {
    const size_t pairs = static_cast<size_t>(frame.width * frame.height / 2);
    for (size_t i = 0; i < pairs; ++i) {
        int y0 = yuyv[i * 4 + 0];
        int u = yuyv[i * 4 + 1] - 128;
        int y1 = yuyv[i * 4 + 2];
        int v = yuyv[i * 4 + 3] - 128;

        auto toBgr = [u, v](int y, uint8_t* dst) {
            int c = y - 16;
            int r = (298 * c + 409 * v + 128) >> 8;
            int g = (298 * c - 100 * u - 208 * v + 128) >> 8;
            int b = (298 * c + 516 * u + 128) >> 8;
            dst[0] = clampByte(b);
            dst[1] = clampByte(g);
            dst[2] = clampByte(r);
        };
        const size_t pixel = i * 2;
        toBgr(y0, frame.pixels.data() + pixel * 3);
        toBgr(y1, frame.pixels.data() + (pixel + 1) * 3);
    }
}

void bgrToYuyv(const Frame& frame, uint8_t* yuyv) {
    const size_t pixels = static_cast<size_t>(frame.width * frame.height);
    for (size_t i = 0; i < pixels; i += 2) {
        const uint8_t* a = frame.pixels.data() + i * 3;
        const uint8_t* b = a + 3;
        auto calcYuv = [](const uint8_t* p, uint8_t& y, int& u, int& v) {
            int bb = p[0], gg = p[1], rr = p[2];
            y = clampByte(((66 * rr + 129 * gg + 25 * bb + 128) >> 8) + 16);
            u = ((-38 * rr - 74 * gg + 112 * bb + 128) >> 8) + 128;
            v = ((112 * rr - 94 * gg - 18 * bb + 128) >> 8) + 128;
        };
        uint8_t ya, yb;
        int ua, va, ub, vb;
        calcYuv(a, ya, ua, va);
        calcYuv(b, yb, ub, vb);
        int u = (ua + ub) / 2;
        int v = (va + vb) / 2;
        yuyv[i * 2 + 0] = ya;
        yuyv[i * 2 + 1] = clampByte(u);
        yuyv[i * 2 + 2] = yb;
        yuyv[i * 2 + 3] = clampByte(v);
    }
}

bool xioctl(int fd, unsigned long request, void* arg) {
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR);
    return ret != -1;
}

class V4L2DeviceBase {
protected:
    int fd_ = -1;
    std::vector<void*> buffers_;
    std::vector<size_t> bufferSizes_;
    enum v4l2_buf_type type_;
    struct v4l2_format format_{};
    int width_ = 0, height_ = 0;

    ~V4L2DeviceBase() { close(); }

    bool setOutputFormat(int fd, bool isOutput) {
        struct v4l2_format format{};
        format.type = isOutput ? V4L2_BUF_TYPE_VIDEO_OUTPUT : V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = static_cast<__u32>(width_);
        format.fmt.pix.height = static_cast<__u32>(height_);
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        format.fmt.pix.field = V4L2_FIELD_NONE;
        if (!xioctl(fd, VIDIOC_S_FMT, &format)) return false;
        format_ = format;
        return true;
    }

    bool requestBuffers(int fd, v4l2_buf_type type) {
        type_ = type;
        struct v4l2_requestbuffers req{};
        req.count = 4;
        req.type = type;
        req.memory = V4L2_MEMORY_MMAP;
        if (!xioctl(fd, VIDIOC_REQBUFS, &req)) return false;
        if (req.count < 2) return false;

        buffers_.assign(req.count, nullptr);
        bufferSizes_.assign(req.count, 0);
        for (unsigned int i = 0; i < req.count; ++i) {
            struct v4l2_buffer buf{};
            buf.type = type;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (!xioctl(fd, VIDIOC_QUERYBUF, &buf)) return false;
            void* p = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
            if (p == MAP_FAILED) return false;
            buffers_[i] = p;
            bufferSizes_[i] = buf.length;
            buf.flags = 0;
            buf.type = type;
            buf.memory = V4L2_MEMORY_MMAP;
            if (!xioctl(fd, VIDIOC_QBUF, &buf)) return false;
        }
        return true;
    }

    void close() {
        for (size_t i = 0; i < buffers_.size(); ++i) if (buffers_[i] && buffers_[i] != MAP_FAILED) munmap(buffers_[i], bufferSizes_[i]);
        buffers_.clear();
        bufferSizes_.clear();
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
};

class V4L2CameraInput : public CameraInput, private V4L2DeviceBase {
public:
    bool open(const std::string& source, int width, int height, double) override {
        fd_ = ::open(source.c_str(), O_RDWR | O_NONBLOCK);
        if (fd_ < 0) return false;
        width_ = width; height_ = height;
        if (!setOutputFormat(fd_, false) || !requestBuffers(fd_, V4L2_BUF_TYPE_VIDEO_CAPTURE)) return false;
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        return xioctl(fd_, VIDIOC_STREAMON, &type);
    }

    bool read(Frame& frame) override {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd_, &fds);
        struct timeval tv{1, 0};
        if (select(fd_ + 1, &fds, nullptr, nullptr, &tv) <= 0) return false;

        struct v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (!xioctl(fd_, VIDIOC_DQBUF, &buf)) return false;
        if (buf.index >= buffers_.size() || buf.bytesused < static_cast<__u32>(frame.width * frame.height * 2)) {
            xioctl(fd_, VIDIOC_QBUF, &buf);
            return false;
        }
        frame.pixels.resize(static_cast<size_t>(frame.width * frame.height * 3));
        yuyvToBgr(static_cast<const uint8_t*>(buffers_[buf.index]), frame);
        xioctl(fd_, VIDIOC_QBUF, &buf);
        return true;
    }

    void close() override { V4L2DeviceBase::close(); }
};

class V4L2CameraOutput : public CameraOutput, private V4L2DeviceBase {
public:
    bool open(const std::string& device, int width, int height, double) override {
        fd_ = ::open(device.c_str(), O_RDWR | O_NONBLOCK);
        if (fd_ < 0) return false;
        width_ = width; height_ = height;
        if (!setOutputFormat(fd_, true) || !requestBuffers(fd_, V4L2_BUF_TYPE_VIDEO_OUTPUT)) return false;
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        return xioctl(fd_, VIDIOC_STREAMON, &type);
    }

    bool write(const Frame& frame) override {
        struct v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buf.memory = V4L2_MEMORY_MMAP;
        if (!xioctl(fd_, VIDIOC_DQBUF, &buf)) return false;
        bgrToYuyv(frame, static_cast<uint8_t*>(buffers_[buf.index]));
        buf.bytesused = static_cast<__u32>(frame.width * frame.height * 2);
        return xioctl(fd_, VIDIOC_QBUF, &buf);
    }

    void close() override { V4L2DeviceBase::close(); }
};

} // namespace

std::unique_ptr<CameraInput> createCameraInput() { return std::make_unique<V4L2CameraInput>(); }
std::unique_ptr<CameraOutput> createCameraOutput() { return std::make_unique<V4L2CameraOutput>(); }
#endif // !_WIN32
