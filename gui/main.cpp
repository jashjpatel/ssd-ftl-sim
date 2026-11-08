// NAND FTL simulator: ImGui front end.
// Shows the test cases step by step: each step sends NVMe commands through the
// real NvmeController + Ftl on a tiny simulated drive and draws every page.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#define GL_SILENCE_DEPRECATION  // macOS: OpenGL still works, just marked deprecated
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#include "scenarios.hpp"

// ---------------------------------------------------------------------------
// Look and feel: modern font + flat dark theme
// ---------------------------------------------------------------------------
#ifndef FTL_FONT_DIR
#define FTL_FONT_DIR ""
#endif

static const ImVec4 THEME_BG = ImVec4(0.067f, 0.075f, 0.090f, 1.0f);   // app background

static void loadFont(GLFWwindow* window) {
    // Rasterize at the display's real pixel density (2x on Retina) and scale back,
    // so text is sharp instead of the blurry upscaled pixel font.
    float xs = 1.0f, ys = 1.0f;
    glfwGetWindowContentScale(window, &xs, &ys);
    const float scale = std::max(1.0f, xs);
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.OversampleH = 2; cfg.OversampleV = 2;
    const std::string path = std::string(FTL_FONT_DIR) + "/Roboto-Medium.ttf";
    ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), 15.0f * scale, &cfg);
    if (font) io.FontGlobalScale = 1.0f / scale;
    else { io.Fonts->AddFontDefault(); std::fprintf(stderr, "[ftl_gui] %s not found, using the default font\n", path.c_str()); }
}

static void applyTheme() {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 10.0f; st.ChildRounding = 8.0f; st.FrameRounding = 6.0f; st.PopupRounding = 8.0f;
    st.GrabRounding = 6.0f; st.TabRounding = 6.0f; st.ScrollbarRounding = 8.0f;
    st.WindowPadding = ImVec2(14, 12); st.FramePadding = ImVec2(10, 5); st.ItemSpacing = ImVec2(9, 7);
    st.ItemInnerSpacing = ImVec2(7, 5); st.ScrollbarSize = 12.0f; st.GrabMinSize = 10.0f;
    st.WindowBorderSize = 1.0f; st.ChildBorderSize = 1.0f; st.FrameBorderSize = 0.0f; st.TabBorderSize = 0.0f;
    st.WindowTitleAlign = ImVec2(0.0f, 0.5f); st.SeparatorTextBorderSize = 1.0f; st.SeparatorTextPadding = ImVec2(10, 4);

    const ImVec4 panel  = ImVec4(0.098f, 0.110f, 0.133f, 1.0f);
    const ImVec4 raised = ImVec4(0.145f, 0.160f, 0.192f, 1.0f);
    const ImVec4 hover  = ImVec4(0.200f, 0.220f, 0.265f, 1.0f);
    const ImVec4 accent = ImVec4(0.310f, 0.560f, 0.980f, 1.0f);
    const ImVec4 border = ImVec4(1.0f, 1.0f, 1.0f, 0.07f);
    auto a = [](ImVec4 c, float alpha) { c.w = alpha; return c; };

    ImVec4* c = st.Colors;
    c[ImGuiCol_Text]                 = ImVec4(0.91f, 0.93f, 0.96f, 1.0f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.55f, 0.59f, 0.66f, 1.0f);
    c[ImGuiCol_WindowBg]             = panel;
    c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]              = raised;
    c[ImGuiCol_Border]               = border;
    c[ImGuiCol_FrameBg]              = raised;
    c[ImGuiCol_FrameBgHovered]       = hover;
    c[ImGuiCol_FrameBgActive]        = hover;
    c[ImGuiCol_TitleBg]              = panel;
    c[ImGuiCol_TitleBgActive]        = panel;
    c[ImGuiCol_TitleBgCollapsed]     = panel;
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = raised;
    c[ImGuiCol_ScrollbarGrabHovered] = hover;
    c[ImGuiCol_ScrollbarGrabActive]  = hover;
    c[ImGuiCol_CheckMark]            = accent;
    c[ImGuiCol_SliderGrab]           = accent;
    c[ImGuiCol_SliderGrabActive]     = accent;
    c[ImGuiCol_Button]               = raised;
    c[ImGuiCol_ButtonHovered]        = hover;
    c[ImGuiCol_ButtonActive]         = a(accent, 0.85f);
    c[ImGuiCol_Header]               = a(accent, 0.22f);
    c[ImGuiCol_HeaderHovered]        = a(accent, 0.32f);
    c[ImGuiCol_HeaderActive]         = a(accent, 0.45f);
    c[ImGuiCol_Separator]            = border;
    c[ImGuiCol_SeparatorHovered]     = a(accent, 0.6f);
    c[ImGuiCol_SeparatorActive]      = accent;
    c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]    = a(accent, 0.5f);
    c[ImGuiCol_ResizeGripActive]     = accent;
    c[ImGuiCol_Tab]                  = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered]           = hover;
    c[ImGuiCol_TabSelected]          = raised;
    c[ImGuiCol_TabSelectedOverline]  = accent;
    c[ImGuiCol_TabDimmed]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelected]    = raised;
    c[ImGuiCol_PlotHistogram]        = accent;   // progress bar
    c[ImGuiCol_TableHeaderBg]        = raised;
    c[ImGuiCol_TableBorderStrong]    = border;
    c[ImGuiCol_TableBorderLight]     = border;
    c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_TextSelectedBg]       = a(accent, 0.35f);
    c[ImGuiCol_NavHighlight]         = accent;

    ImPlotStyle& ps = ImPlot::GetStyle();
    ImPlot::StyleColorsDark(&ps);
    ps.LineWeight = 2.0f;
    ps.PlotPadding = ImVec2(10, 10);
    ps.LegendPadding = ImVec2(8, 6);
    ps.Colormap = ImPlotColormap_Deep;
    ps.Colors[ImPlotCol_FrameBg]       = ImVec4(0, 0, 0, 0);
    ps.Colors[ImPlotCol_PlotBg]        = THEME_BG;
    ps.Colors[ImPlotCol_PlotBorder]    = border;
    ps.Colors[ImPlotCol_LegendBg]      = a(raised, 0.9f);
    ps.Colors[ImPlotCol_LegendBorder]  = border;
    ps.Colors[ImPlotCol_AxisGrid]      = ImVec4(1, 1, 1, 0.06f);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
static void glfwErrorCallback(int code, const char* desc) {
    std::fprintf(stderr, "GLFW error %d: %s\n", code, desc);
}

int main(int argc, char** argv) {
    // Optional flags:
    //   --scenarios N          open on test case N (0 = TC1)
    //   --exit-after-frames N  quit after N frames (for screenshots / CI)
    int exitAfterFrames = -1, startScenario = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--scenarios") startScenario = std::atoi(argv[i + 1]);
        if (std::string(argv[i]) == "--exit-after-frames") exitAfterFrames = std::atoi(argv[i + 1]);
    }

    glfwSetErrorCallback(glfwErrorCallback);
    std::fprintf(stderr, "[ftl_gui] starting\n");
    if (!glfwInit()) { std::fprintf(stderr, "[ftl_gui] glfwInit failed\n"); return 1; }

#if defined(__APPLE__)
    const char* glslVersion = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glslVersion = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    GLFWwindow* window = glfwCreateWindow(1400, 850, "NAND FTL Simulator: test cases", nullptr, nullptr);
    if (!window) { std::fprintf(stderr, "[ftl_gui] window creation failed\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwShowWindow(window);
    glfwFocusWindow(window);  // macOS: bring window in front of the terminal
    std::fprintf(stderr, "[ftl_gui] window open (OpenGL %s), close it to exit\n", (const char*)glGetString(GL_VERSION));
    glfwSwapInterval(1);      // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();   // only used for its style; harmless
    applyTheme();
    loadFont(window);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    scn::Runner runner;
    runner.select(std::clamp(startScenario, 0, int(runner.scenarios.size()) - 1));

    int frame = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        scn::window(runner);

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(THEME_BG.x, THEME_BG.y, THEME_BG.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        if (exitAfterFrames > 0 && ++frame >= exitAfterFrames) break;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
