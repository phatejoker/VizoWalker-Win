#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <wrl/client.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

using Microsoft::WRL::ComPtr;
using namespace winrt;
using namespace Windows::Graphics;
using namespace Windows::Graphics::Capture;
using namespace Windows::Graphics::DirectX;
using namespace Windows::Graphics::DirectX::Direct3D11;

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace
{
    constexpr UINT HOTKEY_RECENTER = 1;
    constexpr UINT HOTKEY_QUIT = 2;
    constexpr UINT HOTKEY_CURSOR = 3;
    constexpr double Q30 = 1073741824.0;
    constexpr double DEG = 3.14159265358979323846 / 180.0;

    struct Quaternion
    {
        double w{1.0}, x{0.0}, y{0.0}, z{0.0};
    };

    Quaternion Normalize(Quaternion q)
    {
        double n = std::sqrt(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
        if (n < 1e-12) return {};
        q.w /= n; q.x /= n; q.y /= n; q.z /= n;
        return q;
    }

    Quaternion Conjugate(const Quaternion& q)
    {
        return {q.w, -q.x, -q.y, -q.z};
    }

    Quaternion Multiply(const Quaternion& a, const Quaternion& b)
    {
        return {
            a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
            a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
            a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
            a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w
        };
    }

    Quaternion Relative(const Quaternion& zero, const Quaternion& current)
    {
        return Normalize(Multiply(Conjugate(Normalize(zero)), Normalize(current)));
    }

    // Confirmed mapping from the previous probe:
    // VIZO X = pitch, Y = roll, Z = yaw.
    // Scene axes used by the old spatial renderer:
    // X = pitch, Y = yaw, Z = -roll.
    Quaternion DeviceToScene(const Quaternion& q)
    {
        return Normalize({q.w, q.x, q.z, -q.y});
    }

    struct Vec3 { double x{}, y{}, z{}; };

    Vec3 RotateVec(const Vec3& v, const Quaternion& q)
    {
        Quaternion p{0.0, v.x, v.y, v.z};
        Quaternion r = Multiply(Multiply(q, p), Conjugate(q));
        return {r.x, r.y, r.z};
    }

    int32_t ReadBE32(const uint8_t* p)
    {
        uint32_t v =
            (uint32_t(p[0]) << 24) |
            (uint32_t(p[1]) << 16) |
            (uint32_t(p[2]) << 8) |
             uint32_t(p[3]);
        return static_cast<int32_t>(v);
    }

    class VizoTracker
    {
    public:
        ~VizoTracker() { Stop(); }

        bool Start()
        {
            if (m_thread.joinable()) return true;

            GUID hidGuid{};
            HidD_GetHidGuid(&hidGuid);

            HDEVINFO info = SetupDiGetClassDevsW(
                &hidGuid, nullptr, nullptr,
                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

            if (info == INVALID_HANDLE_VALUE) return false;

            HANDLE chosen = INVALID_HANDLE_VALUE;
            USHORT chosenLen = 0;

            for (DWORD i = 0;; ++i)
            {
                SP_DEVICE_INTERFACE_DATA iface{};
                iface.cbSize = sizeof(iface);
                if (!SetupDiEnumDeviceInterfaces(info, nullptr, &hidGuid, i, &iface))
                {
                    if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
                    continue;
                }

                DWORD required = 0;
                SetupDiGetDeviceInterfaceDetailW(info, &iface, nullptr, 0, &required, nullptr);
                if (!required) continue;

                std::vector<uint8_t> storage(required);
                auto detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(storage.data());
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

                if (!SetupDiGetDeviceInterfaceDetailW(
                    info, &iface, detail, required, nullptr, nullptr))
                    continue;

                HANDLE h = CreateFileW(
                    detail->DevicePath,
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);

                if (h == INVALID_HANDLE_VALUE) continue;

                HIDD_ATTRIBUTES attr{};
                attr.Size = sizeof(attr);
                if (!HidD_GetAttributes(h, &attr) ||
                    attr.VendorID != 0x0483 ||
                    attr.ProductID != 0x5743)
                {
                    CloseHandle(h);
                    continue;
                }

                PHIDP_PREPARSED_DATA ppd = nullptr;
                HIDP_CAPS caps{};
                if (!HidD_GetPreparsedData(h, &ppd))
                {
                    CloseHandle(h);
                    continue;
                }

                NTSTATUS capsStatus = HidP_GetCaps(ppd, &caps);
                HidD_FreePreparsedData(ppd);

                if (capsStatus != HIDP_STATUS_SUCCESS || caps.InputReportByteLength < 20)
                {
                    CloseHandle(h);
                    continue;
                }

                // WebHID exposed usagePage 255 for the streaming collection.
                // Prefer that collection, but allow another VID/PID HID collection
                // as a fallback if it has a sufficiently large input report.
                bool preferred = (caps.UsagePage == 0x00FF) || ((caps.UsagePage & 0xFF00) == 0xFF00);

                if (preferred)
                {
                    if (chosen != INVALID_HANDLE_VALUE) CloseHandle(chosen);
                    chosen = h;
                    chosenLen = caps.InputReportByteLength;
                    break;
                }

                if (chosen == INVALID_HANDLE_VALUE)
                {
                    chosen = h;
                    chosenLen = caps.InputReportByteLength;
                }
                else
                {
                    CloseHandle(h);
                }
            }

            SetupDiDestroyDeviceInfoList(info);

            if (chosen == INVALID_HANDLE_VALUE) return false;

            m_handle = chosen;
            m_reportLength = chosenLen;
            m_stop = false;
            m_thread = std::thread([this] { ReaderLoop(); });
            return true;
        }

        void Stop()
        {
            m_stop = true;
            HANDLE h = m_handle.exchange(INVALID_HANDLE_VALUE);
            if (h != INVALID_HANDLE_VALUE)
            {
                CancelIoEx(h, nullptr);
                CloseHandle(h);
            }

            if (m_thread.joinable())
                m_thread.join();
        }

        bool GetRelative(Quaternion& out)
        {
            std::scoped_lock lock(m_mutex);
            if (!m_haveCurrent) return false;
            out = Relative(m_zero, m_current);
            return true;
        }

        void Recenter()
        {
            std::scoped_lock lock(m_mutex);
            if (m_haveCurrent) m_zero = m_current;
        }

        bool Connected() const { return m_haveCurrent.load(); }

    private:
        void ReaderLoop()
        {
            std::vector<uint8_t> report(std::max<USHORT>(m_reportLength, 64));

            while (!m_stop)
            {
                HANDLE h = m_handle.load();
                if (h == INVALID_HANDLE_VALUE) break;

                DWORD got = 0;
                BOOL ok = ReadFile(h, report.data(), static_cast<DWORD>(report.size()), &got, nullptr);
                if (!ok)
                {
                    if (m_stop) break;
                    Sleep(20);
                    continue;
                }

                // Win32 HID ReadFile includes the report ID at byte 0.
                // WebHID report 3 exposed payload bytes 3..18 as Q30 W,X,Y,Z,
                // therefore native offsets are 4,8,12,16.
                if (got < 20 || report[0] != 3) continue;

                Quaternion q{
                    ReadBE32(&report[4])  / Q30,
                    ReadBE32(&report[8])  / Q30,
                    ReadBE32(&report[12]) / Q30,
                    ReadBE32(&report[16]) / Q30
                };
                q = Normalize(q);

                {
                    std::scoped_lock lock(m_mutex);
                    m_current = q;
                    if (!m_haveCurrent)
                    {
                        m_zero = q;
                        m_haveCurrent = true;
                    }
                }
            }
        }

        std::atomic<HANDLE> m_handle{INVALID_HANDLE_VALUE};
        USHORT m_reportLength{64};
        std::atomic<bool> m_stop{false};
        std::atomic<bool> m_haveCurrent{false};
        std::thread m_thread;
        std::mutex m_mutex;
        Quaternion m_current{};
        Quaternion m_zero{};
    };

    struct Vertex
    {
        float x, y;
        float u, v;
    };

    class NativeSpatialApp
    {
    public:
        ~NativeSpatialApp()
        {
            Cleanup();
        }

        int Run(HINSTANCE instance)
        {
            m_instance = instance;

            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

            winrt::init_apartment(winrt::apartment_type::multi_threaded);

            if (!FindMonitors())
            {
                MessageBoxW(nullptr, L"Impossibile trovare un display.", L"VizoWalker Native", MB_ICONERROR);
                return 1;
            }

            if (!CreateAppWindow())
                return 2;

            if (!InitD3D())
            {
                MessageBoxW(m_hwnd, L"Impossibile inizializzare Direct3D 11.", L"VizoWalker Native", MB_ICONERROR);
                return 3;
            }

            if (!InitCapture())
            {
                MessageBoxW(m_hwnd,
                    L"Impossibile inizializzare Windows Graphics Capture sul display primario.",
                    L"VizoWalker Native", MB_ICONERROR);
                return 4;
            }

            m_tracker.Start();

            RegisterHotKey(m_hwnd, HOTKEY_RECENTER, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'C');
            RegisterHotKey(m_hwnd, HOTKEY_QUIT, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'Q');
            RegisterHotKey(m_hwnd, HOTKEY_CURSOR, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'V');

            ShowWindow(m_hwnd, SW_SHOW);
            UpdateWindow(m_hwnd);

            // Match the previous Extended Canvas workflow: keep the real Windows
            // pointer on the primary Surface display. Toggle with Ctrl+Alt+Shift+V.
            SetCursorLock(true);

            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            Cleanup();
            return static_cast<int>(msg.wParam);
        }

    private:
        bool FindMonitors()
        {
            m_primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);

            struct State
            {
                HMONITOR primary{};
                HMONITOR external{};
            } state{m_primary, nullptr};

            EnumDisplayMonitors(nullptr, nullptr,
                [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL
                {
                    auto* s = reinterpret_cast<State*>(lp);
                    MONITORINFO mi{};
                    mi.cbSize = sizeof(mi);
                    if (GetMonitorInfoW(mon, &mi))
                    {
                        if (!(mi.dwFlags & MONITORINFOF_PRIMARY) && !s->external)
                            s->external = mon;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&state));

            m_output = state.external ? state.external : m_primary;

            MONITORINFO src{};
            src.cbSize = sizeof(src);
            if (!GetMonitorInfoW(m_primary, &src)) return false;
            m_primaryRect = src.rcMonitor;

            MONITORINFO out{};
            out.cbSize = sizeof(out);
            if (!GetMonitorInfoW(m_output, &out)) return false;
            m_outputRect = out.rcMonitor;

            return true;
        }

        bool CreateAppWindow()
        {
            const wchar_t* className = L"VizoWalkerNativeWindow";

            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.hInstance = m_instance;
            wc.lpfnWndProc = WindowProcStatic;
            wc.lpszClassName = className;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));

            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                return false;

            int x = m_outputRect.left;
            int y = m_outputRect.top;
            int w = m_outputRect.right - m_outputRect.left;
            int h = m_outputRect.bottom - m_outputRect.top;

            m_hwnd = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
                className,
                L"VizoWalker Native v0.1",
                WS_POPUP,
                x, y, w, h,
                nullptr, nullptr, m_instance, this);

            return m_hwnd != nullptr;
        }

        bool InitD3D()
        {
            UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
            flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
            D3D_FEATURE_LEVEL levels[] = {
                D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1,
                D3D_FEATURE_LEVEL_10_0
            };

            D3D_FEATURE_LEVEL actual{};
            HRESULT hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                flags,
                levels,
                ARRAYSIZE(levels),
                D3D11_SDK_VERSION,
                &m_device,
                &actual,
                &m_context);

            if (FAILED(hr)) return false;

            ComPtr<IDXGIDevice> dxgiDevice;
            if (FAILED(m_device.As(&dxgiDevice))) return false;

            ComPtr<IDXGIAdapter> adapter;
            if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;

            ComPtr<IDXGIFactory2> factory;
            if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

            RECT rc{};
            GetClientRect(m_hwnd, &rc);
            m_width = std::max<LONG>(1, rc.right - rc.left);
            m_height = std::max<LONG>(1, rc.bottom - rc.top);

            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = m_width;
            sd.Height = m_height;
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            sd.Scaling = DXGI_SCALING_STRETCH;
            sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

            if (FAILED(factory->CreateSwapChainForHwnd(
                m_device.Get(), m_hwnd, &sd, nullptr, nullptr, &m_swapChain)))
                return false;

            factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);

            if (!CreateBackBuffer()) return false;
            if (!CreatePipeline()) return false;

            ComPtr<IInspectable> inspectable;
            hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), &inspectable);
            if (FAILED(hr)) return false;
            m_winrtDevice = inspectable.as<IDirect3DDevice>();

            return true;
        }

        bool CreateBackBuffer()
        {
            m_rtv.Reset();

            ComPtr<ID3D11Texture2D> backBuffer;
            if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
                return false;

            if (FAILED(m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv)))
                return false;

            return true;
        }

        bool CreatePipeline()
        {
            static const char* vsSource = R"(
                struct VSIn {
                    float2 pos : POSITION;
                    float2 uv  : TEXCOORD0;
                };
                struct VSOut {
                    float4 pos : SV_POSITION;
                    float2 uv  : TEXCOORD0;
                };
                VSOut main(VSIn i) {
                    VSOut o;
                    o.pos = float4(i.pos, 0.0, 1.0);
                    o.uv = i.uv;
                    return o;
                }
            )";

            static const char* psSource = R"(
                Texture2D sourceTexture : register(t0);
                SamplerState linearSampler : register(s0);

                float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
                    return sourceTexture.Sample(linearSampler, uv);
                }
            )";

            UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
            ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;

            HRESULT hr = D3DCompile(
                vsSource, strlen(vsSource), nullptr, nullptr, nullptr,
                "main", "vs_5_0", compileFlags, 0, &vsBlob, &errorBlob);
            if (FAILED(hr)) return false;

            errorBlob.Reset();
            hr = D3DCompile(
                psSource, strlen(psSource), nullptr, nullptr, nullptr,
                "main", "ps_5_0", compileFlags, 0, &psBlob, &errorBlob);
            if (FAILED(hr)) return false;

            if (FAILED(m_device->CreateVertexShader(
                vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vs)))
                return false;

            if (FAILED(m_device->CreatePixelShader(
                psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_ps)))
                return false;

            D3D11_INPUT_ELEMENT_DESC layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                 D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
                 D3D11_INPUT_PER_VERTEX_DATA, 0}
            };

            if (FAILED(m_device->CreateInputLayout(
                layout, ARRAYSIZE(layout),
                vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                &m_inputLayout)))
                return false;

            D3D11_BUFFER_DESC vb{};
            vb.ByteWidth = sizeof(Vertex) * 4;
            vb.Usage = D3D11_USAGE_DYNAMIC;
            vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            if (FAILED(m_device->CreateBuffer(&vb, nullptr, &m_vertexBuffer)))
                return false;

            const uint16_t indices[] = {0,1,2, 0,2,3};
            D3D11_BUFFER_DESC ib{};
            ib.ByteWidth = sizeof(indices);
            ib.Usage = D3D11_USAGE_IMMUTABLE;
            ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
            D3D11_SUBRESOURCE_DATA ibData{};
            ibData.pSysMem = indices;

            if (FAILED(m_device->CreateBuffer(&ib, &ibData, &m_indexBuffer)))
                return false;

            D3D11_SAMPLER_DESC samp{};
            samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            samp.MaxLOD = D3D11_FLOAT32_MAX;

            if (FAILED(m_device->CreateSamplerState(&samp, &m_sampler)))
                return false;

            return true;
        }

        GraphicsCaptureItem CreateItemForMonitor(HMONITOR monitor)
        {
            auto factory = winrt::get_activation_factory<
                GraphicsCaptureItem, IGraphicsCaptureItemInterop>();

            GraphicsCaptureItem item{nullptr};
            winrt::check_hresult(factory->CreateForMonitor(
                monitor,
                winrt::guid_of<GraphicsCaptureItem>(),
                winrt::put_abi(item)));

            return item;
        }

        bool InitCapture()
        {
            try
            {
                if (!GraphicsCaptureSession::IsSupported())
                    return false;

                m_captureItem = CreateItemForMonitor(m_primary);
                auto size = m_captureItem.Size();

                m_framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
                    m_winrtDevice,
                    DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    2,
                    size);

                m_session = m_framePool.CreateCaptureSession(m_captureItem);

                try { m_session.IsCursorCaptureEnabled(true); }
                catch (...) {}

                m_frameToken = m_framePool.FrameArrived(
                    [this](Direct3D11CaptureFramePool const& sender, IInspectable const&)
                    {
                        OnFrame(sender);
                    });

                m_session.StartCapture();
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        void EnsureCaptureTexture(UINT width, UINT height)
        {
            if (m_captureTexture && width == m_captureWidth && height == m_captureHeight)
                return;

            m_captureSrv.Reset();
            m_captureTexture.Reset();

            D3D11_TEXTURE2D_DESC td{};
            td.Width = width;
            td.Height = height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            winrt::check_hresult(m_device->CreateTexture2D(&td, nullptr, &m_captureTexture));
            winrt::check_hresult(m_device->CreateShaderResourceView(
                m_captureTexture.Get(), nullptr, &m_captureSrv));

            m_captureWidth = width;
            m_captureHeight = height;
        }

        void OnFrame(Direct3D11CaptureFramePool const& sender)
        {
            std::scoped_lock lock(m_renderMutex);

            try
            {
                auto frame = sender.TryGetNextFrame();
                if (!frame) return;

                auto size = frame.ContentSize();
                if (size.Width <= 0 || size.Height <= 0) return;

                auto access = frame.Surface().as<IDirect3DDxgiInterfaceAccess>();
                ComPtr<ID3D11Texture2D> source;
                winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&source)));

                EnsureCaptureTexture(
                    static_cast<UINT>(size.Width),
                    static_cast<UINT>(size.Height));

                m_context->CopyResource(m_captureTexture.Get(), source.Get());
                Render();
            }
            catch (...)
            {
                // Keep the capture loop alive; a transient frame failure must not
                // mutate the system or tear down the app.
            }
        }

        bool Project(const Vec3& p, const Quaternion& scene, double& px, double& py)
        {
            Quaternion inv = Conjugate(scene);
            Vec3 pc = RotateVec(p, inv);
            double z = -pc.z;
            if (z <= 0.05) return false;

            double hfov = m_fovDeg * DEG;
            double focal = (double(m_width) / 2.0) / std::tan(hfov / 2.0);

            px = double(m_width) / 2.0 + focal * pc.x / z;
            py = double(m_height) / 2.0 - focal * pc.y / z;
            return true;
        }

        void BuildVertices(Vertex (&v)[4])
        {
            Quaternion rel{};
            Quaternion scene{};
            if (m_tracker.GetRelative(rel))
                scene = DeviceToScene(rel);

            const double hfov = m_fovDeg * DEG;
            const double distance = 3.0;
            const double angle = hfov * m_canvasScale;
            const double halfW = distance * std::tan(angle / 2.0);
            const double halfH = halfW / (16.0 / 9.0);

            const double centerX = distance * std::tan(m_offsetXDeg * DEG);
            const double centerY = distance * std::tan(m_offsetYDeg * DEG);

            Vec3 world[4] = {
                {centerX-halfW, centerY+halfH, -distance}, // TL
                {centerX+halfW, centerY+halfH, -distance}, // TR
                {centerX+halfW, centerY-halfH, -distance}, // BR
                {centerX-halfW, centerY-halfH, -distance}  // BL
            };

            const float uv[4][2] = {
                {0,0},{1,0},{1,1},{0,1}
            };

            for (int i = 0; i < 4; ++i)
            {
                double px = 0, py = 0;
                if (!Project(world[i], scene, px, py))
                {
                    // Move behind-camera vertices outside clip space.
                    v[i] = {2.0f, 2.0f, uv[i][0], uv[i][1]};
                    continue;
                }

                float ndcX = float((px / double(m_width)) * 2.0 - 1.0);
                float ndcY = float(1.0 - (py / double(m_height)) * 2.0);
                v[i] = {ndcX, ndcY, uv[i][0], uv[i][1]};
            }
        }

        void Render()
        {
            if (!m_captureSrv || !m_rtv) return;

            Vertex vertices[4]{};
            BuildVertices(vertices);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(m_context->Map(
                m_vertexBuffer.Get(), 0,
                D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                return;

            memcpy(mapped.pData, vertices, sizeof(vertices));
            m_context->Unmap(m_vertexBuffer.Get(), 0);

            const float black[4] = {0,0,0,1};
            m_context->ClearRenderTargetView(m_rtv.Get(), black);

            D3D11_VIEWPORT vp{};
            vp.Width = static_cast<float>(m_width);
            vp.Height = static_cast<float>(m_height);
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            m_context->RSSetViewports(1, &vp);

            UINT stride = sizeof(Vertex);
            UINT offset = 0;
            ID3D11Buffer* vb = m_vertexBuffer.Get();
            m_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            m_context->IASetIndexBuffer(m_indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
            m_context->IASetInputLayout(m_inputLayout.Get());
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            m_context->VSSetShader(m_vs.Get(), nullptr, 0);
            m_context->PSSetShader(m_ps.Get(), nullptr, 0);

            ID3D11ShaderResourceView* srv = m_captureSrv.Get();
            ID3D11SamplerState* sampler = m_sampler.Get();
            m_context->PSSetShaderResources(0, 1, &srv);
            m_context->PSSetSamplers(0, 1, &sampler);

            ID3D11RenderTargetView* rtv = m_rtv.Get();
            m_context->OMSetRenderTargets(1, &rtv, nullptr);

            m_context->DrawIndexed(6, 0, 0);

            ID3D11ShaderResourceView* nullSrv = nullptr;
            m_context->PSSetShaderResources(0, 1, &nullSrv);

            m_swapChain->Present(1, 0);
        }

        void SetCursorLock(bool lock)
        {
            m_cursorLocked = lock;
            if (lock)
                ClipCursor(&m_primaryRect);
            else
                ClipCursor(nullptr);
        }

        void ToggleCursorLock()
        {
            SetCursorLock(!m_cursorLocked);
        }

        void Cleanup()
        {
            if (m_cleaned.exchange(true)) return;

            ClipCursor(nullptr);

            if (m_hwnd)
            {
                UnregisterHotKey(m_hwnd, HOTKEY_RECENTER);
                UnregisterHotKey(m_hwnd, HOTKEY_QUIT);
                UnregisterHotKey(m_hwnd, HOTKEY_CURSOR);
            }

            m_tracker.Stop();

            try
            {
                if (m_framePool && m_frameToken.value)
                    m_framePool.FrameArrived(m_frameToken);
            }
            catch (...) {}

            try { if (m_session) m_session.Close(); } catch (...) {}
            try { if (m_framePool) m_framePool.Close(); } catch (...) {}

            m_session = nullptr;
            m_framePool = nullptr;
            m_captureItem = nullptr;

            if (m_context)
            {
                m_context->ClearState();
                m_context->Flush();
            }
        }

        LRESULT WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            switch (msg)
            {
            case WM_HOTKEY:
                if (wp == HOTKEY_RECENTER)
                {
                    m_tracker.Recenter();
                    return 0;
                }
                if (wp == HOTKEY_QUIT)
                {
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                }
                if (wp == HOTKEY_CURSOR)
                {
                    ToggleCursorLock();
                    return 0;
                }
                break;

            case WM_KEYDOWN:
                if (wp == VK_ESCAPE)
                {
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                }
                break;

            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;

            case WM_DESTROY:
                Cleanup();
                m_hwnd = nullptr;
                PostQuitMessage(0);
                return 0;
            }

            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        static LRESULT CALLBACK WindowProcStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            NativeSpatialApp* self = nullptr;

            if (msg == WM_NCCREATE)
            {
                auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                self = reinterpret_cast<NativeSpatialApp*>(cs->lpCreateParams);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                self->m_hwnd = hwnd;
            }
            else
            {
                self = reinterpret_cast<NativeSpatialApp*>(
                    GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            }

            return self ? self->WindowProc(hwnd, msg, wp, lp)
                        : DefWindowProcW(hwnd, msg, wp, lp);
        }

        HINSTANCE m_instance{};
        HWND m_hwnd{};
        HMONITOR m_primary{};
        HMONITOR m_output{};
        RECT m_primaryRect{};
        RECT m_outputRect{};
        UINT m_width{1920};
        UINT m_height{1080};

        bool m_cursorLocked{false};
        std::atomic<bool> m_cleaned{false};

        // Same defaults as the working browser/WebView prototype.
        double m_fovDeg{42.0};
        double m_canvasScale{0.98};
        double m_offsetXDeg{0.0};
        double m_offsetYDeg{0.0};

        VizoTracker m_tracker;

        ComPtr<ID3D11Device> m_device;
        ComPtr<ID3D11DeviceContext> m_context;
        ComPtr<IDXGISwapChain1> m_swapChain;
        ComPtr<ID3D11RenderTargetView> m_rtv;

        ComPtr<ID3D11VertexShader> m_vs;
        ComPtr<ID3D11PixelShader> m_ps;
        ComPtr<ID3D11InputLayout> m_inputLayout;
        ComPtr<ID3D11Buffer> m_vertexBuffer;
        ComPtr<ID3D11Buffer> m_indexBuffer;
        ComPtr<ID3D11SamplerState> m_sampler;

        ComPtr<ID3D11Texture2D> m_captureTexture;
        ComPtr<ID3D11ShaderResourceView> m_captureSrv;
        UINT m_captureWidth{};
        UINT m_captureHeight{};

        IDirect3DDevice m_winrtDevice{nullptr};
        GraphicsCaptureItem m_captureItem{nullptr};
        Direct3D11CaptureFramePool m_framePool{nullptr};
        GraphicsCaptureSession m_session{nullptr};
        winrt::event_token m_frameToken{};

        std::mutex m_renderMutex;
    };
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    // Always release a possible cursor clip on process teardown.
    struct CursorGuard
    {
        ~CursorGuard() { ClipCursor(nullptr); }
    } guard;

    try
    {
        NativeSpatialApp app;
        return app.Run(hInstance);
    }
    catch (winrt::hresult_error const& e)
    {
        std::wstring msg = L"Errore WinRT/D3D:\n" + e.message();
        MessageBoxW(nullptr, msg.c_str(), L"VizoWalker Native", MB_ICONERROR);
    }
    catch (...)
    {
        MessageBoxW(nullptr, L"Errore nativo non gestito.", L"VizoWalker Native", MB_ICONERROR);
    }

    ClipCursor(nullptr);
    return 1;
}
