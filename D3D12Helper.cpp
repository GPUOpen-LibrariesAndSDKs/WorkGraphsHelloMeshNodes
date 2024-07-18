/**********************************************************************
Copyright (c) 2024 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
********************************************************************/

#include "HelloMeshNodes.h"

#include <string>
#include <algorithm>

#define ERROR_QUIT(value, ...) if(!(value)) { printf("ERROR: "); printf(__VA_ARGS__); printf("\nPress any key to terminate...\n"); _getch(); throw 0; }

namespace {
    // function GetHardwareAdapter() copy-pasted from the publicly distributed sample provided at: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice
    void GetHardwareAdapter(IDXGIFactory4* pFactory, IDXGIAdapter1** ppAdapter)
    {
        *ppAdapter = nullptr;
        for (UINT adapterIndex = 0; ; ++adapterIndex)
        {
            IDXGIAdapter1* pAdapter = nullptr;
            if (DXGI_ERROR_NOT_FOUND == pFactory->EnumAdapters1(adapterIndex, &pAdapter))
            {
                // No more adapters to enumerate.
                break;
            }

            // Check to see if the adapter supports Direct3D 12, but don't create the
            // actual device yet.
            if (SUCCEEDED(D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr)))
            {
                *ppAdapter = pAdapter;
                return;
            }
            pAdapter->Release();
        }
    }
}

HelloMeshNodes::~HelloMeshNodes()
{
    if (device_) {
        WaitForPreviousFrame();
        CloseHandle(fenceEvent_);
    }
}

void HelloMeshNodes::InitializeDirectX(HWND hwnd)
{
    HRESULT hresult;

    device_ = nullptr;

    CComPtr<IDXGIFactory4> factory;
    hresult = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    ERROR_QUIT(hresult == S_OK, "Failed to create IDXGIFactory4.");

    CComPtr<IDXGIAdapter1> hardwareAdapter;
    GetHardwareAdapter(factory, &hardwareAdapter);
    hresult = D3D12CreateDevice(hardwareAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
    ERROR_QUIT(hresult == S_OK, "Failed to create ID3D12Device.");

    // Create the command queue.
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    hresult = device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue_));
    ERROR_QUIT(hresult == S_OK, "Failed to create ID3D12CommandQueue.");

    // Create the swap chain.
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = WindowSize;
    swapChainDesc.Height = WindowSize;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    CComPtr<IDXGISwapChain1> swapChain;
    hresult = factory->CreateSwapChainForHwnd(
        commandQueue_,
        hwnd,
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain
    );
    ERROR_QUIT(hresult == S_OK, "Failed to create IDXGISwapChain1.");

    hresult = factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    ERROR_QUIT(hresult == S_OK, "Failed to make window association.");

    hresult = swapChain.QueryInterface(&swapChain_);
    ERROR_QUIT(hresult == S_OK, "Failed to query IDXGISwapChain3.");

    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();

    // Create render target view (RTV) descriptor heaps.
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = FrameCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hresult = device_->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&renderViewDescriptorHeap_));
        ERROR_QUIT(hresult == S_OK, "Failed to create RTV descriptor heap.");

        descriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    // Create frame resources.
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(renderViewDescriptorHeap_->GetCPUDescriptorHandleForHeapStart());

        // Create a RTV for each frame.
        for (UINT n = 0; n < FrameCount; n++)
        {
            hresult = swapChain_->GetBuffer(n, IID_PPV_ARGS(&renderTargets_[n]));
            ERROR_QUIT(hresult == S_OK, "Failed to access render target of swap chain.");
            device_->CreateRenderTargetView(renderTargets_[n].p, nullptr, rtvHandle);
            rtvHandle.Offset(1, descriptorSize_);
        }
    }

    // Create a depth-stencil view (DSV) descriptor heap and depth buffer
    {
        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
        dsvHeapDesc.NumDescriptors = 1;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hresult = device_->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&depthDescriptorHeap_));
        ERROR_QUIT(hresult == S_OK, "Failed to create DSV descriptor heap.");

        depthDescriptorHeap_->SetName(L"Depth/Stencil Resource Heap");

        D3D12_DEPTH_STENCIL_VIEW_DESC depthStencilDesc = {};
        depthStencilDesc.Format = DXGI_FORMAT_D32_FLOAT;
        depthStencilDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        depthStencilDesc.Flags = D3D12_DSV_FLAG_NONE;

        D3D12_CLEAR_VALUE depthOptimizedClearValue = {};
        depthOptimizedClearValue.Format = DXGI_FORMAT_D32_FLOAT;
        depthOptimizedClearValue.DepthStencil.Depth = 1.0f;
        depthOptimizedClearValue.DepthStencil.Stencil = 0;

        CD3DX12_HEAP_PROPERTIES depthHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        CD3DX12_RESOURCE_DESC depthResourceDescription = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_D32_FLOAT,
            WindowSize, WindowSize,
            1, 0, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        hresult = device_->CreateCommittedResource(
            &depthHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &depthResourceDescription,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &depthOptimizedClearValue,
            IID_PPV_ARGS(&depthBuffer_)
        );
        ERROR_QUIT(hresult == S_OK, "Failed to create depth buffer.");

        device_->CreateDepthStencilView(depthBuffer_, &depthStencilDesc, depthDescriptorHeap_->GetCPUDescriptorHandleForHeapStart());
    }

    hresult = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator_));
    ERROR_QUIT(hresult == S_OK, "Failed to create ID3D12CommandAllocator.");

    // Create the command list.
    hresult = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator_.p, nullptr, IID_PPV_ARGS(&commandList_));
    ERROR_QUIT(hresult == S_OK, "Failed to create ID3D12GraphicsCommandList.");

    // Command lists are created in the recording state, but there is nothing
    // to record yet. The main loop expects it to be closed, so close it now.
    hresult = commandList_->Close();
    ERROR_QUIT(hresult == S_OK, "Failed to close ID3D12GraphicsCommandList.");

    // Create sync objects
    hresult = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    ERROR_QUIT(hresult == S_OK, "Failed to create ID3D12Fence.");
    fenceValue_ = 1;

    // Create an event handle to use for frame synchronization.
    fenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    ERROR_QUIT(fenceEvent_ != nullptr, "Failed to create synchronization event.");

    // Create empty root signature
    {
        CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init(0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        CComPtr<ID3DBlob> signature;
        CComPtr<ID3DBlob> error;
        hresult = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
        ERROR_QUIT(hresult == S_OK, "Failed to serialize RootSignature.");
        hresult = device_->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&globalRootSignature_));
        ERROR_QUIT(hresult == S_OK, "Failed to create RootSignature.");
    }
}

void HelloMeshNodes::Render()
{
    HRESULT hresult;
    // Reset allocator and list
    hresult = commandAllocator_->Reset();
    ERROR_QUIT(hresult == S_OK, "Failed to reset ID3D12CommandAllocator.");

    hresult = commandList_->Reset(commandAllocator_.p, pipelineState_.p);
    ERROR_QUIT(hresult == S_OK, "Failed to reset ID3D12GraphicsCommandList.");

    RecordCommandList();

    hresult = commandList_->Close();
    ERROR_QUIT(hresult == S_OK, "Failed to close ID3D12CommandAllocator.");

    // Execute the command list.
    commandQueue_->ExecuteCommandLists(1, CommandListCast(&commandList_.p));

    // Present the frame.
    hresult = swapChain_->Present(1, 0);
    ERROR_QUIT(hresult == S_OK, "Failed to present frame.");

    WaitForPreviousFrame();
}

void HelloMeshNodes::WaitForPreviousFrame()
{
    HRESULT hresult;
    // WAITING FOR THE FRAME TO COMPLETE BEFORE CONTINUING IS NOT BEST PRACTICE.
    // This is code implemented as such for simplicity. The D3D12HelloFrameBuffering 
    // sample from Microsoft illustrates how to use fences for efficient resource 
    // usage and to maximize GPU utilization.

    // Signal and increment the fence value.
    const UINT64 fence = fenceValue_;
    hresult = commandQueue_->Signal(fence_.p, fence);
    ERROR_QUIT(hresult == S_OK, "Failed to signal fence.");
    fenceValue_++;

    // Wait until the previous frame is finished.
    if (fence_->GetCompletedValue() < fence)
    {
        hresult = fence_->SetEventOnCompletion(fence, fenceEvent_);
        ERROR_QUIT(hresult == S_OK, "Failed to set up fence event.");
        WaitForSingleObject(fenceEvent_, INFINITE);
    }

    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
}

namespace d3d12 {
    HMODULE sDxCompilerDLL = nullptr;
    void LoadCompiler()
    {
        // load compiler
        sDxCompilerDLL = LoadLibrary(L"dxcompiler.dll");

        ERROR_QUIT(sDxCompilerDLL, "Failed to initialize compiler.");
    }

    void ReleaseCompiler()
    {
        if (sDxCompilerDLL)
        {
            FreeLibrary(sDxCompilerDLL);
            sDxCompilerDLL = nullptr;
        }
    }

    ID3D12Resource* AllocateBuffer(CComPtr<ID3D12Device9> pDevice, UINT64 Size, D3D12_RESOURCE_FLAGS ResourceFlags, D3D12_HEAP_TYPE HeapType)
    {
        ID3D12Resource* pResource;

        CD3DX12_HEAP_PROPERTIES HeapProperties(HeapType);
        CD3DX12_RESOURCE_DESC ResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(Size, ResourceFlags);
        HRESULT hr = pDevice->CreateCommittedResource(&HeapProperties, D3D12_HEAP_FLAG_NONE, &ResourceDesc, D3D12_RESOURCE_STATE_COMMON, NULL, IID_PPV_ARGS(&pResource));
        ERROR_QUIT(SUCCEEDED(hr), "Failed to allocate buffer.");

        return pResource;
    }

    void TransitionBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource, D3D12_RESOURCE_STATES stateBefore, D3D12_RESOURCE_STATES stateAfter)
    {
        CD3DX12_RESOURCE_BARRIER transition = CD3DX12_RESOURCE_BARRIER::Transition(resource, stateBefore, stateAfter);
        commandList->ResourceBarrier(1, &transition);
    }
}

namespace window {
    HWND Initialize(HelloMeshNodes* ctx)
    {
        const HINSTANCE hInstance = GetModuleHandleA(NULL);

        WNDCLASSEX windowClass = { 0 };
        windowClass.cbSize = sizeof(WNDCLASSEX);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = Proc;
        windowClass.hInstance = hInstance;
        windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
        windowClass.lpszClassName = L"Hello Mesh Nodes";
        RegisterClassEx(&windowClass);

        RECT windowRect = { 0, 0, WindowSize, WindowSize };
        DWORD style = WS_OVERLAPPED | WS_MINIMIZEBOX | WS_SYSMENU;
        AdjustWindowRect(&windowRect, style, FALSE);

        HWND hwnd = CreateWindow(windowClass.lpszClassName,
            windowClass.lpszClassName,
            style,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            windowRect.right - windowRect.left,
            windowRect.bottom - windowRect.top,
            nullptr,  // We have no parent window.
            nullptr,  // We are not using menus.
            hInstance,
            static_cast<void*>(ctx));

        return hwnd;
    }


    LRESULT CALLBACK Proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
        HelloMeshNodes* ctx = reinterpret_cast<HelloMeshNodes*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

        switch (message)
        {
        case WM_CREATE:
        {
            // Save the WindowContext* passed in to CreateWindow.
            LPCREATESTRUCT pCreateStruct = reinterpret_cast<LPCREATESTRUCT>(lParam);
            SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pCreateStruct->lpCreateParams));
            return 0;
        }
        case WM_PAINT:
            ctx->Render();
            return 0;
        case WM_KEYDOWN: {
            if (lParam == VK_ESCAPE) {
                PostQuitMessage(0);
                return 0;
            }
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }

        // Handle any messages the switch statement didn't.
        return DefWindowProc(hWnd, message, wParam, lParam);
    }


    void MessageLoop()
    {
        MSG msg = {};
        while (msg.message != WM_QUIT)
        {
            // Process any messages in the queue.
            if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
    }
}