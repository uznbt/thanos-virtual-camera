#pragma once
#include "Frame.hpp"
#include <string>
#include <memory>

class CameraInput {
public:
    virtual ~CameraInput() = default;
    virtual bool open(const std::string& source, int width, int height, double fps) = 0;
    virtual bool read(Frame& frame) = 0;
    virtual void close() = 0;
};

class CameraOutput {
public:
    virtual ~CameraOutput() = default;
    virtual bool open(const std::string& device, int width, int height, double fps) = 0;
    virtual bool write(const Frame& frame) = 0;
    virtual void close() = 0;
};

std::unique_ptr<CameraInput> createCameraInput();
std::unique_ptr<CameraOutput> createCameraOutput();
