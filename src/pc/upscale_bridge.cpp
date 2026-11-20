#include "pc/upscale_bridge.h"

#include <dxgi1_2.h>

#include <cstdio>
#include <vector>

namespace xport::pc {

using Microsoft::WRL::ComPtr;

std::wstring upscale_shared_name(DWORD process, const wchar_t* what) {
    wchar_t name[96];
    std::swprintf(name, 96, L"Local\\xport-upscale-%lu-%ls", static_cast<unsigned long>(process), what);
    return name;
}

UpscaleBridge::~UpscaleBridge() { stop(); }

bool UpscaleBridge::fail(const char* what, HRESULT hr) {
    char text[160];
    std::snprintf(text, sizeof(text), hr == S_OK ? "%s" : "%s (%08lX)", what, static_cast<unsigned long>(hr));
    error_ = text;
    stop();
    return false;
}

HRESULT UpscaleBridge::share(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, UINT bind,
                             const wchar_t* what, Shared& shared) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = bind;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &shared.texture);
    ComPtr<IDXGIResource1> resource;
    if (SUCCEEDED(hr)) hr = shared.texture.As(&resource);
    if (SUCCEEDED(hr)) {
        hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                          upscale_shared_name(GetCurrentProcessId(), what).c_str(), &shared.handle);
    }
    if (SUCCEEDED(hr)) hr = shared.texture.As(&shared.lock);
    return hr;
}

bool UpscaleBridge::start(ID3D11Device* device, const std::wstring& helper, uint32_t input_width, uint32_t input_height,
                          uint32_t output_width, uint32_t output_height, const char* mode, DWORD timeout_ms) {
    stop();
    error_.clear();
    const DWORD self = GetCurrentProcessId();
    constexpr UINT kRead = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (const HRESULT hr = share(device, input_width, input_height, DXGI_FORMAT_B8G8R8A8_UNORM, kRead, L"colour", colour_shared_);
        FAILED(hr)) {
        return fail("cannot share the colour texture", hr);
    }
    if (const HRESULT hr = share(device, input_width, input_height, DXGI_FORMAT_R32_FLOAT, kRead, L"depth", depth_shared_);
        FAILED(hr)) {
        return fail("cannot share the depth texture", hr);
    }
    if (const HRESULT hr = share(device, input_width, input_height, DXGI_FORMAT_R16G16_FLOAT, kRead, L"motion", motion_shared_);
        FAILED(hr)) {
        return fail("cannot share the motion texture", hr);
    }
    // The output is written by DLSS as an unordered-access view: RGBA8, which every card can store to.
    if (const HRESULT hr = share(device, output_width, output_height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 kRead | D3D11_BIND_UNORDERED_ACCESS, L"output", output_shared_);
        FAILED(hr)) {
        return fail("cannot share the output texture", hr);
    }
    frame_mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(UpscaleFrame),
                                        upscale_shared_name(self, L"frame").c_str());
    if (frame_mapping_) frame_ = static_cast<UpscaleFrame*>(MapViewOfFile(frame_mapping_, FILE_MAP_WRITE, 0, 0, sizeof(UpscaleFrame)));
    if (!frame_) return fail("cannot share the frame's data", HRESULT_FROM_WIN32(GetLastError()));
    *frame_ = UpscaleFrame{};
    frames_ = 0;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapter_desc{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&adapter_desc))) {
        return fail("cannot tell the graphics card");
    }
    ready_ = CreateEventW(nullptr, TRUE, FALSE, upscale_shared_name(self, L"ready").c_str());
    if (!ready_) return fail("cannot make the ready event", HRESULT_FROM_WIN32(GetLastError()));
    // The helper: the port's process (its names, and it ends with it), the card (LUID), the two sizes, the mode.
    wchar_t arguments[512];
    std::swprintf(arguments, 512, L"\"%ls\" %lu %08lX:%08lX %u %u %u %u %hs", helper.c_str(), static_cast<unsigned long>(self),
                  static_cast<unsigned long>(adapter_desc.AdapterLuid.HighPart),
                  static_cast<unsigned long>(adapter_desc.AdapterLuid.LowPart), input_width, input_height, output_width,
                  output_height, mode);
    // What the helper writes (stdout and stderr) comes back through a pipe, line by line, to set_log's function: the
    // port's log. Only the pipe's end is handed to it, not every handle the port could pass on.
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE log_write = nullptr;
    if (!CreatePipe(&log_read_, &log_write, &inherit, 0)) return fail("cannot make the log pipe", HRESULT_FROM_WIN32(GetLastError()));
    SetHandleInformation(log_read_, HANDLE_FLAG_INHERIT, 0);
    SIZE_T list_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
    std::vector<char> list_memory(list_size);
    const auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_memory.data());
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = log_write;
    startup.lpAttributeList = list;
    const bool listed = InitializeProcThreadAttributeList(list, 1, 0, &list_size) &&
                        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &log_write, sizeof(log_write),
                                                  nullptr, nullptr);
    // Suspended until it is in the job below: no moment when the port could end and leave it running.
    const bool started = listed && CreateProcessW(helper.c_str(), arguments, nullptr, nullptr, TRUE,
                                                  CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                                                  nullptr, nullptr, &startup.StartupInfo, &process_);
    const DWORD start_error = GetLastError();
    if (listed) DeleteProcThreadAttributeList(list);
    CloseHandle(log_write);  // the helper's copy is the only one: the pipe ends with it
    if (!started) return fail("cannot start the helper", HRESULT_FROM_WIN32(start_error));
    // A job that ends the helper when the port's process ends however it ends (a crash too): Windows closes the job's
    // handle with the process.
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job_, process_.hProcess)) {
        return fail("cannot tie the helper to the port", HRESULT_FROM_WIN32(GetLastError()));
    }
    ResumeThread(process_.hThread);
    log_thread_ = std::thread([this] {
        char buffer[512];
        std::string line;
        DWORD read = 0;
        const auto emit = [&] {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty() && log_) log_(line.c_str());
            line.clear();
        };
        while (ReadFile(log_read_, buffer, sizeof(buffer), &read, nullptr) && read) {
            for (DWORD i = 0; i < read; ++i) {
                if (buffer[i] == '\n') emit();
                else line += buffer[i];
            }
        }
        emit();
    });
    const HANDLE waits[2] = {ready_, process_.hProcess};
    const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, timeout_ms);
    if (woke != WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(process_.hProcess, &code);
        char text[96];
        std::snprintf(text, sizeof(text), woke == WAIT_OBJECT_0 + 1 ? "the helper ended (code %lu)" : "the helper did not answer",
                      static_cast<unsigned long>(code));
        return fail(text);
    }
    return true;
}

void UpscaleBridge::stop() {
    if (process_.hProcess) {
        // Gone for good before the names are given again: while it still holds the shared objects, a new start
        // cannot make objects of the same names.
        if (WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(process_.hProcess, 0);
        WaitForSingleObject(process_.hProcess, 5000);
        CloseHandle(process_.hProcess);
        CloseHandle(process_.hThread);
        process_ = {};
    }
    if (job_) CloseHandle(job_);
    job_ = nullptr;
    if (log_thread_.joinable()) log_thread_.join();  // the pipe ended with the helper: its last lines are in the log
    if (log_read_) CloseHandle(log_read_);
    log_read_ = nullptr;
    if (frame_) UnmapViewOfFile(frame_);
    frame_ = nullptr;
    for (HANDLE* handle : {&ready_, &frame_mapping_}) {
        if (*handle) CloseHandle(*handle);
        *handle = nullptr;
    }
    for (Shared* shared : {&colour_shared_, &depth_shared_, &motion_shared_, &output_shared_}) {
        if (shared->handle) CloseHandle(shared->handle);
        *shared = Shared{};
    }
}

bool UpscaleBridge::acquire_input(DWORD timeout_ms) {
    for (Shared* shared : {&colour_shared_, &depth_shared_, &motion_shared_}) {
        if (!shared->lock || shared->lock->AcquireSync(0, timeout_ms) != S_OK) return false;
    }
    return true;
}

bool UpscaleBridge::submit(ID3D11DeviceContext* context, const UpscaleFrame& frame, DWORD timeout_ms) {
    if (!frame_ || !output_shared_.lock) return false;
    *frame_ = frame;
    frame_->number = ++frames_;
    context->Flush();
    for (Shared* shared : {&colour_shared_, &depth_shared_, &motion_shared_}) {
        if (FAILED(shared->lock->ReleaseSync(1))) return false;
    }
    return output_shared_.lock->AcquireSync(1, timeout_ms) == S_OK;
}

void UpscaleBridge::release_output() {
    if (output_shared_.lock) output_shared_.lock->ReleaseSync(0);
}

}  // namespace xport::pc
