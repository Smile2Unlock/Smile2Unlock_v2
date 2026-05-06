#include <app/app.h>
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

struct WindowState {
    bool needsRender = true;
    int renderedFrames = 0;
    double lastTitleUpdate = 0.0;
    double nextFrameTime = 0.0;
    double frameInterval = 1.0 / 60.0;
    double lastFrameRateLimit = 0.0;
    double lastRefreshRateUpdate = 0.0;
};

struct TimerResolutionGuard {
    TimerResolutionGuard() {
#ifdef _WIN32
        timeBeginPeriod(1);
#endif
    }

    ~TimerResolutionGuard() {
#ifdef _WIN32
        timeEndPeriod(1);
#endif
    }
};

float getDpiScale(GLFWwindow* window) {
    float scaleX = 1.0f, scaleY = 1.0f;
    glfwGetWindowContentScale(window, &scaleX, &scaleY);
    return (scaleX + scaleY) * 0.5f;
}

float getPointerScale(GLFWwindow* window) {
    int windowWidth = 0, windowHeight = 0;
    int framebufferWidth = 0, framebufferHeight = 0;
    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    if (windowWidth <= 0 || windowHeight <= 0) return 1.0f;
    const float sx = static_cast<float>(framebufferWidth) / static_cast<float>(windowWidth);
    const float sy = static_cast<float>(framebufferHeight) / static_cast<float>(windowHeight);
    return (sx + sy) * 0.5f;
}

GLFWmonitor* getWindowMonitor(GLFWwindow* window) {
    if (auto* m = glfwGetWindowMonitor(window)) return m;
    int wx = 0, wy = 0, ww = 0, wh = 0;
    glfwGetWindowPos(window, &wx, &wy);
    glfwGetWindowSize(window, &ww, &wh);
    int count = 0;
    auto** monitors = glfwGetMonitors(&count);
    auto* best = glfwGetPrimaryMonitor();
    int bestArea = 0;
    for (int i = 0; i < count; ++i) {
        auto* mon = monitors[i];
        int mx = 0, my = 0;
        glfwGetMonitorPos(mon, &mx, &my);
        auto* mode = glfwGetVideoMode(mon);
        if (!mode) continue;
        const int ox = std::max(wx, mx), oy = std::max(wy, my);
        const int ow = std::max(0, std::min(wx + ww, mx + mode->width) - ox);
        const int oh = std::max(0, std::min(wy + wh, my + mode->height) - oy);
        const int area = ow * oh;
        if (area > bestArea) { bestArea = area; best = mon; }
    }
    return best;
}

double getWindowRefreshRate(GLFWwindow* window) {
    auto* mon = getWindowMonitor(window);
    auto* mode = mon ? glfwGetVideoMode(mon) : nullptr;
    return (mode && mode->refreshRate > 0) ? static_cast<double>(mode->refreshRate) : 60.0;
}

void updateFrameInterval(GLFWwindow* window, WindowState& ws, double now) {
    const double limit = app::frameRateLimit();
    if (limit == ws.lastFrameRateLimit && now - ws.lastRefreshRateUpdate < 0.5) return;
    double rate = std::clamp(getWindowRefreshRate(window), 30.0, 500.0);
    if (limit > 0.0) rate = std::min(rate, limit);
    ws.frameInterval = 1.0 / std::max(1.0, rate);
    ws.lastFrameRateLimit = limit;
    ws.lastRefreshRateUpdate = now;
}

void waitForNextFrame(GLFWwindow* window, const WindowState& ws) {
    while (!glfwWindowShouldClose(window)) {
        const double rem = ws.nextFrameTime - glfwGetTime();
        if (rem <= 0.0) break;
        if (rem > 0.002) glfwWaitEventsTimeout(rem - 0.001);
        else std::this_thread::sleep_for(std::chrono::duration<double>(rem * 0.5));
    }
}

int main() {
    glfwInit();
    TimerResolutionGuard trg;

    glfwWindowHint(GLFW_SAMPLES, 0);
    glfwWindowHint(GLFW_RED_BITS, 8);
    glfwWindowHint(GLFW_GREEN_BITS, 8);
    glfwWindowHint(GLFW_BLUE_BITS, 8);
    glfwWindowHint(GLFW_ALPHA_BITS, 8);
    glfwWindowHint(GLFW_DEPTH_BITS, 16);
    glfwWindowHint(GLFW_STENCIL_BITS, 0);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    auto* window = glfwCreateWindow(
        app::initialWindowWidth(), app::initialWindowHeight(),
        app::windowTitle(), nullptr, nullptr);
    if (!window) { glfwTerminate(); return -1; }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);

    WindowState ws;
    ws.lastTitleUpdate = glfwGetTime();
    ws.nextFrameTime = ws.lastTitleUpdate;
    updateFrameInterval(window, ws, ws.lastTitleUpdate);
    if (app::showFrameCountInTitle()) {
        char title[128];
        std::snprintf(title, sizeof(title), "%s - 0 FPS", app::windowTitle());
        glfwSetWindowTitle(window, title);
    }
    glfwSetWindowUserPointer(window, &ws);
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow* w, int, int) {
        static_cast<WindowState*>(glfwGetWindowUserPointer(w))->needsRender = true;
    });
    glfwSetWindowRefreshCallback(window, [](GLFWwindow* w) {
        static_cast<WindowState*>(glfwGetWindowUserPointer(w))->needsRender = true;
    });

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        glfwTerminate();
        return -1;
    }
    if (!app::initialize(window)) {
        glfwTerminate();
        return -1;
    }

    double lastFrameTime = glfwGetTime();

    while (!glfwWindowShouldClose(window)) {
        if (app::isAnimating()) waitForNextFrame(window, ws);

        const double now = glfwGetTime();
        updateFrameInterval(window, ws, now);
        const float dt = static_cast<float>(now - lastFrameTime);
        lastFrameTime = now;

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, 1);
            break;
        }

        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        if (fbw <= 0 || fbh <= 0) {
            ws.needsRender = true;
            glfwWaitEvents();
            ws.nextFrameTime = glfwGetTime();
            lastFrameTime = ws.nextFrameTime;
            continue;
        }

        if (app::update(window, dt, fbw, fbh, getDpiScale(window), getPointerScale(window))) {
            ws.needsRender = true;
        }

        if (ws.needsRender) {
            app::render(fbw, fbh, getDpiScale(window));
            glfwSwapBuffers(window);
            ws.needsRender = false;
            ++ws.renderedFrames;
        }

        if (app::showFrameCountInTitle()) {
            const double elapsed = glfwGetTime() - ws.lastTitleUpdate;
            if (elapsed >= 1.0) {
                char title[128];
                std::snprintf(title, sizeof(title), "%s - %.0f FPS",
                    app::windowTitle(), static_cast<double>(ws.renderedFrames) / elapsed);
                glfwSetWindowTitle(window, title);
                ws.renderedFrames = 0;
                ws.lastTitleUpdate = glfwGetTime();
            }
        }

        if (app::isAnimating()) {
            ws.nextFrameTime += ws.frameInterval;
            if (ws.nextFrameTime <= now || ws.nextFrameTime > now + ws.frameInterval * 2.0)
                ws.nextFrameTime = now + ws.frameInterval;
            glfwPollEvents();
        } else {
            glfwWaitEvents();
            ws.nextFrameTime = glfwGetTime();
        }
    }

    app::shutdown();
    glfwTerminate();
    return 0;
}
