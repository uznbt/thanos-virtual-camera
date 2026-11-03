#include "CameraIO.hpp"
#include "Frame.hpp"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <onnxruntime_cxx_api.h>
#ifdef __linux__
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <chrono>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
using SOCKET_T = SOCKET;
using LENGTH_T = int;
static const SOCKET_T INVALID_SOCKET_T = INVALID_SOCKET;
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
using SOCKET_T = int;
using LENGTH_T = socklen_t;
static const SOCKET_T INVALID_SOCKET_T = -1;
#define close_socket close
#endif


#undef min
#undef max

struct Config {
    std::string source = "0";
    std::string dst = "/dev/video10";
    int width = 640, height = 480;
    double fps = 30.0;
    float threshold = 0.5f;
    float smooth = 0.55f;
    bool flip = true;
    bool noPreview = true;
};

struct SharedState {
    std::mutex mutex;
    std::string mode = "normal";
    std::string pending;
    std::string pendingValue;
    bool bgCapture = false;
    bool bgReset = false;
    bool flip = false;
    float threshold = 0.5f;
    Frame latest;
    bool latestReady = false;
    uint64_t frameSerial = 0;
};

static SharedState shared;
static std::atomic<bool> running{true};

static void printUsage(const char* program) {
    std::cerr << "Usage: " << program << " [--src 0|/dev/video0] [--dst /dev/video10] [--threshold 0.5] [--smooth 0.55]\n";
}

static std::string repositoryRoot() {
#ifdef _WIN32
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p(path);
    size_t pos = p.find_last_of("\\/");
    return p.substr(0, pos);
#else
    std::vector<char> buf(4096);
    size_t n = readlink("/proc/self/exe", buf.data(), buf.size());
    std::string s(buf.data(), n > 0 ? n : 0);
    size_t pos = s.find_last_of('/');
    return pos == std::string::npos ? std::string(".") : s.substr(0, pos);
#endif
}

static std::vector<uint8_t> loadBinaryFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return in ? std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) : std::vector<uint8_t>();
}

class Segmenter {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "thanos-camera"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
    Ort::AllocatorWithDefaultOptions allocator;
    std::vector<const char*> inputNames;
    std::vector<std::string> inputNameStorage;
    std::vector<const char*> outputNames;
    std::vector<std::string> outputNameStorage;
    int inputWidth = 320, inputHeight = 320;

public:
    bool load(const std::string& path, std::string& error) {
        try {
            options.SetIntraOpNumThreads(1);
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            session = std::make_unique<Ort::Session>(env, path.c_str(), options);
            Ort::AllocatorWithDefaultOptions allocatorReal;

            size_t count = session->GetInputCount();
            inputNameStorage.resize(count);
            inputNames.resize(count);
            for (size_t i = 0; i < count; ++i) {
                auto name = session->GetInputNameAllocated(i, allocatorReal);
                inputNameStorage[i] = name.get();
                inputNames[i] = inputNameStorage[i].c_str();
            }

            count = session->GetOutputCount();
            outputNameStorage.resize(count);
            outputNames.resize(count);
            for (size_t i = 0; i < count; ++i) {
                auto name = session->GetOutputNameAllocated(i, allocatorReal);
                outputNameStorage[i] = name.get();
                outputNames[i] = outputNameStorage[i].c_str();
            }
            return true;
        } catch (const Ort::Exception& e) {
            error = std::string("ONNX Runtime: ") + e.what();
            return false;
        }
    }

    std::vector<float> predict(const Frame& frame) {
        const int w = inputWidth, h = inputHeight;
        std::vector<float> input(static_cast<size_t>(3 * w * h));
        for (int y = 0; y < h; ++y) {
            int sy = static_cast<int>((static_cast<double>(y) + 0.5) / h * frame.height);
            sy = std::clamp(sy, 0, frame.height - 1);
            for (int x = 0; x < w; ++x) {
                int sx = static_cast<int>((static_cast<double>(x) + 0.5) / w * frame.width);
                sx = std::clamp(sx, 0, frame.width - 1);
                const uint8_t* p = frame.ptr(sx, sy);
                float r = p[2] / 255.0f;
                float g = p[1] / 255.0f;
                float b = p[0] / 255.0f;
                input[(0 * h + y) * w + x] = (r - 0.485f) / 0.229f;
                input[(1 * h + y) * w + x] = (g - 0.456f) / 0.224f;
                input[(2 * h + y) * w + x] = (b - 0.406f) / 0.225f;
            }
        }

        std::vector<int64_t> shape{1, 3, h, w};
        Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size());
        Ort::RunOptions runOptions{nullptr};
        auto outputs = session->Run(runOptions, inputNames.data(), &tensor, 1, outputNames.data(), outputNames.size());
        if (outputs.empty()) return {};
        float* out = outputs[0].GetTensorMutableData<float>();
        const char* debugEnv = std::getenv("THANOS_DEBUG_SEG");
        if (debugEnv && *debugEnv) {
            float mn = out[0], mx = out[0];
            double sum = 0.0;
            int aboveHalf = 0;
            const size_t n = static_cast<size_t>(w * h);
            for (size_t i = 0; i < n; ++i) { mn = std::min(mn, out[i]); mx = std::max(mx, out[i]); sum += out[i]; if (out[i] >= 0.5f) ++aboveHalf; }
            std::cerr << "[segment] min=" << mn << " max=" << mx << " mean=" << (sum / n) << " >=0.5=" << (100.0 * aboveHalf / n) << "%" << std::endl;
        }
        return std::vector<float>(out, out + w * h);
    }
};

struct Particle {
    float x = 0, y = 0;
    float vx = 0, vy = 0;
    uint8_t b = 0, g = 0, r = 0;
    int size = 2;
};

struct GatherParticle {
    float sx = 0, sy = 0, tx = 0, ty = 0;
    uint8_t b = 0, g = 0, r = 0;
    int size = 2;
};

static void blendFrames(Frame& dst, const Frame& src, float amount) {
    amount = std::clamp(amount, 0.0f, 1.0f);
    if (amount <= 0.0f) return;
    for (size_t i = 0; i < dst.pixels.size(); ++i) dst.pixels[i] = static_cast<uint8_t>(dst.pixels[i] * (1.0f - amount) + src.pixels[i] * amount);
}

static std::vector<Particle> makeParticles(const std::vector<float>& mask, int width, int height, const Frame& frame, float threshold, std::mt19937& random) {
    std::vector<Particle> out;
    std::vector<size_t> indices;
    for (size_t i = 0; i < mask.size(); ++i) if (mask[i] >= threshold) indices.push_back(i);
    size_t step = std::max<size_t>(1, indices.size() / 2500);
    std::uniform_real_distribution<float> vx(-1.5f, 1.5f), vy(-3.0f, -0.5f);
    std::uniform_int_distribution<int> sz(1, 4);
    for (size_t j = 0; j < indices.size(); j += step) {
        size_t i = indices[j];
        size_t x = i % width, y = i / width;
        const uint8_t* p = frame.ptr(x, y);
        out.push_back({static_cast<float>(x), static_cast<float>(y), vx(random), vy(random), p[0], p[1], p[2], sz(random)});
    }
    return out;
}

static std::vector<GatherParticle> makeGatherParticles(const std::vector<float>& mask, int width, int height, const Frame& frame, float threshold, std::mt19937& random) {
    std::vector<GatherParticle> out;
    std::vector<size_t> indices;
    for (size_t i = 0; i < mask.size(); ++i) if (mask[i] >= threshold) indices.push_back(i);
    size_t step = std::max<size_t>(1, indices.size() / 2500);
    std::uniform_real_distribution<float> scatter(-80.0f, 80.0f);
    std::uniform_int_distribution<int> sz(1, 4);
    for (size_t j = 0; j < indices.size(); j += step) {
        size_t i = indices[j];
        int x = static_cast<int>(i % width), y = static_cast<int>(i / width);
        const uint8_t* p = frame.ptr(x, y);
        out.push_back({x + scatter(random), y + scatter(random), static_cast<float>(x), static_cast<float>(y), p[0], p[1], p[2], sz(random)});
    }
    return out;
}

static void drawCircle(Frame& frame, int cx, int cy, int radius, uint8_t b, uint8_t g, uint8_t r) {
    if (radius <= 0) return;
    int rr = radius * radius;
    for (int y = std::max(0, cy - radius); y <= std::min(frame.height - 1, cy + radius); ++y) {
        for (int x = std::max(0, cx - radius); x <= std::min(frame.width - 1, cx + radius); ++x) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= rr) {
                uint8_t* p = frame.ptr(x, y);
                p[0] = b; p[1] = g; p[2] = r;
            }
        }
    }
}

static std::vector<float> resizeMask(const std::vector<float>& small, int sw, int sh, int dw, int dh) {
    std::vector<float> out(static_cast<size_t>(dw * dh));
    for (int y = 0; y < dh; ++y) {
        int sy = static_cast<int>((y + 0.5f) / dh * sh);
        sy = std::clamp(sy, 0, sh - 1);
        for (int x = 0; x < dw; ++x) {
            int sx = static_cast<int>((x + 0.5f) / dw * sw);
            sx = std::clamp(sx, 0, sw - 1);
            out[static_cast<size_t>(y * dw + x)] = small[static_cast<size_t>(sy * sw + sx)];
        }
    }
    return out;
}

static std::vector<float> blurMask(const std::vector<float>& src, int w, int h, int kernel) {
    int radius = kernel / 2;
    std::vector<float> tmp(src.size());
    std::vector<float> out(src.size());

    auto blurH = [&](const std::vector<float>& in, std::vector<float>& dst) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float sum = 0; int count = 0;
                for (int k = -radius; k <= radius; ++k) {
                    int xx = x + k;
                    if (xx >= 0 && xx < w) { sum += in[y * w + xx]; ++count; }
                }
                dst[y * w + x] = sum / count;
            }
        }
    };
    auto blurV = [&](const std::vector<float>& in, std::vector<float>& dst) {
        for (int x = 0; x < w; ++x) {
            for (int y = 0; y < h; ++y) {
                float sum = 0; int count = 0;
                for (int k = -radius; k <= radius; ++k) {
                    int yy = y + k;
                    if (yy >= 0 && yy < h) { sum += in[yy * w + x]; ++count; }
                }
                dst[y * w + x] = sum / count;
            }
        }
    };
    blurH(src, tmp);
    blurV(tmp, out);
    return out;
}

static std::string toJsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    return out;
}

static std::vector<unsigned char> pngBytesFromFrame(const Frame& frame) {
    std::vector<uint8_t> rgb(frame.pixels.size());
    for (size_t i = 0; i < frame.pixels.size(); i += 3) {
        rgb[i + 0] = frame.pixels[i + 2];
        rgb[i + 1] = frame.pixels[i + 1];
        rgb[i + 2] = frame.pixels[i + 0];
    }
    std::vector<unsigned char> out;
    auto writer = [](void* context, void* data, int size) {
        auto* buffer = static_cast<std::vector<unsigned char>*>(context);
        auto* bytes = static_cast<unsigned char*>(data);
        buffer->insert(buffer->end(), bytes, bytes + size);
    };
    if (!stbi_write_png_to_func(writer, &out, frame.width, frame.height, 3, rgb.data(), frame.width * 3)) return {};
    return out;
}

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
};

static HttpResponse jsonResponse(const std::string& body, int status = 200) {
    return {status, "application/json", body};
}

static HttpResponse handleRequest(const std::string& method, const std::string& path, const std::string& body) {
    if (path == "/status" && method == "GET") {
        std::lock_guard<std::mutex> lock(shared.mutex);
        return jsonResponse("{\"mode\":\"" + toJsonEscape(shared.mode) + "\",\"bg_set\":true,\"threshold\":" + std::to_string(shared.threshold) + "}");
    }
    if (path == "/frame.png" && method == "GET") {
        std::lock_guard<std::mutex> lock(shared.mutex);
        if (!shared.latestReady || shared.latest.pixels.empty()) return {204, "text/plain", ""};
        std::vector<unsigned char> png = pngBytesFromFrame(shared.latest);
        return {200, "image/png", std::string(reinterpret_cast<const char*>(png.data()), png.size())};
    }
    if (path == "/mode" && method == "POST") {
        std::string mode;
        auto p = body.find("\"mode\"");
        if (p != std::string::npos) {
            p = body.find(':', p);
            p = body.find('"', p);
            size_t end = body.find('"', p + 1);
            if (end != std::string::npos) mode = body.substr(p + 1, end - p - 1);
        }
        if (mode == "1") mode = "normal"; else if (mode == "2") mode = "remove"; else if (mode == "3") mode = "virtualbg"; else if (mode == "4") mode = "blur";
        if (mode != "normal" && mode != "remove" && mode != "virtualbg" && mode != "blur") return jsonResponse("{\"error\":\"invalid mode\"}", 400);
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.mode = mode;
        return jsonResponse("{\"ok\":true,\"mode\":\"" + mode + "\"}");
    }
    if (path == "/bg" && method == "POST") {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.bgCapture = true;
        return jsonResponse("{\"ok\":true}");
    }
    if (path == "/resetbg" && method == "POST") {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.bgReset = true;
        return jsonResponse("{\"ok\":true}");
    }
    if (path == "/threshold" && method == "POST") {
        float value = 0.5f;
        auto p = body.find("\"value\"");
        if (p != std::string::npos) {
            p = body.find(':', p) + 1;
            try { value = std::stof(body.substr(p)); } catch (...) {}
        }
        value = std::clamp(value, 0.01f, 0.99f);
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.threshold = value;
        return jsonResponse("{\"ok\":true,\"value\":" + std::to_string(value) + "}");
    }
    return jsonResponse("{\"error\":\"not found\"}", 404);
}

static void httpServer() {
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    SOCKET_T server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET_T) return;
    int reuse = 1;
#ifdef _WIN32
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse), sizeof(reuse));
#else
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8765);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(server, 16) != 0) {
        close_socket(server);
        return;
    }
    while (running) {
        sockaddr_in client{};
        socklen_t len = sizeof(client);
        SOCKET_T s = accept(server, reinterpret_cast<sockaddr*>(&client), &len);
        if (s == INVALID_SOCKET_T) continue;
        char buffer[8192]{};
        int total = 0;
        while (total < static_cast<int>(sizeof(buffer) - 1)) {
            int n = recv(s, buffer + total, sizeof(buffer) - total - 1, 0);
            if (n <= 0) break;
            total += n;
            if (std::string(buffer, buffer + total).find("\r\n\r\n") != std::string::npos) break;
        }
        buffer[total] = '\0';
        std::string request(buffer, buffer + total);
        std::istringstream stream(request);
        std::string method, path, version;
        stream >> method >> path >> version;
        size_t bodyPos = request.find("\r\n\r\n");
        std::string body = bodyPos == std::string::npos ? "" : request.substr(bodyPos + 4);
        HttpResponse response = handleRequest(method, path, body);
        std::ostringstream out;
        out << "HTTP/1.1 " << response.status << " ";
        switch (response.status) { case 200: out << "OK"; break; case 204: out << "No Content"; break; case 400: out << "Bad Request"; break; default: out << "Not Found"; }
        out << "\r\nContent-Type: " << response.contentType << "\r\nContent-Length: " << response.body.size() << "\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
        std::string header = out.str();
        send(s, header.c_str(), static_cast<int>(header.size()), 0);
        if (!response.body.empty()) send(s, response.body.c_str(), static_cast<int>(response.body.size()), 0);
        close_socket(s);
    }
    close_socket(server);
#ifdef _WIN32
    WSACleanup();
#endif
}

static void flipHorizontal(Frame& frame) {
    size_t stride = static_cast<size_t>(frame.width * 3);
    for (int y = 0; y < frame.height; ++y) {
        uint8_t* row = frame.pixels.data() + static_cast<size_t>(y * stride);
        for (int x = 0; x < frame.width / 2; ++x) {
            std::swap_ranges(row + x * 3, row + x * 3 + 3, row + (frame.width - 1 - x) * 3);
        }
    }
}

int main(int argc, char** argv) {
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--src") cfg.source = next();
        else if (arg == "--dst") cfg.dst = next();
        else if (arg == "--width") cfg.width = std::stoi(next());
        else if (arg == "--height") cfg.height = std::stoi(next());
        else if (arg == "--fps") cfg.fps = std::stod(next());
        else if (arg == "--threshold") cfg.threshold = std::stof(next());
        else if (arg == "--smooth") cfg.smooth = std::stof(next());
        else if (arg == "--no-preview") cfg.noPreview = true;
        else if (arg == "--preview") cfg.noPreview = false;
        else if (arg == "--help" || arg == "-h") { printUsage(argv[0]); return 0; }
    }

    std::string model = "models/u2netp.onnx";
    if (const char* env = std::getenv("THANOS_MODEL"); env && *env) model = env;
    Segmenter segmenter;
    std::string error;
    if (!segmenter.load(model, error)) {
        std::cerr << error << "\nModel not found: " << model << "\n";
        return 2;
    }

    if (cfg.source.find_first_not_of("0123456789") == std::string::npos) cfg.source = "/dev/video" + cfg.source;
    auto input = createCameraInput();
    auto output = createCameraOutput();
    if (!input->open(cfg.source, cfg.width, cfg.height, cfg.fps)) {
        std::cerr << "Could not open camera source: " << cfg.source << "\n";
        return 3;
    }
    if (!output->open(cfg.dst, cfg.width, cfg.height, cfg.fps)) {
        std::cerr << "Could not open virtual camera: " << cfg.dst << " (Linux requires v4l2loopback /dev/video10)\n";
        return 4;
    }

    shared.threshold = cfg.threshold;
    shared.flip = cfg.flip;
    std::thread http(httpServer);

    Frame latest(cfg.width, cfg.height), background, outputFrame;
    std::vector<float> smoothMask;
    std::mt19937 random(42);
    std::vector<Particle> particles;
    std::vector<GatherParticle> gatherParticles;
    bool snapActive = false;
    bool gatherActive = false;
    double snapStart = 0.0;
    double gatherStart = 0.0;
    Frame snapBackground;
    Frame gatherBackground;
    std::string previousMode = "normal";
    auto now = []() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };

    while (running) {
        Frame frame(cfg.width, cfg.height);
        if (!input->read(frame)) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        if (cfg.flip) flipHorizontal(frame);

        std::string mode;
        bool captureBg, resetBg;
        float threshold;
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            mode = shared.mode;
            captureBg = shared.bgCapture;
            resetBg = shared.bgReset;
            threshold = shared.threshold;
            shared.bgCapture = false;
            shared.bgReset = false;
        }
        if (captureBg) background = frame;
        if (resetBg) background = Frame{};

        std::vector<float> mask = resizeMask(segmenter.predict(frame), 320, 320, cfg.width, cfg.height);
        if (smoothMask.empty()) smoothMask = mask;
        else {
            float a = std::clamp(cfg.smooth, 0.0f, 0.95f);
            for (size_t i = 0; i < mask.size(); ++i) smoothMask[i] = a * smoothMask[i] + (1.0f - a) * mask[i];
        }
        std::vector<float> soft = blurMask(smoothMask, cfg.width, cfg.height, 7);

        if (background.pixels.empty()) background = frame;

        // Trigger transitions before rendering.
        double currentTime = now();
        if (mode == "remove" && previousMode != "remove" && !snapActive) {
            snapActive = true;
            snapStart = currentTime;
            snapBackground = background;
            particles = makeParticles(smoothMask, cfg.width, cfg.height, frame, threshold, random);
        }
        if (previousMode == "remove" && mode == "normal" && !gatherActive) {
            gatherActive = true;
            gatherStart = currentTime;
            gatherBackground = background;
            gatherParticles = makeGatherParticles(smoothMask, cfg.width, cfg.height, frame, threshold, random);
        }

        outputFrame.width = cfg.width; outputFrame.height = cfg.height; outputFrame.pixels = frame.pixels;
        if (mode == "remove") {
            if (snapActive) {
                double progress = std::clamp((currentTime - snapStart) / 1.6, 0.0, 1.0);
                double eased = progress * progress;
                outputFrame = snapBackground;
                for (auto& pt : particles) {
                    pt.x += pt.vx * static_cast<float>(1.0 + eased * 2.0);
                    pt.y += pt.vy * static_cast<float>(1.0 + eased * 2.0);
                    pt.vy += 0.15f;
                    float alpha = static_cast<float>(1.0 - eased);
                    if (alpha <= 0.0f) continue;
                    int dust = 180;
                    uint8_t b = static_cast<uint8_t>(pt.b * alpha + dust * (1.0f - alpha));
                    uint8_t g = static_cast<uint8_t>(pt.g * alpha + dust * (1.0f - alpha));
                    uint8_t r = static_cast<uint8_t>(pt.r * alpha + dust * (1.0f - alpha));
                    int size = std::max(1, static_cast<int>(pt.size * (0.5f + 0.5f * alpha)));
                    drawCircle(outputFrame, static_cast<int>(pt.x), static_cast<int>(pt.y), size, b, g, r);
                }
                if (progress >= 1.0) snapActive = false;
            } else {
                outputFrame = background;
            }
        } else if (mode == "normal" && gatherActive) {
            double progress = std::clamp((currentTime - gatherStart) / 1.6, 0.0, 1.0);
            double eased = 1.0 - std::pow(1.0 - progress, 3.0);
            outputFrame = gatherBackground;
            int dust = 180;
            for (const auto& pt : gatherParticles) {
                float x = static_cast<float>(pt.sx + (pt.tx - pt.sx) * eased);
                float y = static_cast<float>(pt.sy + (pt.ty - pt.sy) * eased);
                float alpha = static_cast<float>(progress);
                uint8_t b = static_cast<uint8_t>(dust * (1.0f - alpha) + pt.b * alpha);
                uint8_t g = static_cast<uint8_t>(dust * (1.0f - alpha) + pt.g * alpha);
                uint8_t r = static_cast<uint8_t>(dust * (1.0f - alpha) + pt.r * alpha);
                int size = std::max(1, static_cast<int>(pt.size * (0.5f + 0.5f * alpha)));
                drawCircle(outputFrame, static_cast<int>(x), static_cast<int>(y), size, b, g, r);
            }
            if (progress > 0.3) blendFrames(outputFrame, frame, static_cast<float>(std::min(1.0, (progress - 0.3) / 0.7)));
            if (progress >= 1.0) gatherActive = false;
        } else if (mode == "virtualbg") {
            for (size_t i = 0; i < smoothMask.size(); ++i) {
                float person = smoothMask[i];
                int bg = static_cast<int>(i % cfg.width);
                (void)bg;
                int yy = static_cast<int>(i / cfg.width);
                uint8_t virtualR = static_cast<uint8_t>(30 + yy * 70 / cfg.height);
                uint8_t virtualG = static_cast<uint8_t>(45 + yy * 55 / cfg.height);
                uint8_t virtualB = static_cast<uint8_t>(90 + yy * 90 / cfg.height);
                size_t off = i * 3;
                outputFrame.pixels[off + 0] = static_cast<uint8_t>(frame.pixels[off + 0] * person + virtualB * (1.0f - person));
                outputFrame.pixels[off + 1] = static_cast<uint8_t>(frame.pixels[off + 1] * person + virtualG * (1.0f - person));
                outputFrame.pixels[off + 2] = static_cast<uint8_t>(frame.pixels[off + 2] * person + virtualR * (1.0f - person));
            }
        } else if (mode == "blur") {
            // Cheap box blur: 3 cumulative averaging passes are visually close enough.
            Frame blurred = frame;
            auto boxBlur = [&](Frame& f, int radius) {
                for (int y = 0; y < f.height; ++y) {
                    for (int x = 0; x < f.width; ++x) {
                        int sums[3] = {0, 0, 0}; int count = 0;
                        for (int dx = -radius; dx <= radius; dx += 6) {
                            int xx = x + dx;
                            if (xx >= 0 && xx < f.width) {
                                const uint8_t* p = f.ptr(xx, y);
                                sums[0] += p[0]; sums[1] += p[1]; sums[2] += p[2]; ++count;
                            }
                        }
                        uint8_t* p = f.ptr(x, y);
                        p[0] = sums[0] / count; p[1] = sums[1] / count; p[2] = sums[2] / count;
                    }
                }
            };
            boxBlur(blurred, 18);
            boxBlur(blurred, 18);
            for (size_t i = 0; i < smoothMask.size(); ++i) {
                float person = smoothMask[i];
                size_t off = i * 3;
                outputFrame.pixels[off + 0] = static_cast<uint8_t>(frame.pixels[off + 0] * person + blurred.pixels[off + 0] * (1.0f - person));
                outputFrame.pixels[off + 1] = static_cast<uint8_t>(frame.pixels[off + 1] * person + blurred.pixels[off + 1] * (1.0f - person));
                outputFrame.pixels[off + 2] = static_cast<uint8_t>(frame.pixels[off + 2] * person + blurred.pixels[off + 2] * (1.0f - person));
            }
        }

        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            shared.latest = outputFrame;
            shared.latestReady = true;
            ++shared.frameSerial;
        }

        output->write(outputFrame);
        latest = outputFrame;
        previousMode = mode;
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(1000.0 / cfg.fps)));
    }

    running = false;
    http.join();
    input->close();
    output->close();
#ifdef _WIN32
    CoUninitialize();
#endif
    return 0;
}

#ifdef STB_IMAGE_WRITE_IMPLEMENTATION
#endif
