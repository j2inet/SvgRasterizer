// SvgRasterizer.cpp
//
// Command-line tool that rasterizes an SVG file to a PNG using Direct2D and WIC.
//
// Usage:
//   SvgRasterizer.exe <svg_file> <scale_factor>
//
// The output PNG is written next to the SVG file with the same base name.

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shlwapi.h>
#include <d2d1_3.h>
#include <d2d1svg.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Helper: throw a std::runtime_error if hr indicates failure.
// ---------------------------------------------------------------------------
static void ThrowIfFailed(HRESULT hr, const char* context)
{
    if (FAILED(hr))
    {
        std::ostringstream oss;
        oss << context << " failed (HRESULT=0x" << std::hex << static_cast<unsigned long>(hr) << ")";
        throw std::runtime_error(oss.str());
    }
}

// ---------------------------------------------------------------------------
// wmain – entry point
// ---------------------------------------------------------------------------
int wmain(int argc, wchar_t* argv[])
{
    if (argc < 3)
    {
        std::wcerr
            << L"Usage: SvgRasterizer <svg_file> <scale_factor>\n"
            << L"\n"
            << L"  svg_file     Path to the input SVG file.\n"
            << L"  scale_factor Positive floating-point multiplier applied to\n"
            << L"               the SVG's natural width and height.\n"
            << L"\n"
            << L"The output PNG is written alongside the SVG with the extension\n"
            << L"changed to \".png\".\n";
        return 1;
    }

    // -----------------------------------------------------------------------
    // Parse arguments
    // -----------------------------------------------------------------------
    const std::wstring svgPath = argv[1];

    float scaleFactor = 1.0f;
    try
    {
        scaleFactor = std::stof(std::wstring(argv[2]));
    }
    catch (...)
    {
        std::wcerr << L"Invalid scale factor: " << argv[2] << L"\n";
        return 1;
    }

    if (scaleFactor <= 0.0f)
    {
        std::wcerr << L"Scale factor must be greater than 0.\n";
        return 1;
    }

    // Build output PNG path: same path, extension replaced with ".png"
    std::wstring pngPath = svgPath;
    const size_t dotPos = pngPath.rfind(L'.');
    if (dotPos != std::wstring::npos)
        pngPath.replace(dotPos, pngPath.size() - dotPos, L".png");
    else
        pngPath += L".png";

    // -----------------------------------------------------------------------
    // Initialize COM (STA, consistent with D2D1_FACTORY_TYPE_SINGLE_THREADED)
    // -----------------------------------------------------------------------
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comHr))
    {
        std::cerr << "CoInitializeEx failed (HRESULT=0x" << std::hex << static_cast<unsigned long>(comHr) << ")\n";
        return 1;
    }

    int exitCode = 0;
    try
    {
        // -------------------------------------------------------------------
        // Create a D3D11 device (hardware, falling back to WARP)
        // -------------------------------------------------------------------
        ComPtr<ID3D11Device>        d3dDevice;
        ComPtr<ID3D11DeviceContext>  d3dContext;
        D3D_FEATURE_LEVEL           featureLevel{};

        const D3D_DRIVER_TYPE driverTypes[] =
        {
            D3D_DRIVER_TYPE_HARDWARE,
            D3D_DRIVER_TYPE_WARP,
        };

        HRESULT hr = E_FAIL;
        for (auto driverType : driverTypes)
        {
            hr = D3D11CreateDevice(
                nullptr,
                driverType,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr, 0,
                D3D11_SDK_VERSION,
                &d3dDevice,
                &featureLevel,
                &d3dContext);
            if (SUCCEEDED(hr)) break;
        }
        ThrowIfFailed(hr, "D3D11CreateDevice");

        // -------------------------------------------------------------------
        // Create a Direct2D factory and device
        // -------------------------------------------------------------------
        D2D1_FACTORY_OPTIONS factoryOptions{};
        ComPtr<ID2D1Factory3> d2dFactory;
        ThrowIfFailed(
            D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED,
                __uuidof(ID2D1Factory3),
                &factoryOptions,
                reinterpret_cast<void**>(d2dFactory.GetAddressOf())),
            "D2D1CreateFactory");

        ComPtr<IDXGIDevice> dxgiDevice;
        ThrowIfFailed(d3dDevice.As(&dxgiDevice), "Get IDXGIDevice");

        ComPtr<ID2D1Device2> d2dDevice;
        ThrowIfFailed(d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice), "ID2D1Factory3::CreateDevice");

        // -------------------------------------------------------------------
        // Obtain an ID2D1DeviceContext5 (required for SVG APIs)
        // -------------------------------------------------------------------
        ComPtr<ID2D1DeviceContext5> dc;
        {
            ComPtr<ID2D1DeviceContext> dc0;
            ThrowIfFailed(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc0), "CreateDeviceContext");
            ThrowIfFailed(dc0.As(&dc), "QueryInterface ID2D1DeviceContext5");
        }

        // Pixel units == DIPs (96 DPI)
        dc->SetDpi(96.0f, 96.0f);

        // -------------------------------------------------------------------
        // Open the SVG file as a COM stream
        // -------------------------------------------------------------------
        ComPtr<IStream> svgStream;
        ThrowIfFailed(
            SHCreateStreamOnFileW(svgPath.c_str(), STGM_READ, &svgStream),
            "Open SVG file");

        // -------------------------------------------------------------------
        // Load the SVG document with a placeholder viewport, then interrogate
        // the root element to determine the SVG's natural dimensions.
        // -------------------------------------------------------------------
        ComPtr<ID2D1SvgDocument> svgDoc;
        ThrowIfFailed(
            dc->CreateSvgDocument(svgStream.Get(), D2D1::SizeF(1.0f, 1.0f), &svgDoc),
            "CreateSvgDocument");

        ComPtr<ID2D1SvgElement> root;
        svgDoc->GetRoot(&root);

        float svgW = 0.0f, svgH = 0.0f;

        // Prefer explicit width / height attributes
        D2D1_SVG_LENGTH widthAttr{}, heightAttr{};
        if (SUCCEEDED(root->GetAttributeValue(L"width",  &widthAttr))  && widthAttr.value  > 0.0f)
            svgW = widthAttr.value;
        if (SUCCEEDED(root->GetAttributeValue(L"height", &heightAttr)) && heightAttr.value > 0.0f)
            svgH = heightAttr.value;

        // Fall back to the viewBox dimensions
        if (svgW <= 0.0f || svgH <= 0.0f)
        {
            D2D1_SVG_VIEWBOX viewBox{};
            //if (SUCCEEDED(root->GetAttributeValue(L"viewBox", &viewBox)))
            {
                if (svgW <= 0.0f && viewBox.width  > 0.0f) svgW = viewBox.width;
                if (svgH <= 0.0f && viewBox.height > 0.0f) svgH = viewBox.height;
            }
        }

        // Final fallback: 256 px is a common default for SVGs that declare no
        // viewport size (e.g. SVGs whose root element has neither width/height
        // nor a viewBox attribute).
        if (svgW <= 0.0f) svgW = 256.0f;
        if (svgH <= 0.0f) svgH = 256.0f;

        std::wcout << L"Using a width and height of " << svgW << L"x" << svgH << std::endl;
        // -------------------------------------------------------------------
        // Compute output pixel dimensions
        // -------------------------------------------------------------------
        const UINT32 outW = std::max(1u, static_cast<UINT32>(svgW * scaleFactor));
        const UINT32 outH = std::max(1u, static_cast<UINT32>(svgH * scaleFactor));
        std::wcout << L"Requested scale factor is " << scaleFactor << L"." << std::endl;
        std::wcout << L"Using an output size of " << outW << L"x" << outH << std::endl;

        // Tell the SVG document to render into the scaled viewport
        ThrowIfFailed(
            svgDoc->SetViewportSize(D2D1::SizeF(static_cast<float>(outW), static_cast<float>(outH))),
            "SetViewportSize");

        // -------------------------------------------------------------------
        // Create a D3D11 DEFAULT texture as the render target
        // -------------------------------------------------------------------
        D3D11_TEXTURE2D_DESC texDesc{};
        texDesc.Width            = outW;
        texDesc.Height           = outH;
        texDesc.MipLevels        = 1;
        texDesc.ArraySize        = 1;
        texDesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage            = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags        = D3D11_BIND_RENDER_TARGET;

        ComPtr<ID3D11Texture2D> renderTex;
        ThrowIfFailed(d3dDevice->CreateTexture2D(&texDesc, nullptr, &renderTex), "CreateTexture2D (render)");

        // Wrap the texture as a DXGI surface for Direct2D
        ComPtr<IDXGISurface> dxgiSurface;
        ThrowIfFailed(renderTex.As(&dxgiSurface), "Get IDXGISurface");

        // Create a D2D bitmap bound to the DXGI surface
        D2D1_BITMAP_PROPERTIES1 bmpProps{};
        bmpProps.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
        bmpProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        bmpProps.dpiX                  = 96.0f;
        bmpProps.dpiY                  = 96.0f;
        bmpProps.bitmapOptions         = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

        ComPtr<ID2D1Bitmap1> targetBitmap;
        ThrowIfFailed(
            dc->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &bmpProps, &targetBitmap),
            "CreateBitmapFromDxgiSurface");

        // -------------------------------------------------------------------
        // Render the SVG document
        // -------------------------------------------------------------------
        dc->SetTarget(targetBitmap.Get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));  // transparent background
        dc->DrawSvgDocument(svgDoc.Get());
        ThrowIfFailed(dc->EndDraw(), "EndDraw");

        // -------------------------------------------------------------------
        // Copy rendered pixels to a CPU-readable staging texture
        // -------------------------------------------------------------------
        D3D11_TEXTURE2D_DESC stagingDesc = texDesc;
        stagingDesc.Usage          = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags      = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        ComPtr<ID3D11Texture2D> stagingTex;
        ThrowIfFailed(d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &stagingTex), "CreateTexture2D (staging)");

        d3dContext->CopyResource(stagingTex.Get(), renderTex.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(d3dContext->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map staging texture");

        // -------------------------------------------------------------------
        // Wrap the mapped pixel data in a WIC bitmap
        // -------------------------------------------------------------------
        ComPtr<IWICImagingFactory2> wicFactory;
        ThrowIfFailed(
            CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory)),
            "Create WIC factory");

        ComPtr<IWICBitmap> wicBitmap;
        ThrowIfFailed(
            wicFactory->CreateBitmapFromMemory(
                outW, outH,
                GUID_WICPixelFormat32bppPBGRA,
                mapped.RowPitch,
                mapped.RowPitch * outH,
                static_cast<BYTE*>(mapped.pData),
                &wicBitmap),
            "CreateBitmapFromMemory");

        d3dContext->Unmap(stagingTex.Get(), 0);

        // -------------------------------------------------------------------
        // Convert from premultiplied BGRA to straight BGRA for PNG output
        // -------------------------------------------------------------------
        ComPtr<IWICFormatConverter> converter;
        ThrowIfFailed(wicFactory->CreateFormatConverter(&converter), "CreateFormatConverter");
        ThrowIfFailed(
            converter->Initialize(
                wicBitmap.Get(),
                GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0f,
                WICBitmapPaletteTypeCustom),
            "IWICFormatConverter::Initialize");

        // -------------------------------------------------------------------
        // Encode as PNG via WIC
        // -------------------------------------------------------------------
        ComPtr<IWICStream> wicStream;
        ThrowIfFailed(wicFactory->CreateStream(&wicStream), "IWICImagingFactory::CreateStream");
        ThrowIfFailed(
            wicStream->InitializeFromFilename(pngPath.c_str(), GENERIC_WRITE),
            "IWICStream::InitializeFromFilename");

        ComPtr<IWICBitmapEncoder> encoder;
        ThrowIfFailed(
            wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder),
            "CreateEncoder");
        ThrowIfFailed(encoder->Initialize(wicStream.Get(), WICBitmapEncoderNoCache), "Encoder::Initialize");

        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2>         frameOptions;
        ThrowIfFailed(encoder->CreateNewFrame(&frame, &frameOptions), "CreateNewFrame");
        ThrowIfFailed(frame->Initialize(nullptr), "Frame::Initialize");
        ThrowIfFailed(frame->SetSize(outW, outH), "Frame::SetSize");

        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        ThrowIfFailed(frame->SetPixelFormat(&fmt), "Frame::SetPixelFormat");
        ThrowIfFailed(frame->WriteSource(converter.Get(), nullptr), "Frame::WriteSource");
        ThrowIfFailed(frame->Commit(), "Frame::Commit");
        ThrowIfFailed(encoder->Commit(), "Encoder::Commit");

        std::wcout << L"Saved: " << pngPath << L"\n";
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Error: " << ex.what() << "\n";
        exitCode = 1;
    }

    CoUninitialize();
    return exitCode;
}
