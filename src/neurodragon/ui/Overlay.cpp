#include "neurodragon/ui/Overlay.h"

#include "neurodragon/Config.h"
#include "neurodragon/Log.h"
#include "neurodragon/ui/InputGuard.h"
#include "neurodragon/ui/Menu.h"
#include "neurodragon/ui/Screenshot.h"
#include "neurodragon/ui/Theme.h"
#include "neurodragon/utils/Utils.h"

#include <Windows.h>

// Some SDK revisions of d3d12.h re-define what d3d11on12.h already declares. Rename
// them around the include block (same trick Mangrove/LHolo use) instead of fighting
// the headers.
#define D3D12_FEATURE_DATA_D3D12_OPTIONS D3D12_FEATURE_DATA_D3D12_OPTIONS_LEGACY
#define D3D12_FEATURE_DATA_ARCHITECTURE  D3D12_FEATURE_DATA_ARCHITECTURE_LEGACY
#define D3D12_RAYTRACING_GEOMETRY_DESC   D3D12_RAYTRACING_GEOMETRY_DESC_LEGACY
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#undef D3D12_FEATURE_DATA_D3D12_OPTIONS
#undef D3D12_FEATURE_DATA_ARCHITECTURE
#undef D3D12_RAYTRACING_GEOMETRY_DESC

#include <MinHook.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// The backend header no longer declares this in newer ImGui revisions.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace neurodragon::ui {
namespace {

// ---------------------------------------------------------------------------
// DXGI / D3D entry points
// ---------------------------------------------------------------------------

using PresentFn       = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn      = HRESULT(__stdcall*)(IDXGISwapChain1*, UINT, UINT, DXGI_PRESENT_PARAMETERS const*);
using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1Fn =
    HRESULT(__stdcall*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, UINT const*, IUnknown* const*);
using ExecuteCommandListsFn = void(__stdcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

// Vtable slots are stable ABI positions: the addresses are taken once from
// throw-away objects of each interface.
constexpr size_t kPresentVtableIndex             = 8;
constexpr size_t kResizeBuffersVtableIndex       = 13;
constexpr size_t kPresent1VtableIndex            = 22;
constexpr size_t kResizeBuffers1VtableIndex      = 39;
constexpr size_t kExecuteCommandListsVtableIndex = 10;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr ULONGLONG kToastLifetimeMs = 2200;

// "Give the mouse back" message: Present runs on the render thread and must not
// touch the cursor state while the game is dispatching input messages.
constexpr UINT kMsgRestoreGameMouse = WM_APP + 0x201;

// Font atlas build and font texture creation both happen inside the first
// ImGui_ImplDX11_NewFrame(). While the panel is closed no frame is recorded, so
// without this warm-up the whole cost lands on the frame the player opens the
// panel for the first time - a visible one-second hitch. Spend it on a loading
// screen instead.
constexpr int kWarmupFrames = 2;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

PresentFn             gOriginalPresent{};
Present1Fn            gOriginalPresent1{};
ResizeBuffersFn       gOriginalResizeBuffers{};
ResizeBuffers1Fn      gOriginalResizeBuffers1{};
ExecuteCommandListsFn gOriginalExecuteCommandLists{};

void* gPresentTarget{};
void* gPresent1Target{};
void* gResizeBuffersTarget{};
void* gResizeBuffers1Target{};
void* gExecuteCommandListsTarget{};

ID3D11Device*        gDevice{};
ID3D11DeviceContext* gDeviceContext{};
ID3D11On12Device*    gDevice11On12{};
ID3D12CommandQueue*  gGameQueue{};

HWND    gWindow{};
WNDPROC gOriginalWndProc{};

std::atomic_bool gInstalled{false};
std::atomic_bool gShuttingDown{false};
std::atomic_bool gRendering{false};
std::atomic_bool gImGuiReady{false};
std::atomic_bool gVisible{false};
bool             gGraphicsReady{};
bool             gVisibleLastFrame{};
std::mutex       gResourceMutex;

int gWarmupFrames{};

// Input bookkeeping: what the game has been told is down, so opening the panel can
// synthesise the matching releases instead of leaving the player walking forward.
bool                  gToggleKeyDown{};
bool                  gConsumeEscapeRelease{};
std::array<bool, 256> gGameKeysDown{};
std::array<bool, 5>   gGameMouseButtonsDown{};

// Panel hotkey and its one-shot rebinding capture. The window thread reads the
// key and writes the capture result; the render thread (panel) starts the capture
// and takes the result. Atomics keep both sides free of locks.
std::atomic<int> gHotkey{VK_END};
std::atomic_bool gCapturingHotkey{false};
std::atomic<int> gCapturedHotkey{0};

// Screenshot hotkey (config ui.screenshotKey, F2 by default). The window thread
// raises the request, the render thread consumes it at the next Present and copies
// the frame *before* drawing the panel, so the PNG never contains the overlay.
std::atomic<int>  gScreenshotKey{VK_F2};
std::atomic_bool  gScreenshotRequested{false};
bool              gScreenshotKeyDown{};
std::wstring      gScreenshotDirectory;

// Toast.
std::mutex  gToastMutex;
std::string gToastText;
ULONGLONG   gToastExpireAt{};

// Frame timing (published to the panel).
std::mutex                 gFrameHistoryMutex;
std::vector<float>         gFrameHistory;
double                     gLastPresentSeconds{};
double                     gAccumulatedSeconds{};
unsigned                   gAccumulatedFrames{};
std::atomic<double>        gFps{0.0};
std::atomic<double>        gFrameMs{0.0};
std::atomic<std::uint64_t> gFrameCount{0};

auto& logger() { return neurodragon::utils::logger(); }

// ---------------------------------------------------------------------------
// Threading model (read before editing this file)
//
// The window procedure runs on the game's main thread; rendering runs on the
// thread that calls Present. ImGui is single threaded, with exactly two touch
// points: the window thread feeds input (WndProcHandler + the window's state), the
// render thread runs NewFrame/Render and publishes read-only snapshots if the
// window thread needs them. Do not call ImGui from the window thread unless it is
// an input function.
//
// Never write diagnostics with std::printf from inside the game process: a console
// in QuickEdit mode blocks the writer, which looks exactly like a hang. Every line
// here goes through the mod logger.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Window / input
// ---------------------------------------------------------------------------

HWND findGameWindow() {
    struct Search {
        DWORD pid;
        HWND  result;
    } search{GetCurrentProcessId(), nullptr};

    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            auto& state = *reinterpret_cast<Search*>(parameter);
            DWORD pid{};
            GetWindowThreadProcessId(window, &pid);
            if (pid == state.pid && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
                state.result = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search)
    );
    return search.result;
}

LRESULT forwardToGame(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && wParam < gGameKeysDown.size()) {
        gGameKeysDown[static_cast<size_t>(wParam)] = true;
    } else if ((message == WM_KEYUP || message == WM_SYSKEYUP) && wParam < gGameKeysDown.size()) {
        gGameKeysDown[static_cast<size_t>(wParam)] = false;
    }
    switch (message) {
    case WM_LBUTTONDOWN:
        gGameMouseButtonsDown[0] = true;
        break;
    case WM_LBUTTONUP:
        gGameMouseButtonsDown[0] = false;
        break;
    case WM_RBUTTONDOWN:
        gGameMouseButtonsDown[1] = true;
        break;
    case WM_RBUTTONUP:
        gGameMouseButtonsDown[1] = false;
        break;
    case WM_MBUTTONDOWN:
        gGameMouseButtonsDown[2] = true;
        break;
    case WM_MBUTTONUP:
        gGameMouseButtonsDown[2] = false;
        break;
    case WM_XBUTTONDOWN:
        gGameMouseButtonsDown[GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4] = true;
        break;
    case WM_XBUTTONUP:
        gGameMouseButtonsDown[GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4] = false;
        break;
    default:
        break;
    }

    if (gOriginalWndProc) return CallWindowProcW(gOriginalWndProc, window, message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

// The game already received these presses; send the releases now, or the player
// keeps moving / digging while the panel is open.
void releaseGameInput(HWND window) {
    InputHandoffScope handoff; // our own guard hooks must not eat the synthetic events

    for (size_t key = 0; key < gGameKeysDown.size(); ++key) {
        if (!gGameKeysDown[key]) continue;

        auto const virtualKey = static_cast<UINT>(key);
        auto const scanCode   = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
        LPARAM     keyUp      = 1 | (static_cast<LPARAM>(scanCode) << 16) | (1LL << 30) | (1LL << 31);
        auto const systemKey  = virtualKey == VK_MENU || virtualKey == VK_LMENU || virtualKey == VK_RMENU;
        forwardToGame(window, systemKey ? WM_SYSKEYUP : WM_KEYUP, virtualKey, keyUp);
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(window, &cursor);
    auto const                    position = MAKELPARAM(cursor.x, cursor.y);
    constexpr std::array<UINT, 5> kUpMessages{WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP, WM_XBUTTONUP};
    for (size_t button = 0; button < gGameMouseButtonsDown.size(); ++button) {
        if (!gGameMouseButtonsDown[button]) continue;

        WPARAM buttonParam{};
        if (button == 3) buttonParam = MAKEWPARAM(0, XBUTTON1);
        if (button == 4) buttonParam = MAKEWPARAM(0, XBUTTON2);
        forwardToGame(window, kUpMessages[button], buttonParam, position);
    }
}

// Park the cursor in the client centre before the game grabs it again, otherwise
// the camera snaps by however far the cursor drifted across the panel.
bool confineCursorToClientCenter(HWND window) {
    RECT clientRect{};
    if (!window || !GetClientRect(window, &clientRect)) return false;

    POINT topLeft{clientRect.left, clientRect.top};
    POINT bottomRight{clientRect.right, clientRect.bottom};
    if (!ClientToScreen(window, &topLeft) || !ClientToScreen(window, &bottomRight)) return false;

    RECT const screenRect{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    if (!ClipCursor(&screenRect)) return false;
    return SetCursorPos((screenRect.left + screenRect.right) / 2, (screenRect.top + screenRect.bottom) / 2) != FALSE;
}

bool isMenuInputMessage(UINT message) {
    switch (message) {
    case WM_INPUT:
    case WM_INPUT_DEVICE_CHANGE:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
        return true;
    default:
        return false;
    }
}

LRESULT consumeMenuInputMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    // Foreground raw input must still reach DefWindowProc for User32's cleanup, but
    // it must not reach the game.
    if (message == WM_INPUT) return DefWindowProcW(window, message, wParam, lParam);
    return 1;
}

// Screenshot key, handled in both panel states. Swallowing the key pair keeps the
// game from also reacting to it (F2 is unused by Bedrock, but a rebound key might
// not be).
bool handleScreenshotKey(UINT message, WPARAM wParam) {
    auto const key = static_cast<WPARAM>(gScreenshotKey.load(std::memory_order_relaxed));
    if (key == 0 || wParam != key) return false;
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
        if (!gScreenshotKeyDown) {
            gScreenshotKeyDown = true;
            gScreenshotRequested.store(true, std::memory_order_release);
        }
        return true;
    }
    if (message == WM_KEYUP || message == WM_SYSKEYUP) {
        gScreenshotKeyDown = false;
        return true;
    }
    return false;
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kMsgRestoreGameMouse) {
        ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
        confineCursorToClientCenter(window);
        return 0;
    }

    if (message == WM_KILLFOCUS || (message == WM_ACTIVATEAPP && wParam == FALSE)) {
        gToggleKeyDown        = false;
        gConsumeEscapeRelease = false;
        gScreenshotKeyDown    = false;
        ClipCursor(nullptr);
    }

    auto const imguiReady = gImGuiReady.load(std::memory_order_acquire);
    if (!imguiReady) return forwardToGame(window, message, wParam, lParam);

    // Before the panel branches: a screenshot must work whether the panel is open
    // or closed, and the key must not leak into the panel's own hotkey capture.
    if (!gCapturingHotkey.load(std::memory_order_acquire) && handleScreenshotKey(message, wParam)) {
        return 1;
    }

    bool const visible = Overlay::getInstance().isVisible();

    if (!visible) {
        // The release of the Esc that closed the panel: swallow it, but let ImGui
        // know the key went up (it will keep believing Esc is held otherwise).
        if (gConsumeEscapeRelease && (message == WM_KEYUP || message == WM_SYSKEYUP) && wParam == VK_ESCAPE) {
            gConsumeEscapeRelease = false;
            ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
            return 1;
        }

        auto const hotkey = static_cast<WPARAM>(gHotkey.load(std::memory_order_relaxed));
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
            if (wParam == hotkey && !gToggleKeyDown) {
                gToggleKeyDown = true;
                Overlay::getInstance().toggle();
                if (Overlay::getInstance().isVisible()) releaseGameInput(window);
                return 1;
            }
        } else if (message == WM_KEYUP || message == WM_SYSKEYUP) {
            if (wParam == hotkey) {
                gToggleKeyDown = false;
                return 1;
            }
        }
        return forwardToGame(window, message, wParam, lParam);
    }

    // Panel open ------------------------------------------------------------

    ClipCursor(nullptr);

    // Rebinding: the next key press becomes the hotkey, Esc cancels. Runs before
    // everything else so the key cannot leak into ImGui or the game.
    if (gCapturingHotkey.load(std::memory_order_acquire)) {
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
            auto const code = static_cast<int>(wParam);
            if (code == VK_ESCAPE) {
                gCapturingHotkey.store(false, std::memory_order_release);
                return 1;
            }
            // A bare modifier is not a useful "open the panel" key.
            bool const modifier = code == VK_SHIFT || code == VK_LSHIFT || code == VK_RSHIFT || code == VK_CONTROL
                               || code == VK_LCONTROL || code == VK_RCONTROL || code == VK_MENU || code == VK_LMENU
                               || code == VK_RMENU || code == VK_LWIN || code == VK_RWIN;
            if (!modifier) {
                gCapturedHotkey.store(code, std::memory_order_release);
                gCapturingHotkey.store(false, std::memory_order_release);
            }
            return 1;
        }
        if (message == WM_KEYUP || message == WM_SYSKEYUP || message == WM_CHAR) return 1;
    }

    // F11 is a window action, not game input: if the panel ever draws wrong at the
    // current resolution, F11 must stay available.
    bool const keyMessage =
        message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_KEYUP || message == WM_SYSKEYUP;
    if (keyMessage && wParam == VK_F11) return forwardToGame(window, message, wParam, lParam);

    // The panel hotkey also closes the panel. The key-up has to clear the held flag
    // even while the panel is open: it used to be swallowed as generic panel input,
    // so the next press looked like keyboard auto-repeat and was ignored - that is
    // why after an Esc close the panel needed two presses of the hotkey to reopen.
    auto const hotkey = static_cast<WPARAM>(gHotkey.load(std::memory_order_relaxed));
    if (keyMessage && wParam == hotkey) {
        bool const down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        if (down) {
            if (!gToggleKeyDown) {
                gToggleKeyDown = true;
                Overlay::getInstance().setVisible(false);
            }
        } else {
            gToggleKeyDown = false;
        }
        return 1;
    }

    ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);

    if (message == WM_KEYDOWN && wParam == VK_ESCAPE) {
        // Only state here: the cursor hand-back is a window-thread action and is
        // posted from setVisible(), so it runs after this dispatch returns.
        gConsumeEscapeRelease = true;
        Overlay::getInstance().setVisible(false);
        return 1;
    }

    if (isMenuInputMessage(message)) return consumeMenuInputMessage(window, message, wParam, lParam);

    return forwardToGame(window, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Frame timing
// ---------------------------------------------------------------------------

double nowSeconds() {
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
}

void updateFrameStats() {
    double const now = nowSeconds();
    if (gLastPresentSeconds > 0.0) {
        double const delta = now - gLastPresentSeconds;
        if (delta > 0.0 && delta < 1.0) {
            {
                std::lock_guard lock(gFrameHistoryMutex);
                gFrameHistory.push_back(static_cast<float>(delta * 1000.0));
                if (gFrameHistory.size() > 240) gFrameHistory.erase(gFrameHistory.begin());
            }
            gAccumulatedSeconds += delta;
            ++gAccumulatedFrames;
            if (gAccumulatedSeconds >= 0.5) {
                gFps.store(static_cast<double>(gAccumulatedFrames) / gAccumulatedSeconds);
                gFrameMs.store(gAccumulatedSeconds * 1000.0 / static_cast<double>(gAccumulatedFrames));
                gAccumulatedSeconds = 0.0;
                gAccumulatedFrames  = 0;
            }
        }
    }
    gLastPresentSeconds = now;
    gFrameCount.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// ImGui init / rendering
// ---------------------------------------------------------------------------

void releaseGraphics() {
    if (gGraphicsReady) {
        ImGui_ImplDX11_Shutdown();
        gGraphicsReady = false;
    }
    if (gDeviceContext) {
        ID3D11RenderTargetView* empty{};
        gDeviceContext->OMSetRenderTargets(1, &empty, nullptr);
        gDeviceContext->ClearState();
        gDeviceContext->Flush();
    }
    if (gDevice11On12) gDevice11On12->Release();
    if (gDeviceContext) gDeviceContext->Release();
    if (gDevice) gDevice->Release();
    gDevice11On12  = nullptr;
    gDeviceContext = nullptr;
    gDevice        = nullptr;
}

// Bring ImGui up against the game's device. Note that it runs even while the panel
// is hidden: the first frame after a hotkey must be able to draw.
bool initializeGraphics(IDXGISwapChain* swapChain) {
    if (gImGuiReady.load(std::memory_order_acquire) && gGraphicsReady) return true;
    if (gShuttingDown.load(std::memory_order_acquire)) return false;

    releaseGraphics();

    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&gDevice)))) {
        gDevice->GetImmediateContext(&gDeviceContext);
    } else {
        // Bedrock presents through D3D12; ImGui's DX11 backend still works through
        // a D3D11On12 device bound to the game's own direct queue.
        if (!gGameQueue) return false;

        ID3D12Device* device12{};
        if (FAILED(swapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device12)))) return false;

        // Measured at 26 ms on 2026-10-23, so this is no longer worth a stage marker.
        auto const result = D3D11On12CreateDevice(
            device12,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0,
            reinterpret_cast<IUnknown**>(&gGameQueue),
            1,
            0,
            &gDevice,
            &gDeviceContext,
            nullptr
        );
        device12->Release();
        if (FAILED(result) || !gDevice) {
            releaseGraphics();
            return false;
        }
        if (FAILED(gDevice->QueryInterface(__uuidof(ID3D11On12Device), reinterpret_cast<void**>(&gDevice11On12)))) {
            releaseGraphics();
            return false;
        }
    }

    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(swapChain->GetDesc(&description))) {
        releaseGraphics();
        return false;
    }

    auto const window = description.OutputWindow ? description.OutputWindow : findGameWindow();
    if (!window || (gWindow && window != gWindow)) {
        releaseGraphics();
        return false;
    }

    if (!gImGuiReady.load(std::memory_order_acquire)) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().LogFilename = nullptr;
        theme::loadFonts();
        if (!ImGui_ImplWin32_Init(window)) {
            ImGui::DestroyContext();
            releaseGraphics();
            return false;
        }

        gWindow = window;
        gOriginalWndProc =
            reinterpret_cast<WNDPROC>(SetWindowLongPtrW(gWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(windowProc)));
        gImGuiReady.store(true, std::memory_order_release);
        logger().debug("overlay: ImGui context created on window {}", static_cast<void*>(gWindow));
    }

    if (!ImGui_ImplDX11_Init(gDevice, gDeviceContext)) {
        releaseGraphics();
        return false;
    }

    gGraphicsReady = true;
    gWarmupFrames  = kWarmupFrames;
    return true;
}

// Closing discards the panel's input state; the panel is not redrawn while hidden,
// so nothing else would clean it up.
void releaseImGuiInputState() {
    auto& io = ImGui::GetIO();
    for (bool& down : io.MouseDown) down = false;
    io.MouseWheel  = 0.0f;
    io.MouseWheelH = 0.0f;
    io.MousePos    = ImVec2(-FLT_MAX, -FLT_MAX);
    io.ClearInputKeys();
}

bool toastAlive() {
    std::lock_guard lock(gToastMutex);
    auto const      now = GetTickCount64();
    if (gToastText.empty() || now >= gToastExpireAt) {
        gToastText.clear();
        gToastExpireAt = 0;
        return false;
    }
    return true;
}

void drawToasts() {
    std::string text;
    {
        std::lock_guard lock(gToastMutex);
        if (gToastText.empty()) return;
        text = gToastText;
    }

    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
                                      | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
                                      | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                                      | ImGuiWindowFlags_AlwaysAutoResize;

    auto const viewport = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(
        ImVec2(viewport.x * 0.5f, viewport.y - 84.0f * theme::scale()),
        ImGuiCond_Always,
        ImVec2(0.5f, 1.0f)
    );
    ImGui::SetNextWindowBgAlpha(0.82f);
    if (ImGui::Begin("##neurodragonToast", nullptr, kFlags)) ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}

void drawFrame(ID3D11RenderTargetView* target) {
    gDeviceContext->OMSetRenderTargets(1, &target, nullptr);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Theme must be applied after NewFrame and before the first window: apply()
    // only reacts to viewport/scale changes, so this is cheap.
    theme::apply(ImGui::GetIO().DisplaySize);

    if (Overlay::getInstance().isVisible()) menu::render();
    drawToasts();

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    ID3D11RenderTargetView* empty{};
    gDeviceContext->OMSetRenderTargets(1, &empty, nullptr);
}

void drawOnSwapChain(IDXGISwapChain* swapChain, bool screenshot) {
    ID3D11Texture2D* backBuffer{};
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) return;

    // Capture before anything of ours is drawn, so the file is the game's finished
    // frame (docs/K §2.2 ②). The toast is the only feedback the key has: the encode
    // itself runs on a worker thread.
    if (screenshot) {
        auto const path = screenshot::save(gDevice, gDeviceContext, backBuffer, gScreenshotDirectory);
        Overlay::getInstance().notify(
            neurodragon::log::text(path.empty() ? "ui.toast.shotFailed" : "ui.toast.shotSaved")
        );
    }

    ID3D11RenderTargetView* target{};
    auto const              result = gDevice->CreateRenderTargetView(backBuffer, nullptr, &target);
    backBuffer->Release();
    if (SUCCEEDED(result) && target) {
        drawFrame(target);
        target->Release();
    }
}

// Bedrock's case: wrap the current flip-model back buffer for D3D11 use.
void drawOnWrappedBackBuffer(IDXGISwapChain* swapChain, bool screenshot) {
    UINT             index{};
    IDXGISwapChain3* swapChain3{};
    if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swapChain3)))) {
        index = swapChain3->GetCurrentBackBufferIndex();
        swapChain3->Release();
    }

    ID3D12Resource* backBuffer{};
    if (FAILED(swapChain->GetBuffer(index, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&backBuffer)))) return;

    ID3D11Resource*      wrappedBuffer{};
    D3D11_RESOURCE_FLAGS resourceFlags{D3D11_BIND_RENDER_TARGET};
    auto const           wrapResult = gDevice11On12->CreateWrappedResource(
        backBuffer,
        &resourceFlags,
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_PRESENT,
        __uuidof(ID3D11Resource),
        reinterpret_cast<void**>(&wrappedBuffer)
    );
    backBuffer->Release();
    if (FAILED(wrapResult) || !wrappedBuffer) return;

    ID3D11RenderTargetView* target{};
    if (SUCCEEDED(gDevice->CreateRenderTargetView(wrappedBuffer, nullptr, &target)) && target) {
        gDevice11On12->AcquireWrappedResources(&wrappedBuffer, 1);
        if (screenshot) {
            ID3D11Texture2D* frame = nullptr;
            if (SUCCEEDED(wrappedBuffer->QueryInterface(
                    __uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(&frame)
                ))
                && frame) {
                auto const path = screenshot::save(gDevice, gDeviceContext, frame, gScreenshotDirectory);
                Overlay::getInstance().notify(
                    neurodragon::log::text(path.empty() ? "ui.toast.shotFailed" : "ui.toast.shotSaved")
                );
                frame->Release();
            } else {
                Overlay::getInstance().notify(neurodragon::log::text("ui.toast.shotFailed"));
            }
        }
        drawFrame(target);
        gDevice11On12->ReleaseWrappedResources(&wrappedBuffer, 1);
        gDeviceContext->Flush();
        target->Release();
    }
    wrappedBuffer->Release();
}

void render(IDXGISwapChain* swapChain) {
    if (gShuttingDown.load(std::memory_order_acquire)) return;
    // Present can be called from more than one thread; one frame at a time.
    if (gRendering.exchange(true, std::memory_order_acq_rel)) return;
    struct RenderingReset {
        ~RenderingReset() { gRendering.store(false, std::memory_order_release); }
    } renderingReset;

    updateFrameStats();

    std::lock_guard lock(gResourceMutex);
    if (!initializeGraphics(swapChain)) return;

    bool const visible = Overlay::getInstance().isVisible();

    // Consumed here (not in the window proc): the render thread is the one that can
    // read the back buffer, and doing it before drawFrame keeps the panel out of
    // the file.
    bool const screenshot = gScreenshotRequested.exchange(false, std::memory_order_acq_rel);

    if (gVisibleLastFrame && !visible) releaseImGuiInputState();
    gVisibleLastFrame = visible;

    // Panel hidden and nothing to show: no ImGui frame at all (except the warm-up
    // frames that pay for the font atlas, and a screenshot request, which must not
    // be dropped just because the panel is closed).
    if (gWarmupFrames > 0) {
        --gWarmupFrames;
    } else if (!visible && !toastAlive() && !screenshot) {
        return;
    }

    ImGui::GetIO().MouseDrawCursor = visible;
    if (visible) ClipCursor(nullptr);

    // The first frame here rasterises the font atlas and uploads its texture. That
    // measured 16 ms on 2026-10-23, so no stage marker: start-up's one large stall is
    // the temporary objects in hookSwapChainEntryPoints(), which has its own stage.
    if (gDevice11On12) {
        drawOnWrappedBackBuffer(swapChain, screenshot);
    } else {
        drawOnSwapChain(swapChain, screenshot);
    }
}

// ---------------------------------------------------------------------------
// Hook install / remove
// ---------------------------------------------------------------------------

HRESULT __stdcall presentHook(IDXGISwapChain* swapChain, UINT interval, UINT flags);
HRESULT __stdcall
present1Hook(IDXGISwapChain1* swapChain, UINT interval, UINT flags, DXGI_PRESENT_PARAMETERS const* parameters);
HRESULT __stdcall
resizeBuffersHook(IDXGISwapChain* swapChain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags);
HRESULT __stdcall resizeBuffers1Hook(
    IDXGISwapChain3* swapChain,
    UINT             count,
    UINT             width,
    UINT             height,
    DXGI_FORMAT      format,
    UINT             flags,
    UINT const*      creationNodeMask,
    IUnknown* const* presentQueue
);
void __stdcall executeCommandListsHook(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);

bool installHook(void* target, void* detour, void** original) {
    return target && MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
}

void removeHook(void*& target) {
    if (!target) return;
    MH_DisableHook(target);
    MH_RemoveHook(target);
    target = nullptr;
}

// A window of our own, used only as the output of the throw-away swap chain that
// exists to hand us the DXGI vtable slots. Never shown, never presented: a hidden 8x8
// popup never enters DWM's composition path.
constexpr wchar_t kDummyWindowClass[] = L"NeuroDragonTempSwapChainWindow";

class DummyWindow {
public:
    DummyWindow() {
        static std::once_flag registered;
        std::call_once(registered, [] {
            WNDCLASSEXW description{};
            description.cbSize        = sizeof(description);
            description.lpfnWndProc   = DefWindowProcW;
            description.hInstance     = GetModuleHandleW(nullptr);
            description.lpszClassName = kDummyWindowClass;
            RegisterClassExW(&description);
        });
        window_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kDummyWindowClass,
            L"",
            WS_POPUP,
            0,
            0,
            8,
            8,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr
        );
    }

    ~DummyWindow() {
        if (window_) DestroyWindow(window_);
    }

    DummyWindow(DummyWindow const&)            = delete;
    DummyWindow& operator=(DummyWindow const&) = delete;

    [[nodiscard]] HWND get() const { return window_; }

private:
    HWND window_{};
};

// The throw-away D3D11 device and swap chain used only when D3D12 is unavailable.
// Kept in one place so the objects are released in the right order.
struct TempSwapChain {
    ID3D11Device*        device{};
    ID3D11DeviceContext* context{};
    IDXGISwapChain*      swapChain{};

    TempSwapChain() = default;
    ~TempSwapChain() { release(); }
    TempSwapChain(TempSwapChain const&)            = delete;
    TempSwapChain& operator=(TempSwapChain const&) = delete;

    void release() {
        if (swapChain) swapChain->Release();
        if (context) context->Release();
        if (device) device->Release();
        swapChain = nullptr;
        context   = nullptr;
        device    = nullptr;
    }
};

bool createTempSwapChain(HWND outputWindow, TempSwapChain& temp) {
    RECT clientRect{};
    if (!GetClientRect(outputWindow, &clientRect)) return false;

    // The client area can still be 0x0 right after window creation; a swap chain of
    // that size fails to create, so fall back to a minimum.
    auto const width  = static_cast<UINT>(std::max<LONG>(clientRect.right - clientRect.left, 8));
    auto const height = static_cast<UINT>(std::max<LONG>(clientRect.bottom - clientRect.top, 8));

    D3D_FEATURE_LEVEL featureLevels[]{D3D_FEATURE_LEVEL_11_0};

    auto const deviceResult = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        featureLevels,
        1,
        D3D11_SDK_VERSION,
        &temp.device,
        nullptr,
        &temp.context
    );
    if (FAILED(deviceResult) || !temp.device) {
        temp.release();
        return false;
    }

    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount       = 1;
    description.BufferDesc.Width  = width;
    description.BufferDesc.Height = height;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow      = outputWindow;
    description.SampleDesc.Count  = 1;
    description.Windowed          = TRUE;
    description.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    IDXGIFactory1* factory{};
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
        temp.release();
        return false;
    }

    auto const chainResult = factory->CreateSwapChain(temp.device, &description, &temp.swapChain);
    factory->Release();
    if (FAILED(chainResult) || !temp.swapChain) {
        temp.release();
        return false;
    }
    return true;
}

// The throw-away D3D12 device + direct queue. Kept alive while the flip chain below
// and the ExecuteCommandLists hook need them, then released in order.
struct TempD3D12 {
    ID3D12Device*       device{};
    ID3D12CommandQueue* queue{};

    TempD3D12() = default;
    ~TempD3D12() { release(); }
    TempD3D12(TempD3D12 const&)            = delete;
    TempD3D12& operator=(TempD3D12 const&) = delete;

    void release() {
        if (queue) queue->Release();
        if (device) device->Release();
        queue  = nullptr;
        device = nullptr;
    }
};

bool createTempD3D12(TempD3D12& temp) {
    auto const result = D3D12CreateDevice(
        nullptr,
        D3D_FEATURE_LEVEL_11_0,
        __uuidof(ID3D12Device),
        reinterpret_cast<void**>(&temp.device)
    );
    if (FAILED(result) || !temp.device) {
        temp.release();
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC description{};
    description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(temp.device->CreateCommandQueue(
            &description,
            __uuidof(ID3D12CommandQueue),
            reinterpret_cast<void**>(&temp.queue)
        ))
        || !temp.queue) {
        temp.release();
        return false;
    }
    return true;
}

// The game's D3D12 direct queue is what D3D11On12CreateDevice needs; the vtable of
// our throw-away queue is enough to hook it.
bool hookCommandQueue(TempD3D12& temp) {
    if (gOriginalExecuteCommandLists) return true;
    gExecuteCommandListsTarget = (*reinterpret_cast<void***>(temp.queue))[kExecuteCommandListsVtableIndex];
    return installHook(
        gExecuteCommandListsTarget,
        reinterpret_cast<void*>(executeCommandListsHook),
        reinterpret_cast<void**>(&gOriginalExecuteCommandLists)
    );
}

// A flip-model chain on our hidden window, only ever read for the IDXGISwapChain
// vtable slots. Same chain class the game presents with, and - unlike the D3D11
// bitblt chain this replaced - creating and releasing it is free in this process.
bool createTempFlipChain(ID3D12CommandQueue* queue, HWND outputWindow, IDXGISwapChain** chain) {
    IDXGIFactory2* factory{};
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory))) || !factory) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width            = 8;
    description.Height           = 8;
    description.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount      = 2;
    description.SampleDesc.Count = 1;
    description.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* swapChain1{};
    auto const       result =
        factory->CreateSwapChainForHwnd(queue, outputWindow, &description, nullptr, nullptr, &swapChain1);
    factory->Release();
    if (FAILED(result) || !swapChain1) return false;
    *chain = swapChain1;
    return true;
}

// The five entry points taken from a live chain. Both the D3D12 primary path and the
// D3D11 fallback feed this, so whatever the source, the hooked behaviour is the same.
bool installSwapChainHooks(IDXGISwapChain* swapChain) {
    if (gOriginalPresent && gOriginalPresent1 && gOriginalResizeBuffers && gOriginalResizeBuffers1) return true;

    auto** vtable = *reinterpret_cast<void***>(swapChain);
    bool   ok     = installHook(
                  vtable[kPresentVtableIndex],
                  reinterpret_cast<void*>(presentHook),
                  reinterpret_cast<void**>(&gOriginalPresent)
              )
           && installHook(
                  vtable[kResizeBuffersVtableIndex],
                  reinterpret_cast<void*>(resizeBuffersHook),
                  reinterpret_cast<void**>(&gOriginalResizeBuffers)
           );
    gPresentTarget       = vtable[kPresentVtableIndex];
    gResizeBuffersTarget = vtable[kResizeBuffersVtableIndex];

    IDXGISwapChain1* swapChain1{};
    if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swapChain1)))) {
        gPresent1Target = (*reinterpret_cast<void***>(swapChain1))[kPresent1VtableIndex];
        ok              = installHook(
                 gPresent1Target,
                 reinterpret_cast<void*>(present1Hook),
                 reinterpret_cast<void**>(&gOriginalPresent1)
             )
          && ok;
        swapChain1->Release();
    }

    IDXGISwapChain3* swapChain3{};
    if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swapChain3)))) {
        gResizeBuffers1Target = (*reinterpret_cast<void***>(swapChain3))[kResizeBuffers1VtableIndex];
        ok                    = installHook(
                 gResizeBuffers1Target,
                 reinterpret_cast<void*>(resizeBuffers1Hook),
                 reinterpret_cast<void**>(&gOriginalResizeBuffers1)
             )
          && ok;
        swapChain3->Release();
    }
    return ok;
}

bool hookSwapChainEntryPoints(HWND gameWindow) {
    // The five DXGI entry points come from a throw-away chain. Measured 2026-10-23,
    // when that chain was a D3D11 device + bitblt chain: device 112 ms, chain 25 ms,
    // hooks 232 ms - yet the step read 3861 ms (and 3519 / 4869 ms in two earlier
    // runs). What cost the seconds was the *teardown* of those temporary D3D11
    // objects, which sits in this same scope. A D3D12 device + queue costs 0-123 ms
    // to create and 62 ms for chain+device+queue release, and the queue is needed for
    // the ExecuteCommandLists hook anyway, so the vtable now comes from a flip chain
    // on a hidden window: the step is 375 ms, 217 ms of it MinHook's thread
    // suspension. No stage pair of its own - the caller's "installing the in-game
    // panel" marker covers it since 2026-10-23. The D3D11 chain stays only as the
    // fallback.
    DummyWindow output{};

    TempD3D12 d3d12{};
    if (createTempD3D12(d3d12)) {
        if (!hookCommandQueue(d3d12)) {
            logger().warn("overlay: D3D12 queue hook failed; D3D11On12 rendering may be unavailable");
        }

        IDXGISwapChain* chain{};
        if (output.get() && createTempFlipChain(d3d12.queue, output.get(), &chain)) {
            bool const ok = installSwapChainHooks(chain);
            chain->Release();
            d3d12.release();
            return ok;
        }
        d3d12.release();
    }

    // Fallback: a D3D11 device plus a bitblt chain. Its teardown is what made this
    // step cost seconds on 2026-10-23, so it only runs when D3D12 or the hidden flip
    // chain is unavailable.
    TempSwapChain temp{};
    bool const    onOwnWindow = output.get() && createTempSwapChain(output.get(), temp);
    if (!onOwnWindow && (!gameWindow || !createTempSwapChain(gameWindow, temp))) {
        logger().warn("overlay: temporary swap chain creation failed; retry on the next frame");
        return false;
    }

    bool const ok = installSwapChainHooks(temp.swapChain);
    temp.release();
    return ok;
}

// ---------------------------------------------------------------------------
// Hook bodies
// ---------------------------------------------------------------------------

HRESULT __stdcall presentHook(IDXGISwapChain* swapChain, UINT interval, UINT flags) {
    render(swapChain);
    return gOriginalPresent(swapChain, interval, flags);
}

HRESULT __stdcall
present1Hook(IDXGISwapChain1* swapChain, UINT interval, UINT flags, DXGI_PRESENT_PARAMETERS const* parameters) {
    render(static_cast<IDXGISwapChain*>(swapChain));
    return gOriginalPresent1(swapChain, interval, flags, parameters);
}

HRESULT __stdcall
resizeBuffersHook(IDXGISwapChain* swapChain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    std::unique_lock lock(gResourceMutex);
    // ResizeBuffers requires every reference to the old back buffers to be gone.
    releaseGraphics();
    lock.unlock();
    return gOriginalResizeBuffers(swapChain, count, width, height, format, flags);
}

HRESULT __stdcall resizeBuffers1Hook(
    IDXGISwapChain3* swapChain,
    UINT             count,
    UINT             width,
    UINT             height,
    DXGI_FORMAT      format,
    UINT             flags,
    UINT const*      creationNodeMask,
    IUnknown* const* presentQueue
) {
    std::unique_lock lock(gResourceMutex);
    releaseGraphics();
    lock.unlock();
    return gOriginalResizeBuffers1(swapChain, count, width, height, format, flags, creationNodeMask, presentQueue);
}

void __stdcall executeCommandListsHook(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    if (!gGameQueue && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        std::lock_guard lock(gResourceMutex);
        if (!gGameQueue) {
            gGameQueue = queue;
            gGameQueue->AddRef();
        }
    }
    gOriginalExecuteCommandLists(queue, count, lists);
}

} // namespace

// ---------------------------------------------------------------------------
// Overlay
// ---------------------------------------------------------------------------

void* overlayGameQueue() { return gGameQueue; }

Overlay& Overlay::getInstance() {
    static Overlay instance;
    return instance;
}

bool Overlay::install() {
    if (gInstalled.load(std::memory_order_acquire)) return true;

    auto const window = findGameWindow();
    if (!window) {
        logger().warn("overlay: game window not found yet; will retry");
        return false;
    }

    // Everything below creates D3D devices and patches entry points: 523 ms in the
    // 2026-10-23 log, 375 ms of it the temporary objects in
    // hookSwapChainEntryPoints(), which has its own (nested) stage pair - down from
    // 3774 ms before that step stopped destroying a temporary D3D11 device.
    log::Stage stage{"log.stage.overlay"};

    auto const status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        logger().error("overlay: MinHook initialization failed ({})", static_cast<int>(status));
        stage.markFailed();
        return false;
    }

    gShuttingDown.store(false, std::memory_order_release);

    // The hotkey comes from config.json, which was imported during load().
    refreshHotkey();

    // This also installs the D3D12 command-queue hook: both need the same temporary
    // device and queue, so they are created once inside that step.
    if (!hookSwapChainEntryPoints(window)) {
        logger().warn("overlay: DXGI hooks could not be installed; will retry");
        stage.markFailed();
        uninstall();
        return false;
    }

    auto const guard = installInputGuard();
    if (!guard.mouseHookInstalled) logger().warn("overlay: mouse input guard failed");
    if (!guard.keyDownHookInstalled) logger().warn("overlay: key-down guard failed");
    if (!guard.keyUpHookInstalled) logger().warn("overlay: key-up guard failed");

    gInstalled.store(true, std::memory_order_release);
    logger().debug("overlay: ready (VK {:#x} opens the panel)", gHotkey.load(std::memory_order_relaxed));
    return true;
}

bool Overlay::isInstalled() const { return gInstalled.load(std::memory_order_acquire); }

void Overlay::uninstall() {
    bool const wasInstalled = gInstalled.load(std::memory_order_acquire);

    // Persist whatever the panel left unsaved before tearing anything down.
    if (gVisible.load(std::memory_order_acquire)) menu::onClosed();

    gShuttingDown.store(true, std::memory_order_release);
    gVisible.store(false, std::memory_order_release);
    ClipCursor(nullptr);
    cancelHotkeyCapture();

    removeHook(gExecuteCommandListsTarget);
    removeHook(gResizeBuffers1Target);
    removeHook(gPresent1Target);
    removeHook(gResizeBuffersTarget);
    removeHook(gPresentTarget);

    if (gOriginalWndProc && gWindow && IsWindow(gWindow)) {
        SetWindowLongPtrW(gWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(gOriginalWndProc));
    }
    gOriginalWndProc = nullptr;

    uninstallInputGuard();

    {
        std::lock_guard lock(gResourceMutex);
        releaseGraphics();
        if (gImGuiReady.load(std::memory_order_acquire)) {
            ImGui_ImplWin32_Shutdown();
            theme::reset();
            ImGui::DestroyContext();
            gImGuiReady.store(false, std::memory_order_release);
        }
        if (gGameQueue) {
            gGameQueue->Release();
            gGameQueue = nullptr;
        }
    }

    {
        std::lock_guard lock(gToastMutex);
        gToastText.clear();
        gToastExpireAt = 0;
    }

    gWindow               = nullptr;
    gVisibleLastFrame     = false;
    gConsumeEscapeRelease = false;
    gToggleKeyDown        = false;
    gScreenshotKeyDown    = false;
    gScreenshotRequested.store(false, std::memory_order_release);
    screenshot::releaseCache();
    gGameKeysDown.fill(false);
    gGameMouseButtonsDown.fill(false);

    gInstalled.store(false, std::memory_order_release);
    if (wasInstalled) logger().debug("overlay: uninstalled");
}

bool Overlay::isVisible() const { return gVisible.load(std::memory_order_acquire); }

void Overlay::setVisible(bool visible) {
    if (gVisible.exchange(visible, std::memory_order_acq_rel) == visible) return;

    if (visible) menu::onOpened();
    else menu::onClosed();

    // Keep owning the input for a short moment after either transition, so the key
    // that triggered it never leaks into the game.
    setOverlayInputCapture(visible);

    if (!visible && gWindow) {
        // Hand the cursor back on the window thread; doing it here would re-enter
        // the input system from inside a message dispatch.
        PostMessageW(gWindow, kMsgRestoreGameMouse, 0, 0);
    }
}

void Overlay::toggle() { setVisible(!isVisible()); }

void Overlay::notify(std::string message) {
    if (message.empty()) return;
    std::lock_guard lock(gToastMutex);
    gToastText     = std::move(message);
    gToastExpireAt = GetTickCount64() + kToastLifetimeMs;
}

int Overlay::hotkey() const { return gHotkey.load(std::memory_order_relaxed); }

void Overlay::refreshHotkey() {
    int key = config::get().ui.openKey;
    if (key <= 0 || key > 0xFF) key = VK_END;
    gHotkey.store(key, std::memory_order_release);

    int shot = config::get().ui.screenshotKey;
    if (shot <= 0 || shot > 0xFF) shot = VK_F2;
    gScreenshotKey.store(shot, std::memory_order_release);
}

int Overlay::screenshotKey() const { return gScreenshotKey.load(std::memory_order_relaxed); }

void Overlay::setScreenshotDirectory(std::wstring directory) { gScreenshotDirectory = std::move(directory); }

void Overlay::beginHotkeyCapture() {
    gCapturedHotkey.store(0, std::memory_order_release);
    gCapturingHotkey.store(true, std::memory_order_release);
}

void Overlay::cancelHotkeyCapture() { gCapturingHotkey.store(false, std::memory_order_release); }

bool Overlay::isCapturingHotkey() const { return gCapturingHotkey.load(std::memory_order_acquire); }

int Overlay::takeCapturedHotkey() { return gCapturedHotkey.exchange(0, std::memory_order_acq_rel); }

FrameStats Overlay::frameStats() const {
    FrameStats stats;
    stats.fps     = gFps.load(std::memory_order_relaxed);
    stats.frameMs = gFrameMs.load(std::memory_order_relaxed);
    stats.frames  = gFrameCount.load(std::memory_order_relaxed);
    return stats;
}

std::vector<float> Overlay::recentFrameMs() const {
    std::lock_guard lock(gFrameHistoryMutex);
    return gFrameHistory;
}

} // namespace neurodragon::ui
