///////////////////////////////////////////////////////////////////////////////
// Copyright 2017 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License.  You may obtain a copy
// of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
// License for the specific language governing permissions and limitations
// under the License.
///////////////////////////////////////////////////////////////////////////////

#define WIN32_LEAN_AND_MEAN
#include <sdkddkver.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h> // must be after windows.h
#include <ShellScalingApi.h>
#include <interactioncontext.h>
#include <mmsystem.h>
#include <map>
#include <vector>
#include <iostream>

#include "asteroids_d3d11.h"
#include "asteroids_d3d12.h"
#include "camera.h"
#include "profile.h"
#include "gui.h"

#include <fstream>
#include <utility>
#include <tuple>

using namespace DirectX;

namespace {

// Global demo state
Settings gSettings;
OrbitCamera gCamera;

IDXGIFactory2* gDXGIFactory = nullptr;

AsteroidsD3D11::Asteroids* gWorkloadD3D11 = nullptr;
AsteroidsD3D12::Asteroids* gWorkloadD3D12 = nullptr;

GUI gGUI;
GUISprite* gD3D11Control;
GUISprite* gD3D12Control;
GUIText* gFPSControl;

enum
{
    basicStyle = WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_VISIBLE,
    windowedStyle = basicStyle | WS_OVERLAPPEDWINDOW,
    fullscreenStyle = basicStyle
};

bool CheckDll(char const* dllName)
{
    auto hModule = LoadLibrary(dllName);
    if (hModule == NULL) {
        return false;
    }
    FreeLibrary(hModule);
    return true;
}


UINT SetupDPI()
{
    // Just do system DPI awareness for now for simplicity... scale the 3D content
    SetProcessDpiAwareness(PROCESS_SYSTEM_DPI_AWARE);

    UINT dpiX = 0, dpiY;
    POINT pt = {1, 1};
    auto hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (SUCCEEDED(GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        return dpiX;
    } else {
        return 96; // default
    }
}


void ResetCameraView()
{
    auto center    = XMVectorSet(0.0f, -0.4f*SIM_DISC_RADIUS, 0.0f, 0.0f);
    auto radius    = SIM_ORBIT_RADIUS + SIM_DISC_RADIUS + 10.f;
    auto minRadius = SIM_ORBIT_RADIUS - 3.0f * SIM_DISC_RADIUS;
    auto maxRadius = SIM_ORBIT_RADIUS + 3.0f * SIM_DISC_RADIUS;
    auto longAngle = 4.50f;
    auto latAngle  = 1.45f;
    gCamera.View(center, radius, minRadius, maxRadius, longAngle, latAngle);
}

void ToggleFullscreen(HWND hWnd)
{
    static WINDOWPLACEMENT prevPlacement = { sizeof(prevPlacement) };
    DWORD dwStyle = (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE);
    if ((dwStyle & windowedStyle) == windowedStyle)
    {
        MONITORINFO mi = { sizeof(mi) };
        if (GetWindowPlacement(hWnd, &prevPlacement) &&
            GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY), &mi))
        {
            SetWindowLong(hWnd, GWL_STYLE, fullscreenStyle);
            SetWindowPos(hWnd, HWND_TOP,
                mi.rcMonitor.left, mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        SetWindowLong(hWnd, GWL_STYLE, windowedStyle);
        SetWindowPlacement(hWnd, &prevPlacement);
        SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}


} // namespace



LRESULT CALLBACK WindowProc(
    HWND hWnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (message) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_SIZE: {
            UINT ww = LOWORD(lParam);
            UINT wh = HIWORD(lParam);

            // Ignore resizing to minimized
            if (ww == 0 || wh == 0) return 0;

            gSettings.windowWidth = (int)ww;
            gSettings.windowHeight = (int)wh;
            gSettings.renderWidth = (UINT)(double(gSettings.windowWidth)  * gSettings.renderScale);
            gSettings.renderHeight = (UINT)(double(gSettings.windowHeight) * gSettings.renderScale);

            // Update camera projection
            float aspect = (float)gSettings.renderWidth / (float)gSettings.renderHeight;
            gCamera.Projection(XM_PIDIV2 * 0.8f * 3 / 2, aspect);

            // Resize currently active swap chain
            if (gSettings.d3d12)
                gWorkloadD3D12->ResizeSwapChain(gDXGIFactory, hWnd, gSettings.renderWidth, gSettings.renderHeight);
            else
                gWorkloadD3D11->ResizeSwapChain(gDXGIFactory, hWnd, gSettings.renderWidth, gSettings.renderHeight);

            return 0;
        }

        case WM_KEYDOWN:
            if (lParam & (1 << 30)) {
                // Ignore repeats
                return 0;
            }
            switch (wParam) {
            case VK_SPACE:
                gSettings.animate = !gSettings.animate;
                std::cout << "Animate: " << gSettings.animate << std::endl;
                return 0;

                /* Disabled for demo setup */
            case 'V':
                gSettings.vsync = !gSettings.vsync;
                std::cout << "Vsync: " << gSettings.vsync << std::endl;
                return 0;
            case 'M':
                gSettings.multithreadedRendering = !gSettings.multithreadedRendering;
                std::cout << "Multithreaded Rendering: " << gSettings.multithreadedRendering << std::endl;
                return 0;
            case 'I':
                gSettings.executeIndirect = !gSettings.executeIndirect;
                std::cout << "ExecuteIndirect Rendering: " << gSettings.executeIndirect << std::endl;
                return 0;
            case 'S':
                gSettings.submitRendering = !gSettings.submitRendering;
                std::cout << "Submit Rendering: " << gSettings.submitRendering << std::endl;
                return 0;

            case '1': gSettings.d3d12 = (gWorkloadD3D11 == nullptr); return 0;
            case '2': gSettings.d3d12 = (gWorkloadD3D12 != nullptr); return 0;

            case VK_ESCAPE:
                SendMessage(hWnd, WM_CLOSE, 0, 0);
                return 0;
            } // Switch on key code
            return 0;

        case WM_SYSKEYDOWN:
            if (lParam & (1 << 30)) {
                // Ignore repeats
                return 0;
            }
            switch (wParam) {
            case VK_RETURN:
                ToggleFullscreen(hWnd);
                break;
            }
            return 0;

        case WM_MOUSEWHEEL: {
            auto delta = GET_WHEEL_DELTA_WPARAM(wParam);
            gCamera.ZoomRadius(-0.07f * delta);
            return 0;
        }

        case WM_POINTERDOWN:
        case WM_POINTERUPDATE:
        case WM_POINTERUP: {
            auto pointerId = GET_POINTERID_WPARAM(wParam);
            POINTER_INFO pointerInfo;
            if (GetPointerInfo(pointerId, &pointerInfo)) {
                if (message == WM_POINTERDOWN) {
                    // Compute pointer position in render units
                    POINT p = pointerInfo.ptPixelLocation;
                    ScreenToClient(hWnd, &p);
                    RECT clientRect;
                    GetClientRect(hWnd, &clientRect);
                    p.x = p.x * gSettings.renderWidth / (clientRect.right - clientRect.left);
                    p.y = p.y * gSettings.renderHeight / (clientRect.bottom - clientRect.top);

                    auto guiControl = gGUI.HitTest(p.x, p.y);
                    if (guiControl == gFPSControl) {
                        gSettings.lockFrameRate = !gSettings.lockFrameRate;
                    } else if (guiControl == gD3D11Control) { // Switch to D3D12
                        gSettings.d3d12 = (gWorkloadD3D12 != nullptr);
                    } else if (guiControl == gD3D12Control) { // Switch to D3D11
                        gSettings.d3d12 = (gWorkloadD3D11 == nullptr);
                    } else { // Camera manipulation
                        gCamera.AddPointer(pointerId);
                    }
                }

                // Otherwise send it to the camera controls
                gCamera.ProcessPointerFrames(pointerId, &pointerInfo);
                if (message == WM_POINTERUP) gCamera.RemovePointer(pointerId);
            }
            return 0;
        }

        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
}


int main(int argc, char** argv)
{
    auto d3d11Available = CheckDll("d3d11.dll");
    auto d3d12Available = CheckDll("d3d12.dll");

    // Must be done before any windowing-system-like things or else virtualization will kick in
    auto dpi = SetupDPI();
    // By default render at the lower resolution and scale up based on system settings
    gSettings.renderScale = 96.0 / double(dpi);

    // Scale default window size based on dpi
    gSettings.windowWidth *= dpi / 96;
    gSettings.windowHeight *= dpi / 96;

    for (int a = 1; a < argc; ++a) {
        if (_stricmp(argv[a], "-close_after") == 0 && a + 1 < argc) {
            gSettings.closeAfterSeconds = atof(argv[++a]);
        } else if (_stricmp(argv[a], "-nod3d11") == 0) {
            d3d11Available = false;
        } else if (_stricmp(argv[a], "-warp") == 0) {
            gSettings.warp = true;
        } else if (_stricmp(argv[a], "-nod3d12") == 0) {
            d3d12Available = false;
        } else if (_stricmp(argv[a], "-indirect") == 0) {
            gSettings.executeIndirect = true;
        } else if (_stricmp(argv[a], "-fullscreen") == 0) {
            gSettings.windowed = false;
        } else if (_stricmp(argv[a], "-window") == 0 && a + 2 < argc) {
            gSettings.windowWidth = atoi(argv[++a]);
            gSettings.windowHeight = atoi(argv[++a]);
        } else if (_stricmp(argv[a], "-render_scale") == 0 && a + 1 < argc) {
            gSettings.renderScale = atof(argv[++a]);
        } else if (_stricmp(argv[a], "-locked_fps") == 0 && a + 1 < argc) {
            gSettings.lockedFrameRate = atoi(argv[++a]);
        } else if (_stricmp(argv[a], "-stats_csv_file_name") == 0 && a + 1 < argc) {
            gSettings.statsCsvFileName = argv[++a];
        } else if (_stricmp(argv[a], "-stats_summary_csv_file_name") == 0 && a + 1 < argc) {
            gSettings.statsSummaryCsvFileName = argv[++a];
        } else {
            fprintf(stderr, "error: unrecognized argument '%s'\n", argv[a]);
            fprintf(stderr, "usage: asteroids_d3d12 [options]\n");
            fprintf(stderr, "options:\n");
            fprintf(stderr, "  -close_after [seconds]\n");
            fprintf(stderr, "  -nod3d11\n");
            fprintf(stderr, "  -nod3d12\n");
            fprintf(stderr, "  -fullscreen\n");
            fprintf(stderr, "  -window [width] [height]\n");
            fprintf(stderr, "  -render_scale [scale]\n");
            fprintf(stderr, "  -stats_csv_file_name <stats csv file name>\n");
            fprintf(stderr, "  -stats_summary_csv_file_name <stats summary csv file name>\n");
            fprintf(stderr, "  -locked_fps [fps]\n");
            fprintf(stderr, "  -warp\n");
            return -1;
        }
    }

    if (!d3d11Available && !d3d12Available) {
        fprintf(stderr, "error: neither D3D11 nor D3D12 available.\n");
        return -1;
    }

    // DXGI Factory
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&gDXGIFactory)));

    // Setup GUI
    gD3D12Control = gGUI.AddSprite(5, 10, 140, 50, "directx12.dds");
    gD3D11Control = gGUI.AddSprite(5, 10, 140, 50, "directx11.dds");
    gFPSControl = gGUI.AddText(150, 10);

    ResetCameraView();
    // Camera projection set up in WM_SIZE

    AsteroidsSimulation asteroids(1337, NUM_ASTEROIDS, NUM_UNIQUE_MESHES, MESH_MAX_SUBDIV_LEVELS, NUM_UNIQUE_TEXTURES);

    // Create workloads
    if (d3d11Available) {
        gWorkloadD3D11 = new AsteroidsD3D11::Asteroids(&asteroids, &gGUI, gSettings.warp);
    }

    if (d3d12Available) {
        // If requested, enumerate the warp adapter
        // TODO: Allow picking from multiple hardware adapters
        IDXGIAdapter1* adapter = nullptr;

        if (gSettings.warp) {
            IDXGIFactory4* DXGIFactory4 = nullptr;
            if (FAILED(gDXGIFactory->QueryInterface(&DXGIFactory4))) {
                fprintf(stderr, "error: WARP requires IDXGIFactory4 interface which is not present on this system!\n");
                return -1;
            }

            auto hr = DXGIFactory4->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
            DXGIFactory4->Release();

            if (FAILED(hr)) {
                fprintf(stderr, "error: WARP adapter not present on this system!\n");
                return -1;
            }
        }

        gWorkloadD3D12 = new AsteroidsD3D12::Asteroids(&asteroids, &gGUI, NUM_SUBSETS, adapter);
    }
    gSettings.d3d12 = (gWorkloadD3D12 != nullptr);


    // init window class
    WNDCLASSEX windowClass;
    ZeroMemory(&windowClass, sizeof(WNDCLASSEX));
    windowClass.cbSize = sizeof(WNDCLASSEX);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = GetModuleHandle(NULL);
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass.lpszClassName = "AsteroidsD3D12WindowClass";
    RegisterClassEx(&windowClass);

    RECT windowRect = { 0, 0, gSettings.windowWidth, gSettings.windowHeight };
    AdjustWindowRect(&windowRect, windowedStyle, FALSE);

    // create the window and store a handle to it
    auto hWnd = CreateWindowEx(
        WS_EX_APPWINDOW,
        "AsteroidsD3D12WindowClass",
        "AsteroidsD3D12",
        windowedStyle,
        0, // CW_USE_DEFAULT
        0, // CW_USE_DEFAULT
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        NULL,
        NULL,
        windowClass.hInstance,
        NULL);

    if (!gSettings.windowed) {
        ToggleFullscreen(hWnd);
    }

    SetForegroundWindow(hWnd);

    // Initialize performance counters
    UINT64 perfCounterFreq = 0;
    UINT64 lastPerfCount = 0;
    QueryPerformanceFrequency((LARGE_INTEGER*)&perfCounterFreq);
    QueryPerformanceCounter((LARGE_INTEGER*)&lastPerfCount);

    // main loop
    double elapsedTime = 0.0;
    double timeInterval = 0.0;
    double frameTime = 0.0;
    double sumFPS = 0;
    double currentFPS = 0;
    double minFPS = 99999999;
    double maxFPS = 0;
    std::uint64_t numFrames = 0;
    if (gSettings.statsCsvFileName == "")
    {
        gSettings.statsCsvFileName = "asteroid_summary_stats.csv";
    }
    if (gSettings.statsSummaryCsvFileName == "")
    {
        gSettings.statsSummaryCsvFileName = "asteroid_stats.csv";
    }

    std::vector<std::tuple<double, double, double>> fpsHistoryVector;

    int lastMouseX = 0;
    int lastMouseY = 0;
    POINTER_INFO pointerInfo = {};

    timeBeginPeriod(1);
    EnableMouseInPointer(TRUE);

    if (gSettings.closeAfterSeconds > 0.0)
    {
        fpsHistoryVector.reserve(int(gSettings.closeAfterSeconds) + 4);
    }

    for (;;)
    {
        bool d3d12LastFrame = gSettings.d3d12;

        MSG msg = {};
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                // Cleanup
                delete gWorkloadD3D11;
                delete gWorkloadD3D12;
                SafeRelease(&gDXGIFactory);
                timeEndPeriod(1);
                EnableMouseInPointer(FALSE);
                return (int)msg.wParam;
            };

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // If we swap to a new API we need to recreate swap chains
        if (d3d12LastFrame != gSettings.d3d12) {
            if (gSettings.d3d12) {
                gWorkloadD3D11->ReleaseSwapChain();
                gWorkloadD3D12->ResizeSwapChain(gDXGIFactory, hWnd, gSettings.renderWidth, gSettings.renderHeight);
            } else {
                gWorkloadD3D12->ReleaseSwapChain();
                gWorkloadD3D11->ResizeSwapChain(gDXGIFactory, hWnd, gSettings.renderWidth, gSettings.renderHeight);
            }
        }

        // Still need to process inertia even when no interaction is happening
        gCamera.ProcessInertia();

        // In D3D12 we'll wait on the GPU before taking the timestamp (more consistent)
        if (gSettings.d3d12) {
            gWorkloadD3D12->WaitForReadyToRender();
        }

        // Get time delta
        UINT64 count;
        QueryPerformanceCounter((LARGE_INTEGER*)&count);
        auto rawFrameTime = (double)(count - lastPerfCount) / perfCounterFreq;
        elapsedTime += rawFrameTime;
        lastPerfCount = count;

        // Maintaining absolute time sync is not important in this demo so we can err on the "smoother" side
        double alpha = 0.2f;
        frameTime = alpha * rawFrameTime + (1.0f - alpha) * frameTime;

        // Update GUI
        {
            char buffer[256];
            sprintf(buffer, "Asteroids D3D1%c - %4.1f ms", gSettings.d3d12 ? '2' : '1', 1000.f * frameTime);
            SetWindowText(hWnd, buffer);

            if (gSettings.lockFrameRate) {
                sprintf(buffer, "(Locked)");
            } else {
                if (frameTime != 0)
                    currentFPS = 1.0f / frameTime;
                sprintf(buffer, "%.0f fps", currentFPS);
            }

            if (gSettings.closeAfterSeconds > 0.0)
            {
                ++numFrames;
                if (numFrames > 100)
                {
                    sumFPS = sumFPS + currentFPS;

                    if (currentFPS < minFPS)
                    {
                        minFPS = currentFPS;
                    }
                    if (currentFPS > maxFPS)
                    {
                        maxFPS = currentFPS;
                    }
                }

                timeInterval += rawFrameTime * 1000.f;

                if (timeInterval >= 1000)
                {
                    fpsHistoryVector.emplace_back(std::make_tuple(elapsedTime, frameTime * 1000.f, rawFrameTime * 1000.f));
                    timeInterval = 0;
                }
            }

            gFPSControl->Text(buffer);

            gD3D12Control->Visible(gSettings.d3d12);
            gD3D11Control->Visible(!gSettings.d3d12);
        }

        if (gSettings.d3d12) {
            gWorkloadD3D12->Render((float)frameTime, gCamera, gSettings);
        } else {
            gWorkloadD3D11->Render((float)frameTime, gCamera, gSettings);
        }

        if (gSettings.lockFrameRate) {
            ProfileBeginFrameLockWait();

            UINT64 afterRenderCount;
            QueryPerformanceCounter((LARGE_INTEGER*)&afterRenderCount);
            double renderTime = (double)(afterRenderCount - count) / perfCounterFreq;

            double targetRenderTime = 1.0 / double(gSettings.lockedFrameRate);
            double deltaMs = (targetRenderTime - renderTime) * 1000.0;
            if (deltaMs > 1.0) {
                Sleep((DWORD)deltaMs);
            }

            ProfileEndFrameLockWait();
        }

        // All done?
        if (gSettings.closeAfterSeconds > 0.0 && elapsedTime > gSettings.closeAfterSeconds) {

            std::ofstream statsFile;
            statsFile.open(gSettings.statsSummaryCsvFileName);
            statsFile << "MinFPS,MaxFPS,AverageFPS" << std::endl;
            double averageFPS = sumFPS / (numFrames - 100);
            statsFile << minFPS << "," << maxFPS << "," << averageFPS << std::endl;
            statsFile.close();

            statsFile.open(gSettings.statsCsvFileName);
            statsFile << "ElapsedTime(s),FrameTime(ms),RawFrameTime(ms)" << std::endl;
            for (auto sample : fpsHistoryVector)
            {
                statsFile << std::get<0>(sample) << "," << std::get<1>(sample) << "," << std::get<2>(sample) << std::endl;
            }
            statsFile.close();

            SendMessage(hWnd, WM_CLOSE, 0, 0);
            return 0;
            break;
        }
    }

    // Shouldn't get here
    return 1;
}
