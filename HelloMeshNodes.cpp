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
#include "ShaderSource.h"

#include <algorithm>

#define ERROR_QUIT(value, ...) if(!(value)) { printf("ERROR: "); printf(__VA_ARGS__); printf("\nPress any key to terminate...\n"); _getch(); throw 0; }

void HelloMeshNodes::Initialize(HWND hwnd)
{
    EnableExperimentalFeatures();

    InitializeDirectX(hwnd);

    CheckWorkGraphMeshNodeSupport();

    // Compile shader libraries with meta data
    workGraphLibrary_ = d3d12::CompileShader(shader::workGraphSource, nullptr, L"lib_6_9");
    // Compile pixel shader separately
    pixelShaderLibrary_ = d3d12::CompileShader(shader::workGraphSource, L"MeshNodePixelShader", L"ps_6_9");

    stateObject_ = CreateGWGStateObject();
    setProgramDesc_ = PrepareWorkGraph(stateObject_);
}

void HelloMeshNodes::EnableExperimentalFeatures()
{
    // Mesh nodes require experimental state object features and shader model 6.9 which are not supported by default.
    UUID    ExperimentalFeatures[2] = { D3D12ExperimentalShaderModels, D3D12StateObjectsExperiment };
    HRESULT hr = D3D12EnableExperimentalFeatures(_countof(ExperimentalFeatures), ExperimentalFeatures, nullptr, nullptr);

    ERROR_QUIT((hr == S_OK), "Failed to enable experimental features.");
}

void HelloMeshNodes::CheckWorkGraphMeshNodeSupport()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS21 Options = {};

    HRESULT hr = device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &Options, sizeof(Options));
    ERROR_QUIT(hr == S_OK, "Failed to check support for work graphs and mesh nodes.");

    // Mesh nodes are supported in D3D12_WORK_GRAPHS_TIER_1_1
    ERROR_QUIT(Options.WorkGraphsTier >= D3D12_WORK_GRAPHS_TIER_1_1,
        "Failed to find device with D3D12 Work Graphs 1.1 support. Please check if you have a compatible driver and graphics card installed.");
}

ID3D12StateObject* HelloMeshNodes::CreateGWGStateObject()
{
    ID3D12StateObject* stateObject = nullptr;
    CD3DX12_STATE_OBJECT_DESC stateObjectDesc(D3D12_STATE_OBJECT_TYPE_EXECUTABLE);

    // Configure graphics state for global root signature
    auto configSubobject = stateObjectDesc.CreateSubobject<CD3DX12_STATE_OBJECT_CONFIG_SUBOBJECT>();
    configSubobject->SetFlags(
        D3D12_STATE_OBJECT_FLAG_WORK_GRAPHS_USE_GRAPHICS_STATE_FOR_GLOBAL_ROOT_SIGNATURE);

    CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT* globalRootSignatureSubobject = stateObjectDesc.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    globalRootSignatureSubobject->SetRootSignature(globalRootSignature_);

    CD3DX12_WORK_GRAPH_SUBOBJECT* workGraphDesc = stateObjectDesc.CreateSubobject<CD3DX12_WORK_GRAPH_SUBOBJECT>();
    workGraphDesc->IncludeAllAvailableNodes();
    workGraphDesc->SetProgramName(kProgramName);

    // Work Graph Nodes
    {
        // Here we add the DXIL library compiled with "lib_6_9" target to the state object desc.
        // With mesh nodes, this library will also contain the mesh shaders with the [NodeLaunch("mesh")] attribute.
        CD3DX12_DXIL_LIBRARY_SUBOBJECT* libraryDesc = stateObjectDesc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
        CD3DX12_SHADER_BYTECODE libraryCode(workGraphLibrary_);
        libraryDesc->SetDXILLibrary(&libraryCode);
    }

    // Next we need to add the separately compiled pixel shader to the state object desc.
    // The pixel shader itself will be compiled with target "ps_6_9" and is added to the state object as a DXIL library.
    {
        CD3DX12_DXIL_LIBRARY_SUBOBJECT* libraryDesc = stateObjectDesc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
        CD3DX12_SHADER_BYTECODE libraryCode(pixelShaderLibrary_);
        libraryDesc->SetDXILLibrary(&libraryCode);
    }

    // In the following section we add subobject for various graphics states to the state object description.
    // These subobjects form "building blocks" and allow us to then create different mesh nodes with them.

    // Subobject to define rasterizer state for generic programs
    auto rasterizerSubobject = stateObjectDesc.CreateSubobject<CD3DX12_RASTERIZER_SUBOBJECT>();
    rasterizerSubobject->SetFrontCounterClockwise(true);
    rasterizerSubobject->SetFillMode(D3D12_FILL_MODE_SOLID);
    rasterizerSubobject->SetCullMode(D3D12_CULL_MODE_NONE);

    // Subobject to define depth-stencil state for generic programs
    auto depthStencilSubobject = stateObjectDesc.CreateSubobject<CD3DX12_DEPTH_STENCIL_SUBOBJECT>();
    depthStencilSubobject->SetDepthEnable(true);

    // Subobject to define depth-stencil format for generic programs
    auto depthStencilFormatSubobject = stateObjectDesc.CreateSubobject<CD3DX12_DEPTH_STENCIL_FORMAT_SUBOBJECT>();
    depthStencilFormatSubobject->SetDepthStencilFormat(depthBuffer_->GetDesc().Format);

    // Subobject to define render target formats for generic programs
    auto renderTargetFormatSubobject = stateObjectDesc.CreateSubobject<CD3DX12_RENDER_TARGET_FORMATS_SUBOBJECT>();
    renderTargetFormatSubobject->SetNumRenderTargets(1);
    renderTargetFormatSubobject->SetRenderTargetFormat(0, renderTargets_[0]->GetDesc().Format);

    // Next we'll create two generic program subobject for our two mesh nodes.

    // LineMeshNode
    {
        // The line mesh shader defines the [NodeId(...)] attribute, and thus a generic program that references it
        // will be automatically turned into a work graph mesh node.

        auto lineProgramSubobject = stateObjectDesc.CreateSubobject<CD3DX12_GENERIC_PROGRAM_SUBOBJECT>();

        // Add mesh shader to the generic program.
        // The exportName is the name of our mesh shader function in the shader library.
        lineProgramSubobject->AddExport(L"LineMeshShader");
        // Add the pixel shader to the generic program.
        // The exportName is the entry point name of our pixel shader.
        lineProgramSubobject->AddExport(L"MeshNodePixelShader");

        // Add "building blocks" to define the graphics PSO state for our mesh node
        lineProgramSubobject->AddSubobject(*rasterizerSubobject);
        lineProgramSubobject->AddSubobject(*depthStencilSubobject);
        lineProgramSubobject->AddSubobject(*depthStencilFormatSubobject);
        lineProgramSubobject->AddSubobject(*renderTargetFormatSubobject);
    }

    // TriangleMeshNode
    {
        // The triangle mesh shader does not define a [NodeId(...)] attribute,
        // thus the generic program that we create with it would take the name "TriangleMeshShader".
        // Here we'll rename it to "TriangleMeshNode", which is how other nodes in the graph reference it.

        auto triangleProgramSubobject = stateObjectDesc.CreateSubobject<CD3DX12_GENERIC_PROGRAM_SUBOBJECT>();

        // To later rename the mesh node created with this generic program, we first need to give it a unique name.
        const auto genericProgramName = L"TriangleMeshNodeGenericProgram";
        triangleProgramSubobject->SetProgramName(genericProgramName);

        // Mesh and pixel shader are added in the same way as with the line mesh shader above
        triangleProgramSubobject->AddExport(L"TriangleMeshShader");
        triangleProgramSubobject->AddExport(L"MeshNodePixelShader");

        // Same with the "building blocks" for the graphics PSO state
        triangleProgramSubobject->AddSubobject(*rasterizerSubobject);
        triangleProgramSubobject->AddSubobject(*depthStencilSubobject);
        triangleProgramSubobject->AddSubobject(*depthStencilFormatSubobject);
        triangleProgramSubobject->AddSubobject(*renderTargetFormatSubobject);

        // Next, we need to rename the created mesh node to "TriangleMeshNode".
        // To do this, we need to create a mesh launch override with the same name as our generic program.
        auto triangleNodeOverride = workGraphDesc->CreateMeshLaunchNodeOverrides(genericProgramName);
        // Here we set the name and array index of our mesh node.
        // This name will be used by the rest of the work graph to send records to our mesh node.
        // This override will also remove the implicitly created "TriangleMeshShader" mesh node.
        triangleNodeOverride->NewName({ L"TriangleMeshNode", 0 });
        // Here we could also override other attributes, such as the node dispatch grid,
        // but in our case, those attributes are already set in the HLSL source code.
    }

    HRESULT hr = device_->CreateStateObject(stateObjectDesc, IID_PPV_ARGS(&stateObject));
    ERROR_QUIT((hr == S_OK) && stateObject, "Failed to create Work Graph State Object.");

    return stateObject;
}

D3D12_SET_PROGRAM_DESC HelloMeshNodes::PrepareWorkGraph(CComPtr<ID3D12StateObject> stateObject)
{
    HRESULT hr;

    CComPtr<ID3D12Resource> backingMemoryResource = nullptr;

    CComPtr<ID3D12StateObjectProperties1> stateObjectProperties;
    CComPtr<ID3D12WorkGraphProperties1> workGraphProperties;

    hr = stateObject->QueryInterface(IID_PPV_ARGS(&stateObjectProperties));
    ERROR_QUIT(SUCCEEDED(hr), "Failed to query ID3D12StateObjectProperties1.");
    hr = stateObject->QueryInterface(IID_PPV_ARGS(&workGraphProperties));
    ERROR_QUIT(SUCCEEDED(hr), "Failed to query ID3D12WorkGraphProperties1.");

    // Set the input record limit. This is required for work graphs with mesh nodes.
    // In this case we'll only have a single input record
    const auto workGraphIndex = workGraphProperties->GetWorkGraphIndex(kProgramName);
    workGraphProperties->SetMaximumInputRecords(workGraphIndex, 1, 1);

    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS memoryRequirements = {};
    workGraphProperties->GetWorkGraphMemoryRequirements(workGraphIndex, &memoryRequirements);
    if (memoryRequirements.MaxSizeInBytes > 0)
    {
        backingMemoryResource = d3d12::AllocateBuffer(device_, memoryRequirements.MaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_HEAP_TYPE_DEFAULT);
    }

    D3D12_SET_PROGRAM_DESC setProgramDesc = {};
    setProgramDesc.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
    setProgramDesc.WorkGraph.ProgramIdentifier = stateObjectProperties->GetProgramIdentifier(kProgramName);
    setProgramDesc.WorkGraph.Flags = D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
    if (backingMemoryResource)
    {
        setProgramDesc.WorkGraph.BackingMemory = { backingMemoryResource->GetGPUVirtualAddress(), memoryRequirements.MaxSizeInBytes };
    }

    return setProgramDesc;
}

void HelloMeshNodes::RecordCommandList()
{
    ID3D12Resource* backbuffer = renderTargets_[frameIndex_].p;
    d3d12::TransitionBarrier(commandList_, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    // Setup viewport & scissor
    CD3DX12_VIEWPORT viewport(0.f, 0.f, WindowSize, WindowSize);
    CD3DX12_RECT scissorRect(0, 0, WindowSize, WindowSize);
    commandList_->RSSetViewports(1, &viewport);
    commandList_->RSSetScissorRects(1, &scissorRect);

    // Render view and depth handle
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(renderViewDescriptorHeap_->GetCPUDescriptorHandleForHeapStart(), frameIndex_, descriptorSize_);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(depthDescriptorHeap_->GetCPUDescriptorHandleForHeapStart());

    // Clear render target View.
    const float clearColor[] = { 1, 1, 1, 1 };
    commandList_->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    // Clear depth buffer
    commandList_->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Set depth & color render targets
    commandList_->OMSetRenderTargets(1, &rtvHandle, false, &dsvHandle);

    // Dispatch work graph
    D3D12_DISPATCH_GRAPH_DESC dispatchGraphDesc = {};
    dispatchGraphDesc.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
    dispatchGraphDesc.NodeCPUInput = { };
    dispatchGraphDesc.NodeCPUInput.EntrypointIndex = 0;
    // Launch graph with one record
    dispatchGraphDesc.NodeCPUInput.NumRecords = 1;
    // Record does not contain any data
    dispatchGraphDesc.NodeCPUInput.RecordStrideInBytes = 0;
    dispatchGraphDesc.NodeCPUInput.pRecords = nullptr;

    commandList_->SetGraphicsRootSignature(globalRootSignature_);
    commandList_->SetProgram(&setProgramDesc_);
    commandList_->DispatchGraph(&dispatchGraphDesc);

    d3d12::TransitionBarrier(commandList_, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    // Only initialize in the first frame. Set flag from Init to None for all other frames.
    setProgramDesc_.WorkGraph.Flags = D3D12_SET_WORK_GRAPH_FLAG_NONE;
}

namespace d3d12 {
    ID3DBlob* CompileShader(const std::string& shaderCode, const wchar_t* entryPoint, const wchar_t* targetProfile)
    {
        ID3DBlob* resultBlob = nullptr;
        if (d3d12::sDxCompilerDLL)
        {
            DxcCreateInstanceProc pDxcCreateInstance;
            pDxcCreateInstance = (DxcCreateInstanceProc)GetProcAddress(d3d12::sDxCompilerDLL, "DxcCreateInstance");

            if (pDxcCreateInstance)
            {
                CComPtr<IDxcUtils> pUtils;
                CComPtr<IDxcCompiler> pCompiler;
                CComPtr<IDxcBlobEncoding> pSource;
                CComPtr<IDxcOperationResult> pOperationResult;

                if (SUCCEEDED(pDxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&pUtils))) && SUCCEEDED(pDxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&pCompiler))))
                {
                    if (SUCCEEDED(pUtils->CreateBlob(shaderCode.c_str(), static_cast<uint32_t>(shaderCode.length()), 0, &pSource)))
                    {
                        if (SUCCEEDED(pCompiler->Compile(pSource, nullptr, entryPoint, targetProfile, nullptr, 0, nullptr, 0, nullptr, &pOperationResult)))
                        {
                            HRESULT hr;
                            pOperationResult->GetStatus(&hr);
                            if (SUCCEEDED(hr))
                            {
                                pOperationResult->GetResult((IDxcBlob**)&resultBlob);
                            }
                        }
                    }
                }
            }
        }

        ERROR_QUIT(resultBlob, "Failed to compile GWG Library.");
        return resultBlob;
    }
}