#pragma once

#include <cstdint>
#include <string>

struct EglSession {
    void* display = nullptr;
    void* context = nullptr;
    void* surface = nullptr;
    bool surfaceless = false;
};

bool eglMakeHeadlessCurrent (EglSession& session, std::string& error);
void eglTeardown (EglSession& session);

bool createTestFramebuffer (uint32_t width, uint32_t height, uint32_t& framebuffer, uint32_t& texture, std::string& error);
