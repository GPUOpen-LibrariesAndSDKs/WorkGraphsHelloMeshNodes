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

#pragma once

#include <windows.h>
#include <atlbase.h>
#include <conio.h>

#include <d3d12.h>
#include <d3dx12/d3dx12.h>
#include <dxcapi.h>
#include <dxgi1_6.h>

constexpr UINT WindowSize = 720;

static const wchar_t* kProgramName = L"Hello Mesh Nodes";

class HelloMeshNodes
{
public:
    HelloMeshNodes() = default;
    ~HelloMeshNodes();
    // Initialize D3D12 and Work graphs objects
    void Initialize(HWND hwnd);
    // Record command list, execute the list and present the finished frame
    void Render();

private:
    static constexpr UINT FrameCount = 2;

    // Pipeline objects
    CComPtr<IDXGISwapChain3> swapChain_;
    CComPtr<ID3D12Device9> device_;
    CComPtr<ID3D12Resource> renderTargets_[FrameCount];
    CComPtr<ID3D12PipelineState> pipelineState_;
    CComPtr<ID3D12Resource> depthBuffer_;

    UINT descriptorSize_;
    CComPtr<ID3D12DescriptorHeap> renderViewDescriptorHeap_;
    CComPtr<ID3D12DescriptorHeap> depthDescriptorHeap_;

    CComPtr<ID3D12CommandAllocator> commandAllocator_;
    CComPtr<ID3D12CommandQueue> commandQueue_;
    CComPtr<ID3D12GraphicsCommandList10> commandList_;

    // Work graphs objects
    CComPtr<ID3D12RootSignature> globalRootSignature_;
    CComPtr<ID3DBlob> workGraphLibrary_;
    CComPtr<ID3DBlob> pixelShaderLibrary_;

    CComPtr<ID3D12StateObject> stateObject_;
    D3D12_SET_PROGRAM_DESC setProgramDesc_;

    ID3D12Resource* frameBuffer_;

    // Synchronization objects.
    UINT frameIndex_;
    HANDLE fenceEvent_;
    CComPtr<ID3D12Fence> fence_;
    UINT64 fenceValue_;

    // Initializes common DirectX objects
    // - D3D12Device
    // - D3D12CommandQueue
    // - DXGISwapChain
    // - Render View Descriptor Heap
    // - Render Targets
    // - Depth Descriptor Heap
    // - Depth Buffer
    // - D3D12CommandAllocator
    // - D3D12GraphicsCommandList
    // - D3D12RootSignature
    void InitializeDirectX(HWND hwnd);

    // Enables experimental D3D12 features for mesh nodes
    void EnableExperimentalFeatures();

    // Checks if work graphs and mesh nodes are supported on the current device
    void CheckWorkGraphMeshNodeSupport();

    // Creates work graphs state object
    ID3D12StateObject* CreateGWGStateObject();
    // Prepares work graph state object description for execution
    D3D12_SET_PROGRAM_DESC PrepareWorkGraph(CComPtr<ID3D12StateObject> pStateObject);

    // Records command list:
    // - clear render target
    // - clear depth buffer
    // - dispatch work graph
    void RecordCommandList();

    // wait for previous frame to finish
    void WaitForPreviousFrame();
};

namespace d3d12 {
    extern HMODULE sDxCompilerDLL;
    void LoadCompiler();	
    // Compiles work graphs library with required meta data
    ID3DBlob* CompileShader(const std::string& shaderCode, const wchar_t* entryPoint, const wchar_t* targetProfil);
    void ReleaseCompiler();
    
    ID3D12Resource* AllocateBuffer(CComPtr<ID3D12Device9> pDevice, UINT64 Size, D3D12_RESOURCE_FLAGS ResourceFlags, D3D12_HEAP_TYPE HeapType);
    
    void TransitionBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES stateBefore, D3D12_RESOURCE_STATES stateAfter);
}

namespace window {
    HWND Initialize(HelloMeshNodes* ctx);
    LRESULT CALLBACK Proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

    void MessageLoop();
}
