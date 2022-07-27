// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cmath>
#include <mutex>
#include "core/3ds.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/input.h"
#include "core/settings.h"
#include "input_common/main.h"
#include "video_core/renderer_base.h"
#include "video_core/video_core.h"

namespace Frontend {

GraphicsContext::~GraphicsContext() = default;

class EmuWindow::TouchState : public Input::Factory<Input::TouchDevice>,
                              public std::enable_shared_from_this<TouchState> {
public:
    std::unique_ptr<Input::TouchDevice> Create(const Common::ParamPackage&) override {
        return std::make_unique<Device>(shared_from_this());
    }

    std::mutex mutex;

    bool touch_pressed = false; ///< True if touchpad area is currently pressed, otherwise false

    float touch_x = 0.0f; ///< Touchpad X-position
    float touch_y = 0.0f; ///< Touchpad Y-position

private:
    class Device : public Input::TouchDevice {
    public:
        explicit Device(std::weak_ptr<TouchState>&& touch_state) : touch_state(touch_state) {}
        std::tuple<float, float, bool> GetStatus() const override {
            if (auto state = touch_state.lock()) {
                std::lock_guard guard{state->mutex};
                return std::make_tuple(state->touch_x, state->touch_y, state->touch_pressed);
            }
            return std::make_tuple(0.0f, 0.0f, false);
        }

    private:
        std::weak_ptr<TouchState> touch_state;
    };
};

EmuWindow::EmuWindow() {
    // TODO: Find a better place to set this.
    config.min_client_area_size =
        std::make_pair(Core::kScreenTopWidth, Core::kScreenTopHeight + Core::kScreenBottomHeight);
    active_config = config;
    touch_state = std::make_shared<TouchState>();
    Input::RegisterFactory<Input::TouchDevice>("emu_window", touch_state);
}

EmuWindow::~EmuWindow() {
    Input::UnregisterFactory<Input::TouchDevice>("emu_window");
}

static bool IsWithinTopScreen(const Layout::FramebufferLayout& layout, float* norm_x, float* norm_y,
                              unsigned fbx, unsigned fby) {
    if (fbx >= layout.top_screen.left && fbx < layout.top_screen.right &&
        fby >= layout.top_screen.top && fby < layout.top_screen.bottom) {
        *norm_x = fbx - layout.top_screen.left;
        *norm_y = fby - layout.top_screen.top;
        return true;
    } else {
        return false;
    }
}

/**
 * Check if the given x/y coordinates are within the touchpad specified by the framebuffer layout
 * @param layout FramebufferLayout object describing the framebuffer size and screen positions
 * @param framebuffer_x Framebuffer x-coordinate to check
 * @param framebuffer_y Framebuffer y-coordinate to check
 * @return True if the coordinates are within the touchpad, otherwise false
 */
static bool IsWithinTouchscreen(const Layout::FramebufferLayout& layout, unsigned framebuffer_x,
                                unsigned framebuffer_y) {
    if (Settings::values.render_3d == Settings::StereoRenderOption::SideBySide) {
        return (framebuffer_y >= layout.bottom_screen.top &&
                framebuffer_y < layout.bottom_screen.bottom &&
                ((framebuffer_x >= layout.bottom_screen.left / 2 &&
                  framebuffer_x < layout.bottom_screen.right / 2) ||
                 (framebuffer_x >= (layout.bottom_screen.left / 2) + (layout.width / 2) &&
                  framebuffer_x < (layout.bottom_screen.right / 2) + (layout.width / 2))));
    } else if (Settings::values.render_3d == Settings::StereoRenderOption::CardboardVR) {
        return (framebuffer_y >= layout.bottom_screen.top &&
                framebuffer_y < layout.bottom_screen.bottom &&
                ((framebuffer_x >= layout.bottom_screen.left &&
                  framebuffer_x < layout.bottom_screen.right) ||
                 (framebuffer_x >= layout.cardboard.bottom_screen_right_eye + (layout.width / 2) &&
                  framebuffer_x < layout.cardboard.bottom_screen_right_eye +
                                      layout.bottom_screen.GetWidth() + (layout.width / 2))));
    } else {
        return (framebuffer_y >= layout.bottom_screen.top &&
                framebuffer_y < layout.bottom_screen.bottom &&
                framebuffer_x >= layout.bottom_screen.left &&
                framebuffer_x < layout.bottom_screen.right);
    }
}

std::tuple<unsigned, unsigned> EmuWindow::ClipToTouchScreen(unsigned new_x, unsigned new_y) const {
    if (new_x >= framebuffer_layout.width / 2) {
        if (Settings::values.render_3d == Settings::StereoRenderOption::SideBySide)
            new_x -= framebuffer_layout.width / 2;
        else if (Settings::values.render_3d == Settings::StereoRenderOption::CardboardVR)
            new_x -=
                (framebuffer_layout.width / 2) - (framebuffer_layout.cardboard.user_x_shift * 2);
    }
    if (Settings::values.render_3d == Settings::StereoRenderOption::SideBySide) {
        new_x = std::max(new_x, framebuffer_layout.bottom_screen.left / 2);
        new_x = std::min(new_x, framebuffer_layout.bottom_screen.right / 2 - 1);
    } else {
        new_x = std::max(new_x, framebuffer_layout.bottom_screen.left);
        new_x = std::min(new_x, framebuffer_layout.bottom_screen.right - 1);
    }

    new_y = std::max(new_y, framebuffer_layout.bottom_screen.top);
    new_y = std::min(new_y, framebuffer_layout.bottom_screen.bottom - 1);

    return std::make_tuple(new_x, new_y);
}

bool EmuWindow::TouchPressed(unsigned framebuffer_x, unsigned framebuffer_y) {

    if (IsWithinTopScreen(framebuffer_layout, &camera_hack_state.tx, &camera_hack_state.ty,
                          framebuffer_x, framebuffer_y)) {
        camera_hack_state.lastX = camera_hack_state.tx;
        camera_hack_state.lastY = camera_hack_state.ty;
        camera_hack_state.grabbed = true;
        return;
    }
    if (!IsWithinTouchscreen(framebuffer_layout, framebuffer_x, framebuffer_y))
        return false;

    if (framebuffer_x >= framebuffer_layout.width / 2) {
        if (Settings::values.render_3d == Settings::StereoRenderOption::SideBySide)
            framebuffer_x -= framebuffer_layout.width / 2;
        else if (Settings::values.render_3d == Settings::StereoRenderOption::CardboardVR)
            framebuffer_x -=
                (framebuffer_layout.width / 2) - (framebuffer_layout.cardboard.user_x_shift * 2);
    }
    std::lock_guard guard(touch_state->mutex);
    if (Settings::values.render_3d == Settings::StereoRenderOption::SideBySide) {
        touch_state->touch_x =
            static_cast<float>(framebuffer_x - framebuffer_layout.bottom_screen.left / 2) /
            (framebuffer_layout.bottom_screen.right / 2 -
             framebuffer_layout.bottom_screen.left / 2);
    } else {
        touch_state->touch_x =
            static_cast<float>(framebuffer_x - framebuffer_layout.bottom_screen.left) /
            (framebuffer_layout.bottom_screen.right - framebuffer_layout.bottom_screen.left);
    }
    touch_state->touch_y =
        static_cast<float>(framebuffer_y - framebuffer_layout.bottom_screen.top) /
        (framebuffer_layout.bottom_screen.bottom - framebuffer_layout.bottom_screen.top);

    if (!framebuffer_layout.is_rotated) {
        std::swap(touch_state->touch_x, touch_state->touch_y);
        touch_state->touch_x = 1.f - touch_state->touch_x;
    }

    touch_state->touch_pressed = true;
    return true;
}

void EmuWindow::TouchReleased() {

    if (camera_hack_state.grabbed) {
        camera_hack_state.grabbed = false;
        return;
    }

    std::lock_guard guard{touch_state->mutex};

    touch_state->touch_pressed = false;
    touch_state->touch_x = 0;
    touch_state->touch_y = 0;
}

static double clamp(double v, double _min, double _max) {
    return std::min(std::max(v, _min), _max);
}

static double clampRange(double v, double r) {
    return clamp(v, -r, r);
}

static bool isZero(double* v) {
    return v[0] == 0.0 && v[1] == 0.0 && v[2] == 0.0;
}

void EmuWindow::UpdateCameraHack() {
    static std::array<std::unique_ptr<Input::ButtonDevice>, 8> buttons;
    if (buttons[0] == nullptr) {
        buttons[0] =
            Input::CreateDevice<Input::ButtonDevice>(InputCommon::GenerateKeyboardParam(0x57)); // W
        buttons[1] =
            Input::CreateDevice<Input::ButtonDevice>(InputCommon::GenerateKeyboardParam(0x41)); // A
        buttons[2] =
            Input::CreateDevice<Input::ButtonDevice>(InputCommon::GenerateKeyboardParam(0x53)); // S
        buttons[3] =
            Input::CreateDevice<Input::ButtonDevice>(InputCommon::GenerateKeyboardParam(0x44)); // D
        buttons[4] = Input::CreateDevice<Input::ButtonDevice>(
            InputCommon::GenerateKeyboardParam(0x01000020)); // Shift
        buttons[5] =
            Input::CreateDevice<Input::ButtonDevice>(InputCommon::GenerateKeyboardParam(0x42)); // B
        buttons[6] =
            Input::CreateDevice<Input::ButtonDevice>(InputCommon::GenerateKeyboardParam(0x51)); // Q
        buttons[7] =
            Input::CreateDevice<Input::ButtonDevice>(InputCommon::GenerateKeyboardParam(0x45)); // E
    }

    const bool keyW = buttons[0]->GetStatus();
    const bool keyA = buttons[1]->GetStatus();
    const bool keyS = buttons[2]->GetStatus();
    const bool keyD = buttons[3]->GetStatus();
    const bool isShiftPressed = buttons[4]->GetStatus();
    const bool keyB = buttons[5]->GetStatus();
    const bool keyQ = buttons[6]->GetStatus();
    const bool keyE = buttons[7]->GetStatus();

    float mouseDelta[2] = {};
    mouseDelta[0] = (camera_hack_state.tx - camera_hack_state.lastX);
    mouseDelta[1] = (camera_hack_state.ty - camera_hack_state.lastY);
    camera_hack_state.lastX = camera_hack_state.tx;
    camera_hack_state.lastY = camera_hack_state.ty;

    const auto keyMoveSpeed = 10.0;
    const auto keyMoveShiftMult = 5.0;
    const auto keyMoveVelocityMult = 1.0 / 5.0;
    const auto keyMoveDrag = 0.8;
    const auto keyMoveLowSpeedCap = 0.01;

    auto keyMoveMult = 1.0;
    if (isShiftPressed)
        keyMoveMult = keyMoveShiftMult;

    const auto keyMoveSpeedCap = keyMoveSpeed * keyMoveMult;
    const auto keyMoveVelocity = keyMoveSpeedCap * keyMoveVelocityMult;

    auto& keyMovement = fps_camera_controller.keyMovement;
    if (keyW) {
        keyMovement[2] = clampRange(keyMovement[2] - keyMoveVelocity, keyMoveSpeedCap);
    } else if (keyS) {
        keyMovement[2] = clampRange(keyMovement[2] + keyMoveVelocity, keyMoveSpeedCap);
    } else {
        keyMovement[2] *= keyMoveDrag;
        if (std::abs(keyMovement[2]) < keyMoveLowSpeedCap)
            keyMovement[2] = 0.0;
    }

    if (keyA) {
        keyMovement[0] = clampRange(keyMovement[0] - keyMoveVelocity, keyMoveSpeedCap);
    } else if (keyD) {
        keyMovement[0] = clampRange(keyMovement[0] + keyMoveVelocity, keyMoveSpeedCap);
    } else {
        keyMovement[0] *= keyMoveDrag;
        if (std::abs(keyMovement[0]) < keyMoveLowSpeedCap)
            keyMovement[0] = 0.0;
    }

    if (keyQ) {
        keyMovement[1] = clampRange(keyMovement[1] - keyMoveVelocity, keyMoveSpeedCap);
    } else if (keyE) {
        keyMovement[1] = clampRange(keyMovement[1] + keyMoveVelocity, keyMoveSpeedCap);
    } else {
        keyMovement[1] *= keyMoveDrag;
        if (std::abs(keyMovement[1]) < keyMoveLowSpeedCap)
            keyMovement[1] = 0.0;
    }

    auto& worldMatrix = fps_camera_controller.worldMatrix;

    if (keyB) {
        Common::mat4_identity(worldMatrix);
    }

    Common::Vec3 cameraUp{0.0, 0.0, 0.0};
    cameraUp.x = worldMatrix[1];
    cameraUp.y = worldMatrix[5];
    cameraUp.z = worldMatrix[9];

    if (!isZero(keyMovement)) {
        float finalMovement[3] = {};
        finalMovement[0] = keyMovement[0];
        finalMovement[2] = keyMovement[2];

        // Instead of getting the camera up, instead use world up. Feels more natural.
        finalMovement[0] += cameraUp[0] * keyMovement[1];
        finalMovement[1] += cameraUp[1] * keyMovement[1];
        finalMovement[2] += cameraUp[2] * keyMovement[1];

        Common::mat4_translate(worldMatrix, worldMatrix, keyMovement);
    }

    const auto mouseMoveLowSpeedCap = 0.0001;

    auto& mouseMovement = fps_camera_controller.mouseMovement;
    mouseMovement[0] += mouseDelta[0] / -500.0;
    mouseMovement[1] += mouseDelta[1] / -500.0;

    if (!isZero(mouseMovement)) {
        auto tmp = cameraUp;
        tmp.Normalize();
        Common::mat4_rotate(worldMatrix, worldMatrix, mouseMovement[0], tmp.AsArray());

        double worldUp[] = {1.0, 0.0, 0.0};
        Common::mat4_rotate(worldMatrix, worldMatrix, mouseMovement[1], worldUp);
    }

    const double mouseLookDrag = 0.0;
    mouseMovement[0] *= mouseLookDrag;
    mouseMovement[1] *= mouseLookDrag;

    if (std::abs(mouseMovement[0]) < mouseMoveLowSpeedCap) mouseMovement[0] = 0.0;
    if (std::abs(mouseMovement[1]) < mouseMoveLowSpeedCap) mouseMovement[1] = 0.0;

    VideoCore::RenderHacksInput input;
    Common::mat4_inverse(input.view_matrix, worldMatrix);
    input.disable_fog = fog_disabled;

    VideoCore::g_renderer->SetRenderHacks(input);
}

void EmuWindow::TouchMoved(unsigned framebuffer_x, unsigned framebuffer_y) {
    if (camera_hack_state.grabbed) {
        IsWithinTopScreen(framebuffer_layout, &camera_hack_state.tx, &camera_hack_state.ty,
                          framebuffer_x, framebuffer_y);
    }

    if (!touch_state->touch_pressed)
        return;

    if (!IsWithinTouchscreen(framebuffer_layout, framebuffer_x, framebuffer_y))
        std::tie(framebuffer_x, framebuffer_y) = ClipToTouchScreen(framebuffer_x, framebuffer_y);

    TouchPressed(framebuffer_x, framebuffer_y);
}

void EmuWindow::UpdateCurrentFramebufferLayout(unsigned width, unsigned height,
                                               bool is_portrait_mode) {
    Layout::FramebufferLayout layout;
    const auto layout_option = Settings::values.layout_option;
    const auto min_size =
        Layout::GetMinimumSizeFromLayout(layout_option, Settings::values.upright_screen);

    if (Settings::values.custom_layout == true) {
        layout = Layout::CustomFrameLayout(width, height);
    } else {
        width = std::max(width, min_size.first);
        height = std::max(height, min_size.second);

        // If in portrait mode, only the MobilePortrait option really makes sense
        const Settings::LayoutOption layout_option = is_portrait_mode
                                                         ? Settings::LayoutOption::MobilePortrait
                                                         : Settings::values.layout_option;

        switch (layout_option) {
        case Settings::LayoutOption::SingleScreen:
            layout = Layout::SingleFrameLayout(width, height, Settings::values.swap_screen,
                                               Settings::values.upright_screen);
            break;
        case Settings::LayoutOption::LargeScreen:
            layout = Layout::LargeFrameLayout(width, height, Settings::values.swap_screen,
                                              Settings::values.upright_screen);
            break;
        case Settings::LayoutOption::SideScreen:
            layout = Layout::SideFrameLayout(width, height, Settings::values.swap_screen,
                                             Settings::values.upright_screen);
            break;
        case Settings::LayoutOption::MobilePortrait:
            layout = Layout::MobilePortraitFrameLayout(width, height, Settings::values.swap_screen);
            break;
        case Settings::LayoutOption::MobileLandscape:
            layout = Layout::MobileLandscapeFrameLayout(width, height, Settings::values.swap_screen,
                                                        2.25f, false);
            break;
        case Settings::LayoutOption::Default:
        default:
            layout = Layout::DefaultFrameLayout(width, height, Settings::values.swap_screen,
                                                Settings::values.upright_screen);
            break;
        }
        UpdateMinimumWindowSize(min_size);
    }
    if (Settings::values.render_3d == Settings::StereoRenderOption::CardboardVR) {
        layout = Layout::GetCardboardSettings(layout);
    }
    NotifyFramebufferLayoutChanged(layout);
}

void EmuWindow::UpdateMinimumWindowSize(std::pair<unsigned, unsigned> min_size) {
    WindowConfig new_config = config;
    new_config.min_client_area_size = min_size;
    SetConfig(new_config);
    ProcessConfigurationChanges();
}

} // namespace Frontend
