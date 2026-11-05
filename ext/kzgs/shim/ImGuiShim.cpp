#include <mutex>
#include <string>
#include <cfloat>
// kzgs shim: ImGuiManager / FullscreenUI replacements.
// The GS devices always call ImGui::Render() when presenting, so a live ImGui context is required. kzgs creates a
// minimal one (default ProggyClean font via stb_truetype, no OSD, no fullscreen UI, no input) so presents are cheap.
// SPDX-License-Identifier: GPL-3.0+

#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiManager.h"
#include "GS/Renderers/Common/GSDevice.h"

#include "common/Timer.h"

#include "imgui.h"
#include "imgui_freetype.h"
#include "imgui_internal.h"

#include <algorithm>
#include <atomic>
#include <cstdio>

// GImGui is renamed to KzGImGui by kzgs_imgui_rename.h, which makes imgui.cpp skip its own definition.
ImGuiContext* GImGui = nullptr;

static Common::Timer s_last_render_time;

static void UpdateDisplaySize()
{
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(g_gs_device ? static_cast<float>(g_gs_device->GetWindowWidth()) : 0.0f,
		g_gs_device ? static_cast<float>(g_gs_device->GetWindowHeight()) : 0.0f);
}

bool ImGuiManager::Initialize()
{
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
	io.DisplayFramebufferScale = ImVec2(1, 1);
	UpdateDisplaySize();
	io.Fonts->AddFontDefault();
	s_last_render_time.Reset();
	NewFrame();
	return true;
}

bool ImGuiManager::InitializeFullscreenUI()
{
	return false;
}

void ImGuiManager::Shutdown(bool clear_state)
{
	if (g_gs_device)
		g_gs_device->DestroyImGuiTextures();
	if (ImGui::GetCurrentContext())
		ImGui::DestroyContext();
}

void ImGuiManager::WindowResized()
{
	if (ImGui::GetCurrentContext())
		UpdateDisplaySize();
}

void ImGuiManager::RequestScaleUpdate() {}
void ImGuiManager::ReloadFonts() {}

void ImGuiManager::NewFrame()
{
	ImGuiIO& io = ImGui::GetIO();
	io.DeltaTime = std::max(static_cast<float>(s_last_render_time.GetTimeSecondsAndReset()), 1.0f / 1000.0f);
	ImGui::NewFrame();
}

void ImGuiManager::SkipFrame()
{
	ImGui::EndFrame();
	NewFrame();
}

// kzgsSetShowFps: frame-rate counter. RenderOSD runs once per present (GSRenderer::PresentCurrentFrame), so counting its
// calls gives the presented frame rate.
std::atomic<bool> g_kzgs_show_fps{false};

static std::mutex s_banner_mutex;
static std::string s_banner;
void kzgsBannerSet(const char* text)
{
	std::lock_guard lock(s_banner_mutex);
	s_banner = text;
}

static void RenderBanner()
{
	std::string text;
	{
		std::lock_guard lock(s_banner_mutex);
		text = s_banner;
	}
	if (text.empty())
		return;
	const ImGuiIO& io = ImGui::GetIO();
	const float size = std::max(18.0f, io.DisplaySize.y / 26.0f);
	ImFont* font = ImGui::GetFont();
	// one or more lines, centred; dark panel with an orange (ISA) edge
	const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
	const float padX = size * 1.2f, padY = size * 0.7f;
	// the pre-boot waits ("KILLZONE CO-OP ...") in the middle of the black screen, in-game notices near the bottom
	const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * (text.rfind("KILLZONE", 0) == 0 ? 0.5f : 0.82f));
	const ImVec2 a(c.x - ts.x * 0.5f - padX, c.y - ts.y * 0.5f - padY), b(c.x + ts.x * 0.5f + padX, c.y + ts.y * 0.5f + padY);
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	const float edge = std::max(1.5f, size / 12.0f);
	dl->AddRectFilled(ImVec2(a.x - edge, a.y - edge), ImVec2(b.x + edge, b.y + edge), IM_COL32(232, 140, 40, 230), size * 0.25f);
	dl->AddRectFilled(a, b, IM_COL32(12, 11, 9, 235), size * 0.25f);
	dl->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(240, 236, 226, 255), text.c_str());
}

void ImGuiManager::RenderOSD()
{
	RenderBanner();
	if (!g_kzgs_show_fps.load(std::memory_order_relaxed))
		return;
	static Common::Timer s_timer;
	static int s_frames = 0;
	static float s_fps = 0.0f;
	++s_frames;
	const double t = s_timer.GetTimeSeconds();
	if (t >= 0.5)
	{
		s_fps = static_cast<float>(s_frames / t);
		s_frames = 0;
		s_timer.Reset();
	}
	const ImGuiIO& io = ImGui::GetIO();
	const float size = std::max(16.0f, io.DisplaySize.y / 36.0f);
	char text[32];
	std::snprintf(text, sizeof(text), "%.0f FPS", s_fps);
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	const ImVec2 pos(size * 0.5f, size * 0.4f);
	const float shadow = std::max(1.0f, size / 16.0f);
	dl->AddText(ImGui::GetFont(), size, ImVec2(pos.x + shadow, pos.y + shadow), IM_COL32(0, 0, 0, 200), text);
	dl->AddText(ImGui::GetFont(), size, pos, IM_COL32(255, 230, 64, 255), text);
}

void FullscreenUI::Render() {}

// PCSX2's imconfig.h enables IMGUI_ENABLE_FREETYPE; kzgs_imconfig.h adds stb_truetype and this routes the default
// font loader to it, so imgui_freetype.cpp / FreeType are not linked.
const ImFontLoader* ImGuiFreeType::GetFontLoader()
{
	return ImFontAtlasGetFontLoaderForStbTruetype();
}
