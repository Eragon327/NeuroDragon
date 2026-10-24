#include "neurodragon/ui/Screenshot.h"

#include "neurodragon/Log.h"

#include <Windows.h>
#include <objbase.h>

#include <d3d11.h>
#include <wincodec.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace neurodragon::ui::screenshot {
namespace {

// IEEE 754 binary16 -> float. Subnormals go through float math (simpler than
// renormalising the exponent by hand); normals/inf/nan are bit-copied.
float halfToFloat(std::uint16_t h) {
    std::uint32_t const sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
    std::uint32_t const exp  = (h >> 10) & 0x1Fu;
    std::uint32_t const mant = h & 0x3FFu;
    if (exp == 0) {
        float const value = (static_cast<float>(mant) / 1024.0f) * std::ldexp(1.0f, -14);
        return (h & 0x8000u) ? -value : value;
    }
    std::uint32_t bits = sign;
    if (exp == 31) {
        bits |= 0x7F800000u | (mant << 13);
    } else {
        bits |= ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

void shotLog(int level, char const* format, ...) {
    char    buffer[1024]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    neurodragon::log::write(level, buffer);
}

// One staging texture, reused across captures. Present thread only (the same
// thread the overlay draws on), so no locking.
struct Cache {
    ID3D11Texture2D* staging = nullptr;
    UINT             width   = 0;
    UINT             height  = 0;
    DXGI_FORMAT      format  = DXGI_FORMAT_UNKNOWN;
};
Cache gCache;

bool ensureStaging(ID3D11Device* device, D3D11_TEXTURE2D_DESC const& desc) {
    if (gCache.staging && gCache.width == desc.Width && gCache.height == desc.Height && gCache.format == desc.Format) {
        return true;
    }
    if (gCache.staging) {
        gCache.staging->Release();
        gCache.staging = nullptr;
    }
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage                 = D3D11_USAGE_STAGING;
    staging.BindFlags             = 0;
    staging.CPUAccessFlags        = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags             = 0;
    staging.MipLevels             = 1;
    staging.ArraySize             = 1;
    staging.SampleDesc            = {1, 0};
    if (FAILED(device->CreateTexture2D(&staging, nullptr, &gCache.staging)) || !gCache.staging) {
        gCache.staging = nullptr;
        return false;
    }
    gCache.width  = desc.Width;
    gCache.height = desc.Height;
    gCache.format = desc.Format;
    return true;
}

std::uint8_t toByte(float value) {
    if (!(value > 0.0f)) {
        return 0;
    }
    if (value > 1.0f) {
        value = 1.0f;
    }
    return static_cast<std::uint8_t>(value * 255.0f + 0.5f);
}

// Linear HDR (a real HDR swap chain) -> display-referred 8-bit, using the same
// Reinhard + gamma 2.2 curve the NR encoder and the TEST dumps use. The SDR case
// (B8G8R8A8 / R8G8B8A8) is a plain copy; a 10-bit scRGB chain is treated as
// display-referred for now (logged once, so a washed-out capture is explainable).
void toBgra8(
    std::uint8_t const*    source,
    UINT                   sourcePitch,
    UINT                   width,
    UINT                   height,
    DXGI_FORMAT            format,
    std::vector<std::uint8_t>& out
) {
    out.resize(static_cast<std::size_t>(width) * height * 4u);
    for (UINT y = 0; y < height; ++y) {
        auto const* row  = source + static_cast<std::size_t>(sourcePitch) * y;
        auto*       dest = out.data() + static_cast<std::size_t>(width) * 4u * y;
        switch (format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            std::memcpy(dest, row, static_cast<std::size_t>(width) * 4u);
            break;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            for (UINT x = 0; x < width; ++x) {
                dest[x * 4 + 0] = row[x * 4 + 2];
                dest[x * 4 + 1] = row[x * 4 + 1];
                dest[x * 4 + 2] = row[x * 4 + 0];
                dest[x * 4 + 3] = row[x * 4 + 3];
            }
            break;
        case DXGI_FORMAT_R10G10B10A2_UNORM: {
            auto const* pixels = reinterpret_cast<std::uint32_t const*>(row);
            for (UINT x = 0; x < width; ++x) {
                std::uint32_t const p = pixels[x];
                dest[x * 4 + 0] = static_cast<std::uint8_t>(((p >> 20) & 0x3FF) >> 2); // B
                dest[x * 4 + 1] = static_cast<std::uint8_t>(((p >> 10) & 0x3FF) >> 2); // G
                dest[x * 4 + 2] = static_cast<std::uint8_t>((p & 0x3FF) >> 2);        // R
                dest[x * 4 + 3] = static_cast<std::uint8_t>(((p >> 30) & 0x3) * 85);
            }
            break;
        }
        case DXGI_FORMAT_R16G16B16A16_FLOAT: {
            auto const* pixels = reinterpret_cast<std::uint16_t const*>(row);
            for (UINT x = 0; x < width; ++x) {
                float const r = halfToFloat(pixels[x * 4 + 0]);
                float const g = halfToFloat(pixels[x * 4 + 1]);
                float const b = halfToFloat(pixels[x * 4 + 2]);
                auto const  tone = [](float v) {
                    float const t = (v > 0.0f ? v : 0.0f) / (1.0f + (v > 0.0f ? v : 0.0f));
                    return toByte(std::pow(t, 1.0f / 2.2f));
                };
                dest[x * 4 + 0] = tone(b);
                dest[x * 4 + 1] = tone(g);
                dest[x * 4 + 2] = tone(r);
                dest[x * 4 + 3] = 255;
            }
            break;
        }
        default:
            std::memset(dest, 0, static_cast<std::size_t>(width) * 4u);
            break;
        }
    }
}

// PNG through WIC. COM is initialised per call because the Present thread's
// apartment state is the game's choice; RPC_E_CHANGED_MODE means it is already
// initialised and CoCreateInstance still works.
bool writePng(std::wstring const& path, UINT width, UINT height, std::vector<std::uint8_t> const& bgra) {
    HRESULT const com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool const    comBalanced = SUCCEEDED(com);

    IWICImagingFactory*  factory = nullptr;
    IWICStream*          stream  = nullptr;
    IWICBitmapEncoder*   encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = false;

    HRESULT hr = ::CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)
    );
    if (SUCCEEDED(hr) && factory) {
        hr = factory->CreateStream(&stream);
    }
    if (SUCCEEDED(hr) && stream) {
        hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    }
    if (SUCCEEDED(hr) && stream) {
        hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    }
    if (SUCCEEDED(hr) && encoder) {
        hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(hr) && encoder) {
        hr = encoder->CreateNewFrame(&frame, nullptr);
    }
    if (SUCCEEDED(hr) && frame) {
        hr = frame->Initialize(nullptr);
    }
    if (SUCCEEDED(hr) && frame) {
        hr = frame->SetSize(width, height);
    }
    if (SUCCEEDED(hr) && frame) {
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        hr                        = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr) && !::IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
            shotLog(1, "[shot] WIC 不接受 BGRA8，实际格式被改为别的（放弃这一张）");
            hr = E_FAIL;
        }
    }
    if (SUCCEEDED(hr) && frame) {
        hr = frame->WritePixels(
            height,
            width * 4u,
            static_cast<UINT>(bgra.size()),
            const_cast<BYTE*>(bgra.data())
        );
    }
    if (SUCCEEDED(hr) && frame) {
        hr = frame->Commit();
    }
    if (SUCCEEDED(hr) && encoder) {
        hr = encoder->Commit();
    }
    ok = SUCCEEDED(hr);
    if (!ok) {
        shotLog(1, "[shot] PNG 写入失败 0x%08X（%ls）", static_cast<unsigned>(hr), path.c_str());
    }

    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (comBalanced) ::CoUninitialize();
    return ok;
}

std::wstring makePath(std::wstring const& directory) {
    SYSTEMTIME local{};
    ::GetLocalTime(&local);
    wchar_t name[96]{};
    static unsigned counter = 0;
    swprintf_s(
        name,
        sizeof(name) / sizeof(name[0]),
        L"%04u%02u%02u-%02u%02u%02u-%03u-%u.png",
        local.wYear,
        local.wMonth,
        local.wDay,
        local.wHour,
        local.wMinute,
        local.wSecond,
        local.wMilliseconds,
        counter++
    );
    std::filesystem::path path = directory;
    path /= name;
    // Absolute, because this string is also what the player is shown in chat
    // (ui::notice::chat) and logged: `./mods/...` only means something relative to
    // the game's working directory. The mod directory itself comes from LeviLamina
    // as a relative path (latest.log proves it: "./mods/NeuroDragon\screenshots\…
    // .png"), so the resolution happens here, where the file is also written.
    std::error_code error;
    auto const      absolute = std::filesystem::absolute(path, error);
    return (error ? path : absolute).wstring();
}

} // namespace

// The PNG encode of a 2560x1440 frame measured ~300 ms of the original 395 ms
// capture (Map + convert are ~20 ms, disk ~20 ms), which is a visible freeze on
// the Present thread. One worker thread owns the encode: save() only does the GPU
// readback + BGRA conversion and hands the pixels over.
namespace {
struct Job {
    std::wstring              path;
    UINT                      width  = 0;
    UINT                      height = 0;
    std::vector<std::uint8_t> pixels;
};

std::mutex                 gWorkerMutex;
std::condition_variable    gWorkerCv;
std::deque<Job>            gJobs;
HANDLE                     gWorkerThread = nullptr;
bool                       gWorkerStop   = false;

void workerMain() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(gWorkerMutex);
            gWorkerCv.wait(lock, [] { return gWorkerStop || !gJobs.empty(); });
            if (gJobs.empty()) {
                if (gWorkerStop) return;
                continue;
            }
            job = std::move(gJobs.front());
            gJobs.pop_front();
        }
        auto const started = std::chrono::steady_clock::now();
        bool const ok      = writePng(job.path, job.width, job.height, job.pixels);
        double const ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (ok) {
            shotLog(
                0,
                "[shot] 已写入 %ls（%ux%u，编码 %.1f ms，%.1f MB）",
                job.path.c_str(),
                job.width,
                job.height,
                ms,
                static_cast<double>(job.pixels.size()) / (1024.0 * 1024.0)
            );
        }
    }
}

DWORD WINAPI workerThread(void*) {
    workerMain();
    return 0;
}

bool startWorker() {
    if (gWorkerThread) {
        return true;
    }
    gWorkerStop   = false;
    gWorkerThread = ::CreateThread(nullptr, 0, workerThread, nullptr, 0, nullptr);
    return gWorkerThread != nullptr;
}

bool enqueue(Job&& job) {
    {
        std::lock_guard lock(gWorkerMutex);
        if (!startWorker()) {
            return false;
        }
        gJobs.push_back(std::move(job));
    }
    gWorkerCv.notify_one();
    return true;
}

} // namespace

std::wstring
save(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* buffer, std::wstring const& directory) {
    if (!device || !context || !buffer || directory.empty()) {
        return {};
    }
    auto const started = std::chrono::steady_clock::now();

    D3D11_TEXTURE2D_DESC desc{};
    buffer->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.ArraySize != 1) {
        shotLog(1, "[shot] 后缓冲不是单采样 2D 纹理（msaa=%u mips=%u arr=%u），跳过", desc.SampleDesc.Count, desc.MipLevels, desc.ArraySize);
        return {};
    }
    if (!ensureStaging(device, desc)) {
        shotLog(1, "[shot] staging 纹理创建失败（%ux%u fmt=%u）", desc.Width, desc.Height, static_cast<unsigned>(desc.Format));
        return {};
    }

    context->CopyResource(gCache.staging, buffer);
    // D3D11On12 submits the immediate context's commands on Flush; the Map below
    // waits for the copy, so the submission has to happen first.
    context->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(gCache.staging, 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
        shotLog(1, "[shot] 读回失败（Map）");
        return {};
    }
    std::vector<std::uint8_t> bgra;
    toBgra8(
        static_cast<std::uint8_t const*>(mapped.pData),
        mapped.RowPitch,
        desc.Width,
        desc.Height,
        desc.Format,
        bgra
    );
    context->Unmap(gCache.staging, 0);

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    std::wstring const path = makePath(directory);

    Job job;
    job.path   = path;
    job.width  = desc.Width;
    job.height = desc.Height;
    job.pixels = std::move(bgra);
    if (!enqueue(std::move(job))) {
        shotLog(1, "[shot] 编码线程启动失败，这一张丢弃");
        return {};
    }

    double const ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    shotLog(
        0,
        "[shot] 已排队 %ls（%ux%u，格式 %u，读回+转换 %.1f ms，编码在后台）",
        path.c_str(),
        desc.Width,
        desc.Height,
        static_cast<unsigned>(desc.Format),
        ms
    );
    return path;
}

void releaseCache() {
    {
        std::unique_lock lock(gWorkerMutex);
        // Finish whatever is already queued, then stop: a written file must not be
        // abandoned on unload, and the thread must not outlive this module.
        gWorkerCv.wait(lock, [] { return gJobs.empty(); });
        if (gWorkerThread) {
            gWorkerStop = true;
            lock.unlock();
            gWorkerCv.notify_all();
            ::WaitForSingleObject(gWorkerThread, 5000);
            ::CloseHandle(gWorkerThread);
            gWorkerThread = nullptr;
        }
    }
    if (gCache.staging) {
        gCache.staging->Release();
        gCache.staging = nullptr;
    }
    gCache.width  = 0;
    gCache.height = 0;
    gCache.format = DXGI_FORMAT_UNKNOWN;
}

} // namespace neurodragon::ui::screenshot
