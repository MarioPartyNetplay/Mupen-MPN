/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "OnScreenDisplay.hpp"
#include "VidExt.hpp"

#include <RMG-Core/Settings.hpp>
#include <RMG-Core/TurnCount.hpp>

#include <backends/imgui_impl_opengl3.h>
#include <imgui.h>
#include <chrono>
#include <mutex>

namespace
{

enum class OsdElement : int
{
    Message = 0,
    Overlay,
    TurnCount,
    Count
};

struct OsdElementRect
{
    float x      = 0.0f;
    float y      = 0.0f;
    float width  = 0.0f;
    float height = 0.0f;
    bool  valid  = false;
};

constexpr int PERMILLE_MAX = 1000;
// Matches the default main-window size so 100% OSD scale stays unchanged there.
constexpr float OSD_REFERENCE_WIDTH  = 960.0f;
constexpr float OSD_REFERENCE_HEIGHT = 720.0f;

static bool l_Initialized     = false;
static bool l_Enabled         = false;
static bool l_RenderingPaused = false;

static std::chrono::time_point<std::chrono::high_resolution_clock> l_MessageTime;
static std::string l_Message;
static std::string l_OverlayText;
static int         l_MessagePosition = 1;
static float       l_MessagePaddingX = 20.0f;
static float       l_MessagePaddingY = 20.0f;
static float       l_BackgroundRed   = 1.0f;
static float       l_BackgroundGreen = 1.0f;
static float       l_BackgroundBlue  = 1.0f;
static float       l_BackgroundAlpha = 1.0f;
static float       l_TextRed         = 1.0f;
static float       l_TextGreen       = 1.0f;
static float       l_TextBlue        = 1.0f;
static float       l_TextAlpha       = 1.0f;
static int         l_MessageDuration = 3;
static float       l_Scale           = 1.0f;
static bool        l_CustomLayout    = false;
static int         l_CustomPosPermille[static_cast<int>(OsdElement::Count)][2] = {};

static std::mutex       l_Mutex;
static bool             l_ConfigureModifier = false;
static int              l_DraggingElement   = -1;
static float            l_DragOffsetX         = 0.0f;
static float            l_DragOffsetY         = 0.0f;
static OsdElementRect   l_ElementRects[static_cast<int>(OsdElement::Count)] = {};
static int              l_DisplayWidth        = 0;
static int              l_DisplayHeight       = 0;

static void load_custom_positions(void)
{
    const std::vector<std::pair<SettingsID, OsdElement>> settings = {
        {SettingsID::GUI_OnScreenDisplayMessagePos, OsdElement::Message},
        {SettingsID::GUI_OnScreenDisplayOverlayPos, OsdElement::Overlay},
        {SettingsID::GUI_OnScreenDisplayTurnCountPos, OsdElement::TurnCount},
    };

    for (const auto& entry : settings)
    {
        const int index = static_cast<int>(entry.second);
        const std::vector<int> pos = CoreSettingsGetIntListValue(entry.first);
        if (pos.size() == 2)
        {
            l_CustomPosPermille[index][0] = pos.at(0);
            l_CustomPosPermille[index][1] = pos.at(1);
        }
    }
}

static void save_custom_positions(void)
{
    CoreSettingsSetValue(SettingsID::GUI_OnScreenDisplayCustomLayout, true);
    CoreSettingsSetValue(SettingsID::GUI_OnScreenDisplayMessagePos,
                         std::vector<int>({l_CustomPosPermille[static_cast<int>(OsdElement::Message)][0],
                                           l_CustomPosPermille[static_cast<int>(OsdElement::Message)][1]}));
    CoreSettingsSetValue(SettingsID::GUI_OnScreenDisplayOverlayPos,
                         std::vector<int>({l_CustomPosPermille[static_cast<int>(OsdElement::Overlay)][0],
                                           l_CustomPosPermille[static_cast<int>(OsdElement::Overlay)][1]}));
    CoreSettingsSetValue(SettingsID::GUI_OnScreenDisplayTurnCountPos,
                         std::vector<int>({l_CustomPosPermille[static_cast<int>(OsdElement::TurnCount)][0],
                                           l_CustomPosPermille[static_cast<int>(OsdElement::TurnCount)][1]}));
    CoreSettingsSave();
}

static float osd_window_scale(const ImVec2& displaySize)
{
    if (displaySize.x <= 0.0f || displaySize.y <= 0.0f)
    {
        return 1.0f;
    }

    const float scaleX = displaySize.x / OSD_REFERENCE_WIDTH;
    const float scaleY = displaySize.y / OSD_REFERENCE_HEIGHT;
    float scale = (scaleX < scaleY) ? scaleX : scaleY;

    if (scale < 0.35f)
    {
        scale = 0.35f;
    }
    else if (scale > 4.0f)
    {
        scale = 4.0f;
    }

    return scale;
}

static float osd_effective_scale(const ImVec2& displaySize)
{
    return l_Scale * osd_window_scale(displaySize);
}

static ImVec2 permille_to_pos(int xPermille, int yPermille, const ImVec2& displaySize)
{
    return ImVec2(displaySize.x * static_cast<float>(xPermille) / static_cast<float>(PERMILLE_MAX),
                  displaySize.y * static_cast<float>(yPermille) / static_cast<float>(PERMILLE_MAX));
}

static void pos_to_permille(float x, float y, const ImVec2& displaySize, int& xPermille, int& yPermille)
{
    if (displaySize.x <= 0.0f || displaySize.y <= 0.0f)
    {
        xPermille = 0;
        yPermille = 0;
        return;
    }

    xPermille = static_cast<int>((x / displaySize.x) * static_cast<float>(PERMILLE_MAX));
    yPermille = static_cast<int>((y / displaySize.y) * static_cast<float>(PERMILLE_MAX));

    if (xPermille < 0)
    {
        xPermille = 0;
    }
    else if (xPermille > PERMILLE_MAX)
    {
        xPermille = PERMILLE_MAX;
    }

    if (yPermille < 0)
    {
        yPermille = 0;
    }
    else if (yPermille > PERMILLE_MAX)
    {
        yPermille = PERMILLE_MAX;
    }
}

static void clamp_pos_to_display(float& x, float& y, float width, float height, const ImVec2& displaySize)
{
    const float maxX = displaySize.x - width;
    const float maxY = displaySize.y - height;

    if (x < 0.0f)
    {
        x = 0.0f;
    }
    else if (x > maxX)
    {
        x = maxX;
    }

    if (y < 0.0f)
    {
        y = 0.0f;
    }
    else if (y > maxY)
    {
        y = maxY;
    }
}

static ImVec2 legacy_position(OsdElement element, const ImVec2& displaySize, ImVec2& pivot)
{
    const float windowScale = osd_window_scale(displaySize);
    const float padX = l_MessagePaddingX * windowScale;
    const float padY = l_MessagePaddingY * windowScale;

    switch (element)
    {
    case OsdElement::Message:
        switch (l_MessagePosition)
        {
        default:
        case 0: // left bottom
            pivot = ImVec2(0.0f, 1.0f);
            return ImVec2(padX, displaySize.y - padY);
        case 1: // left top
            pivot = ImVec2(0.0f, 0.0f);
            return ImVec2(padX, padY);
        case 2: // right top
            pivot = ImVec2(1.0f, 0.0f);
            return ImVec2(displaySize.x - padX, padY);
        case 3: // right bottom
            pivot = ImVec2(1.0f, 1.0f);
            return ImVec2(displaySize.x - padX, displaySize.y - padY);
        }
    case OsdElement::Overlay:
        pivot = ImVec2(1.0f, 0.0f);
        return ImVec2(displaySize.x - padX, padY);
    case OsdElement::TurnCount:
        pivot = ImVec2(0.0f, 0.0f);
        return ImVec2(padX, padY);
    default:
        pivot = ImVec2(0.0f, 0.0f);
        return ImVec2(padX, padY);
    }
}

static bool point_in_rect(float x, float y, const OsdElementRect& rect)
{
    return rect.valid &&
           x >= rect.x && x <= (rect.x + rect.width) &&
           y >= rect.y && y <= (rect.y + rect.height);
}

static void draw_configure_highlight(const OsdElementRect& rect, float thickness)
{
    if (!rect.valid)
    {
        return;
    }

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    drawList->AddRect(ImVec2(rect.x, rect.y),
                      ImVec2(rect.x + rect.width, rect.y + rect.height),
                      IM_COL32(255, 220, 64, 220),
                      0.0f,
                      0,
                      thickness);
}

static void render_osd_window(OsdElement element,
                              const char* windowId,
                              const char* text,
                              const ImVec2& displaySize,
                              bool configureModifier,
                              int draggingElement,
                              bool customLayout,
                              const int customPosPermille[static_cast<int>(OsdElement::Count)][2],
                              OsdElementRect* outRect)
{
    const int elementIndex = static_cast<int>(element);
    ImVec2 pivot(0.0f, 0.0f);
    ImVec2 pos;

    if (customLayout || draggingElement == elementIndex)
    {
        pos = permille_to_pos(customPosPermille[elementIndex][0],
                              customPosPermille[elementIndex][1],
                              displaySize);
    }
    else
    {
        pos = legacy_position(element, displaySize, pivot);
    }

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);

    const float scale = osd_effective_scale(displaySize);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(l_BackgroundRed, l_BackgroundGreen, l_BackgroundBlue, l_BackgroundAlpha));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(l_TextRed, l_TextGreen, l_TextBlue, l_TextAlpha));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, 8.0f * scale));

    ImGui::Begin(windowId, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(scale);
    ImGui::Text("%s", text);
    const ImVec2 windowPos = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();
    ImGui::End();

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    outRect->x      = windowPos.x;
    outRect->y      = windowPos.y;
    outRect->width  = windowSize.x;
    outRect->height = windowSize.y;
    outRect->valid  = true;

    if (configureModifier)
    {
        draw_configure_highlight(*outRect, 2.0f * osd_window_scale(displaySize));
    }
}

} // namespace

//
// Exported Functions
//

bool OnScreenDisplayInit(void)
{
    if (l_Initialized)
    {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplOpenGL3_SetProcLoader(VidExt_GetProcAddress);

    if (!ImGui_ImplOpenGL3_Init(
#ifdef __APPLE__
            "#version 300 es"
#else
            nullptr
#endif
        ))
    {
        return false;
    }

    l_Initialized = true;
    return true;
}

void OnScreenDisplayShutdown(void)
{
    if (!l_Initialized)
    {
        return;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();

    l_Message         = "";
    l_OverlayText     = "";
    l_Initialized     = false;
    l_RenderingPaused = false;
    l_DraggingElement = -1;

    std::lock_guard<std::mutex> lock(l_Mutex);
    for (auto& rect : l_ElementRects)
    {
        rect = {};
    }
}

void OnScreenDisplayLoadSettings(void)
{
    l_Enabled         = CoreSettingsGetBoolValue(SettingsID::GUI_OnScreenDisplayEnabled);
    l_MessagePosition = CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayLocation);
    l_MessagePaddingX = static_cast<float>(CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayPaddingX));
    l_MessagePaddingY = static_cast<float>(CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayPaddingY));
    l_MessageDuration = CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayDuration);
    l_CustomLayout    = CoreSettingsGetBoolValue(SettingsID::GUI_OnScreenDisplayCustomLayout);

    int scalePercent = CoreSettingsGetIntValue(SettingsID::GUI_OnScreenDisplayScale);
    if (scalePercent < 25)
    {
        scalePercent = 25;
    }
    else if (scalePercent > 400)
    {
        scalePercent = 400;
    }
    l_Scale = static_cast<float>(scalePercent) / 100.0f;

    load_custom_positions();

    std::vector<int> backgroundColor = CoreSettingsGetIntListValue(SettingsID::GUI_OnScreenDisplayBackgroundColor);
    std::vector<int> textColor       = CoreSettingsGetIntListValue(SettingsID::GUI_OnScreenDisplayTextColor);
    if (backgroundColor.size() == 4)
    {
        l_BackgroundRed   = backgroundColor.at(0) / 255.0f;
        l_BackgroundGreen = backgroundColor.at(1) / 255.0f;
        l_BackgroundBlue  = backgroundColor.at(2) / 255.0f;
        l_BackgroundAlpha = backgroundColor.at(3) / 255.0f;
    }
    if (textColor.size() == 4)
    {
        l_TextRed   = textColor.at(0) / 255.0f;
        l_TextGreen = textColor.at(1) / 255.0f;
        l_TextBlue  = textColor.at(2) / 255.0f;
        l_TextAlpha = textColor.at(3) / 255.0f;
    }
}

bool OnScreenDisplaySetDisplaySize(int width, int height)
{
    if (!l_Initialized)
    {
        return false;
    }

    ImGuiIO& io    = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2((float)width, (float)height);

    l_DisplayWidth  = width;
    l_DisplayHeight = height;
    return true;
}

void OnScreenDisplaySetMessage(std::string message)
{
    if (!l_Initialized)
    {
        return;
    }

    l_Message     = message;
    l_MessageTime = std::chrono::high_resolution_clock::now();
}

void OnScreenDisplaySetOverlayText(std::string text)
{
    if (!l_Initialized)
    {
        return;
    }

    l_OverlayText = text;
}

void OnScreenDisplaySetConfigureModifier(bool active)
{
    std::lock_guard<std::mutex> lock(l_Mutex);
    l_ConfigureModifier = active;
}

bool OnScreenDisplayHandleMousePress(float x, float y, bool configureModifier)
{
    if (!l_Initialized || !l_Enabled)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(l_Mutex);
    l_ConfigureModifier = configureModifier || l_ConfigureModifier;

    if (!l_ConfigureModifier)
    {
        return false;
    }

    if (!l_CustomLayout)
    {
        const ImVec2 displaySize(static_cast<float>(l_DisplayWidth), static_cast<float>(l_DisplayHeight));
        for (int index = 0; index < static_cast<int>(OsdElement::Count); ++index)
        {
            if (!l_ElementRects[index].valid)
            {
                continue;
            }

            pos_to_permille(l_ElementRects[index].x,
                            l_ElementRects[index].y,
                            displaySize,
                            l_CustomPosPermille[index][0],
                            l_CustomPosPermille[index][1]);
        }
    }

    for (int index = static_cast<int>(OsdElement::Count) - 1; index >= 0; --index)
    {
        if (!point_in_rect(x, y, l_ElementRects[index]))
        {
            continue;
        }

        l_DraggingElement = index;
        l_DragOffsetX     = x - l_ElementRects[index].x;
        l_DragOffsetY     = y - l_ElementRects[index].y;
        l_CustomLayout    = true;

        const ImVec2 displaySize(static_cast<float>(l_DisplayWidth), static_cast<float>(l_DisplayHeight));
        pos_to_permille(l_ElementRects[index].x,
                        l_ElementRects[index].y,
                        displaySize,
                        l_CustomPosPermille[index][0],
                        l_CustomPosPermille[index][1]);
        return true;
    }

    return false;
}

bool OnScreenDisplayHandleMouseMove(float x, float y, bool configureModifier)
{
    if (!l_Initialized || !l_Enabled)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(l_Mutex);
    l_ConfigureModifier = configureModifier || l_ConfigureModifier;

    if (l_DraggingElement < 0)
    {
        return false;
    }

    const ImVec2 displaySize(static_cast<float>(l_DisplayWidth), static_cast<float>(l_DisplayHeight));
    float posX = x - l_DragOffsetX;
    float posY = y - l_DragOffsetY;
    clamp_pos_to_display(posX, posY, l_ElementRects[l_DraggingElement].width, l_ElementRects[l_DraggingElement].height, displaySize);
    pos_to_permille(posX, posY, displaySize, l_CustomPosPermille[l_DraggingElement][0], l_CustomPosPermille[l_DraggingElement][1]);
    return true;
}

bool OnScreenDisplayHandleMouseRelease(void)
{
    if (!l_Initialized || !l_Enabled)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(l_Mutex);
    if (l_DraggingElement < 0)
    {
        return false;
    }

    l_DraggingElement = -1;
    save_custom_positions();
    return true;
}

bool OnScreenDisplayIsDragging(void)
{
    std::lock_guard<std::mutex> lock(l_Mutex);
    return l_DraggingElement >= 0;
}

void OnScreenDisplayRender(void)
{
    if (!l_Initialized || !l_Enabled || l_RenderingPaused)
    {
        return;
    }

    bool showMessage = false;
    if (!l_Message.empty())
    {
        const auto currentTime  = std::chrono::high_resolution_clock::now();
        const int secondsPassed = std::chrono::duration_cast<std::chrono::seconds>(currentTime - l_MessageTime).count();
        showMessage = (secondsPassed < l_MessageDuration);
    }

    const bool showOverlay = !l_OverlayText.empty();
    std::string turnOverlayText;
    if (CoreSettingsGetBoolValue(SettingsID::GUI_TurnCountHud))
    {
        turnOverlayText = CoreGetTurnCountOverlayText();
    }
    const bool showTurnOverlay = !turnOverlayText.empty();
    if (!showMessage && !showOverlay && !showTurnOverlay)
    {
        return;
    }

    bool configureModifier = false;
    int draggingElement    = -1;
    bool customLayout      = false;
    int customPosPermille[static_cast<int>(OsdElement::Count)][2] = {};
    OsdElementRect frameRects[static_cast<int>(OsdElement::Count)] = {};
    {
        std::lock_guard<std::mutex> lock(l_Mutex);
        configureModifier = l_ConfigureModifier;
        draggingElement   = l_DraggingElement;
        customLayout      = l_CustomLayout;
        for (int index = 0; index < static_cast<int>(OsdElement::Count); ++index)
        {
            customPosPermille[index][0] = l_CustomPosPermille[index][0];
            customPosPermille[index][1] = l_CustomPosPermille[index][1];
        }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();

    if (showMessage)
    {
        render_osd_window(OsdElement::Message,
                          "Message",
                          l_Message.c_str(),
                          io.DisplaySize,
                          configureModifier,
                          draggingElement,
                          customLayout,
                          customPosPermille,
                          &frameRects[static_cast<int>(OsdElement::Message)]);
    }

    if (showOverlay)
    {
        render_osd_window(OsdElement::Overlay,
                          "Overlay",
                          l_OverlayText.c_str(),
                          io.DisplaySize,
                          configureModifier,
                          draggingElement,
                          customLayout,
                          customPosPermille,
                          &frameRects[static_cast<int>(OsdElement::Overlay)]);
    }

    if (showTurnOverlay)
    {
        render_osd_window(OsdElement::TurnCount,
                          "TurnOverlay",
                          turnOverlayText.c_str(),
                          io.DisplaySize,
                          configureModifier,
                          draggingElement,
                          customLayout,
                          customPosPermille,
                          &frameRects[static_cast<int>(OsdElement::TurnCount)]);
    }

    {
        std::lock_guard<std::mutex> lock(l_Mutex);
        for (auto& rect : l_ElementRects)
        {
            rect = {};
        }
        for (int index = 0; index < static_cast<int>(OsdElement::Count); ++index)
        {
            if (frameRects[index].valid)
            {
                l_ElementRects[index] = frameRects[index];
            }
        }
    }

    ImGui::Render();

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void OnScreenDisplayPause(void)
{
    l_RenderingPaused = true;
}

void OnScreenDisplayResume(void)
{
    l_RenderingPaused = false;
}
