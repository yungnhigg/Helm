// Helm host: Win32 window, WebView2 and ownership wiring.
#include "common/config.h"
#include "common/util.h"
#include <dwmapi.h>
#include "engine/engine.h"
#include "agent/registry.h"
#include "agent/jobs.h"
#include "agent/loop.h"
#include "session/session.h"
#include "session/memory.h"
#include <algorithm>
#include "workspace/workspace.h"
#include "ui/bridge.h"
#include "resource.h"

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <memory>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using namespace lar;

namespace {

struct App {
    Config cfg;
    std::unique_ptr<Engine> engine;
    Registry registry;
    std::unique_ptr<JobManager> jobs;
    std::unique_ptr<SessionStore> store;
    std::unique_ptr<WorkspaceStore> workspace;
    std::unique_ptr<MemoryStore> memory;
    std::unique_ptr<AgentLoop> loop;
    std::unique_ptr<Bridge> bridge;

    HWND hwnd = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    EventRegistrationToken web_message_token{};
    EventRegistrationToken navigation_token{};
    EventRegistrationToken permission_token{};
    bool web_message_registered = false;
    bool navigation_registered = false;
    bool permission_registered = false;
};

App g;

void resize_webview() {
    if (!g.controller || !g.hwnd) return;
    RECT rc{};
    GetClientRect(g.hwnd, &rc);
    g.controller->put_Bounds(rc);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: resize_webview(); return 0;
        case WM_APP_EMIT:
            if (g.bridge) g.bridge->on_wm_app_emit(lp);
            else delete reinterpret_cast<std::string*>(lp);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

bool trusted_source(ICoreWebView2WebMessageReceivedEventArgs* args) {
    LPWSTR raw = nullptr;
    if (FAILED(args->get_Source(&raw)) || !raw) return false;
    const std::wstring source(raw);
    CoTaskMemFree(raw);
    return source.rfind(L"https://app.local/", 0) == 0 || source == L"https://app.local";
}

void shutdown_runtime() noexcept {
    try {
        if (g.engine) g.engine->cancel();
        g.jobs.reset();       // completion callbacks may still use loop/store/bridge/engine
        g.engine.reset();     // drain inference work while loop/store/bridge remain alive

        if (g.webview && g.web_message_registered) {
            g.webview->remove_WebMessageReceived(g.web_message_token);
            g.web_message_registered = false;
        }
        if (g.webview && g.navigation_registered) {
            g.webview->remove_NavigationStarting(g.navigation_token);
            g.navigation_registered = false;
        }
        if (g.webview && g.permission_registered) {
            g.webview->remove_PermissionRequested(g.permission_token);
            g.permission_registered = false;
        }
        if (g.controller) g.controller->Close();
        g.webview.Reset();
        g.controller.Reset();

        g.bridge.reset();
        g.loop.reset();
        g.workspace.reset();
        g.store.reset();
    } catch (...) {
        // Shutdown is best-effort and must never throw through wWinMain.
    }
}

void create_webview() {
    const std::wstring user_data = utf8_to_wide(app_data_dir() + "webview-data");
    const HRESULT start = CreateCoreWebView2EnvironmentWithOptions(nullptr, user_data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || !env) {
                    MessageBoxW(g.hwnd, L"WebView2 environment failed. Install the Microsoft Edge WebView2 runtime.", L"Helm", MB_ICONERROR);
                    return hr;
                }
                return env->CreateCoreWebView2Controller(g.hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT hr2, ICoreWebView2Controller* ctrl) -> HRESULT {
                            if (FAILED(hr2) || !ctrl) {
                                MessageBoxW(g.hwnd, L"WebView2 controller creation failed.", L"Helm", MB_ICONERROR);
                                return hr2;
                            }
                            g.controller = ctrl;
                            if (FAILED(g.controller->get_CoreWebView2(&g.webview)) || !g.webview) return E_FAIL;
                            resize_webview();

                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(g.webview->get_Settings(&settings)) && settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(TRUE);
                                settings->put_IsZoomControlEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                            }

                            ComPtr<ICoreWebView2_3> wv3;
                            if (FAILED(g.webview.As(&wv3))) return E_NOINTERFACE;
                            const std::wstring webdir = utf8_to_wide(exe_dir() + "web");
                            HRESULT map_hr = wv3->SetVirtualHostNameToFolderMapping(
                                L"app.local", webdir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                            if (FAILED(map_hr)) return map_hr;

                            g.bridge = std::make_unique<Bridge>(
                                g.cfg, *g.engine, *g.loop, *g.jobs, *g.store, *g.memory, *g.workspace, g.registry, g.hwnd,
                                [](const std::wstring& payload) {
                                    if (g.webview) g.webview->PostWebMessageAsJson(payload.c_str());
                                });

                            HRESULT nav_hr = g.webview->add_NavigationStarting(
                                Callback<ICoreWebView2NavigationStartingEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                        LPWSTR raw = nullptr;
                                        if (SUCCEEDED(args->get_Uri(&raw)) && raw) {
                                            const std::wstring uri(raw);
                                            CoTaskMemFree(raw);
                                            if (uri.rfind(L"https://app.local/", 0) != 0 && uri != L"about:blank")
                                                args->put_Cancel(TRUE);
                                        }
                                        return S_OK;
                                    }).Get(), &g.navigation_token);
                            if (FAILED(nav_hr)) {
                                PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
                                return nav_hr;
                            }
                            g.navigation_registered = true;

                            HRESULT permission_hr = g.webview->add_PermissionRequested(
                                Callback<ICoreWebView2PermissionRequestedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                                        LPWSTR raw_uri = nullptr;
                                        COREWEBVIEW2_PERMISSION_KIND kind{};
                                        if (FAILED(args->get_Uri(&raw_uri)) || !raw_uri || FAILED(args->get_PermissionKind(&kind))) {
                                            if (raw_uri) CoTaskMemFree(raw_uri);
                                            return S_OK;
                                        }
                                        const std::wstring uri(raw_uri);
                                        CoTaskMemFree(raw_uri);
                                        if ((uri.rfind(L"https://app.local/", 0) == 0 || uri == L"https://app.local") &&
                                            kind == COREWEBVIEW2_PERMISSION_KIND_MICROPHONE) {
                                            args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
                                        }
                                        return S_OK;
                                    }).Get(), &g.permission_token);
                            if (SUCCEEDED(permission_hr)) g.permission_registered = true;

                            HRESULT message_hr = g.webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        if (!trusted_source(args)) {
                                            log("blocked WebView message from untrusted origin");
                                            return S_OK;
                                        }
                                        LPWSTR msg = nullptr;
                                        if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg) {
                                            if (g.bridge) g.bridge->on_web_message(msg);
                                            CoTaskMemFree(msg);
                                        }
                                        return S_OK;
                                    }).Get(), &g.web_message_token);
                            if (FAILED(message_hr)) {
                                MessageBoxW(g.hwnd, L"Helm could not attach its UI bridge.", L"Helm", MB_ICONERROR);
                                PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
                                return message_hr;
                            }
                            g.web_message_registered = true;

                            return g.webview->Navigate(L"https://app.local/index.html");
                        }).Get());
            }).Get());

    if (FAILED(start)) MessageBoxW(g.hwnd, L"WebView2 startup failed.", L"Helm", MB_ICONERROR);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_hr)) return 1;

    std::string err;
    if (!g.cfg.load(err)) {
        MessageBoxW(nullptr, utf8_to_wide(err).c_str(), L"Helm — configuration error", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    g.engine = std::make_unique<Engine>(g.cfg);
    g.jobs = std::make_unique<JobManager>(2);
    g.store = std::make_unique<SessionStore>();
    g.workspace = std::make_unique<WorkspaceStore>(g.cfg);
    g.memory = std::make_unique<MemoryStore>(static_cast<size_t>(std::max(1024, g.cfg.memory_budget_bytes)));

    register_tool_time(g.registry);
    register_tool_dice(g.registry);
    register_tool_demo_job(g.registry);
    register_tool_files(g.registry, g.cfg);
    if (g.cfg.allow_process_tools) register_tool_process(g.registry, g.cfg);
    register_tool_web_crawl(g.registry);
    register_external_tools(g.registry, g.cfg);
    register_run_control(g.registry);
    if (g.cfg.enable_memory) register_tool_memory(g.registry, g.cfg, *g.memory);
    g.workspace->register_tool_packs(g.registry);

    AgentEvents events;
    events.emit = [](const nlohmann::json& j) { if (g.bridge) g.bridge->emit(j); };
    g.loop = std::make_unique<AgentLoop>(g.cfg, *g.engine, g.registry, *g.store, *g.workspace,
                                        *g.jobs, *g.memory, events);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(8, 8, 10));
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_HELM));
    wc.lpszClassName = L"helm_main";
    if (!RegisterClassW(&wc)) {
        shutdown_runtime();
        CoUninitialize();
        return 1;
    }

    g.hwnd = CreateWindowExW(0, L"helm_main", L"Helm",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1440, 920,
        nullptr, nullptr, inst, nullptr);
    if (!g.hwnd) {
        shutdown_runtime();
        CoUninitialize();
        return 1;
    }
    // The window caption is drawn by Windows, not by our CSS, so no amount of
    // styling inside the web view can darken it. It has to be opted in through
    // DWM. Attribute 20 is DWMWA_USE_IMMERSIVE_DARK_MODE on Windows 10 2004+
    // and Windows 11; 19 was the pre-release number on earlier 10 builds, hence
    // the fallback. Painting the caption black as well makes it continuous with
    // the in-app header instead of a dark grey strip above a black one.
    {
        BOOL dark = TRUE;
        if (FAILED(DwmSetWindowAttribute(g.hwnd, 20, &dark, sizeof(dark))))
            DwmSetWindowAttribute(g.hwnd, 19, &dark, sizeof(dark));
        // COLORREF is 0x00BBGGRR. Ignored before Windows 11 22000, harmlessly.
        COLORREF caption = 0x00000000;
        DwmSetWindowAttribute(g.hwnd, 35, &caption, sizeof(caption));   // caption
        COLORREF text = 0x00F0F0F0;
        DwmSetWindowAttribute(g.hwnd, 36, &text, sizeof(text));         // caption text
        COLORREF border = 0x00151515;
        DwmSetWindowAttribute(g.hwnd, 34, &border, sizeof(border));     // border
    }

    ShowWindow(g.hwnd, show);
    UpdateWindow(g.hwnd);
    create_webview();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    shutdown_runtime();
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
