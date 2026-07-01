// WG-Spike — D3D12 Work Graphs の feasibility 確認。
// Agility SDK で Device9 を作り WorkGraphsTier を確認、最小 work graph (broadcasting ノード) を
// 実行して UAV に書けるかを headless で検証する。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dx12/d3dx12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

// Agility SDK: exe の隣の .\D3D12\ から D3D12Core.dll を読む
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 615; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

static void chk(HRESULT hr, const char* w){ if(FAILED(hr)){ std::printf("[spike] FAIL %s hr=0x%08lx\n",w,(unsigned long)hr); std::exit(2);} }

static std::vector<std::uint8_t> readFile(const char* p){
    FILE* f=std::fopen(p,"rb"); if(!f){ std::printf("[spike] cannot open %s\n",p); std::exit(2);}
    std::fseek(f,0,SEEK_END); long n=std::ftell(f); std::fseek(f,0,SEEK_SET);
    std::vector<std::uint8_t> b(n); std::fread(b.data(),1,n,f); std::fclose(f); return b;
}

int main()
{
    // ── デバイス (NVIDIA アダプタを明示選択) ──
    ComPtr<IDXGIFactory6> factory; chk(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter1> adapter, chosen;
    for(UINT i=0; factory->EnumAdapters1(i,&adapter)!=DXGI_ERROR_NOT_FOUND; ++i){
        DXGI_ADAPTER_DESC1 d; adapter->GetDesc1(&d);
        if(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        std::wstring name=d.Description;
        if(name.find(L"NVIDIA")!=std::wstring::npos){ chosen=adapter; break; }
        if(!chosen) chosen=adapter;
    }
    ComPtr<ID3D12Device9> dev;
    chk(D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)), "device9");
    { DXGI_ADAPTER_DESC1 d; chosen->GetDesc1(&d); std::wprintf(L"[spike] adapter: %s\n", d.Description); }

    // ── WorkGraphsTier ──
    D3D12_FEATURE_DATA_D3D12_OPTIONS21 o21{};
    chk(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21,&o21,sizeof(o21)), "opt21");
    std::printf("[spike] WorkGraphsTier = %d (0=NotSupported, 1=Tier1.0)\n", (int)o21.WorkGraphsTier);
    if(o21.WorkGraphsTier==D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED){ std::printf("[spike] Work Graphs NOT supported -> stop\n"); return 1; }

    // ── ルートシグネチャ (u0 = root UAV) ──
    CD3DX12_ROOT_PARAMETER1 rp[1]; rp[0].InitAsUnorderedAccessView(0);
    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsd; rsd.Init_1_1(1, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    ComPtr<ID3DBlob> sig, err;
    chk(D3D12SerializeVersionedRootSignature(&rsd, &sig, &err), "serialize rs");
    ComPtr<ID3D12RootSignature> rootSig;
    chk(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rootSig)), "rootsig");

    // ── work graph state object ──
    auto dxil = readFile("wg_spike.dxil");
    CD3DX12_STATE_OBJECT_DESC so(D3D12_STATE_OBJECT_TYPE_EXECUTABLE);
    auto lib = so.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    D3D12_SHADER_BYTECODE bc{ dxil.data(), dxil.size() }; lib->SetDXILLibrary(&bc);
    auto grs = so.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>(); grs->SetRootSignature(rootSig.Get());
    auto wg = so.CreateSubobject<CD3DX12_WORK_GRAPH_SUBOBJECT>();
    wg->IncludeAllAvailableNodes(); wg->SetProgramName(L"WG");
    ComPtr<ID3D12StateObject> stateObj;
    chk(dev->CreateStateObject(so, IID_PPV_ARGS(&stateObj)), "CreateStateObject");

    ComPtr<ID3D12StateObjectProperties1> props1; chk(stateObj.As(&props1), "props1");
    ComPtr<ID3D12WorkGraphProperties> wgp;       chk(stateObj.As(&wgp), "wgprops");
    UINT wgIndex = wgp->GetWorkGraphIndex(L"WG");
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS req{}; wgp->GetWorkGraphMemoryRequirements(wgIndex, &req);
    std::printf("[spike] backing memory max = %llu bytes\n", (unsigned long long)req.MaxSizeInBytes);
    D3D12_PROGRAM_IDENTIFIER pid = props1->GetProgramIdentifier(L"WG");

    // ── バッファ (out 64 uint, backing memory) ──
    auto makeUav=[&](UINT64 bytes){
        CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_DEFAULT);
        CD3DX12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> r;
        chk(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&r)),"uavbuf");
        return r;
    };
    ComPtr<ID3D12Resource> outBuf = makeUav(64*sizeof(UINT));
    ComPtr<ID3D12Resource> backing = req.MaxSizeInBytes? makeUav(req.MaxSizeInBytes) : nullptr;

    // ── コマンド ──
    ComPtr<ID3D12CommandQueue> q; D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    chk(dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)),"queue");
    ComPtr<ID3D12CommandAllocator> alloc; chk(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)),"alloc");
    ComPtr<ID3D12GraphicsCommandList10> cl;
    chk(dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&cl)),"cl10");

    cl->SetComputeRootSignature(rootSig.Get());
    cl->SetComputeRootUnorderedAccessView(0, outBuf->GetGPUVirtualAddress());
    D3D12_SET_PROGRAM_DESC sp{}; sp.Type=D3D12_PROGRAM_TYPE_WORK_GRAPH;
    sp.WorkGraph.ProgramIdentifier=pid; sp.WorkGraph.Flags=D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
    if(backing) sp.WorkGraph.BackingMemory = { backing->GetGPUVirtualAddress(), req.MaxSizeInBytes };
    cl->SetProgram(&sp);

    struct Rec { UINT seed; } rec{ 100 };
    D3D12_DISPATCH_GRAPH_DESC dg{}; dg.Mode=D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
    dg.NodeCPUInput.EntrypointIndex = 0;
    dg.NodeCPUInput.NumRecords = 1;
    dg.NodeCPUInput.pRecords = &rec;
    dg.NodeCPUInput.RecordStrideInBytes = sizeof(Rec);
    cl->DispatchGraph(&dg);

    // out -> readback
    CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(outBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->ResourceBarrier(1,&b);
    CD3DX12_HEAP_PROPERTIES rbhp(D3D12_HEAP_TYPE_READBACK);
    CD3DX12_RESOURCE_DESC rbrd = CD3DX12_RESOURCE_DESC::Buffer(64*sizeof(UINT));
    ComPtr<ID3D12Resource> rb; chk(dev->CreateCommittedResource(&rbhp,D3D12_HEAP_FLAG_NONE,&rbrd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&rb)),"rb");
    cl->CopyResource(rb.Get(), outBuf.Get());
    chk(cl->Close(),"close");

    ID3D12CommandList* lists[]={cl.Get()}; q->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence; chk(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    HANDLE ev=CreateEventW(nullptr,FALSE,FALSE,nullptr); q->Signal(fence.Get(),1);
    if(fence->GetCompletedValue()<1){ fence->SetEventOnCompletion(1,ev); WaitForSingleObject(ev,INFINITE); }

    UINT* mp=nullptr; D3D12_RANGE rr{0,64*sizeof(UINT)}; rb->Map(0,&rr,reinterpret_cast<void**>(&mp));
    std::printf("[spike] out[0..7] = %u %u %u %u %u %u %u %u\n", mp[0],mp[1],mp[2],mp[3],mp[4],mp[5],mp[6],mp[7]);
    bool ok = (mp[0]==100 && mp[1]==101 && mp[7]==107);
    rb->Unmap(0,nullptr);
    std::printf(ok? "[spike] SUCCESS: work graph ran (expected 100 101 ... 107)\n" : "[spike] MISMATCH: graph did not write expected values\n");
    return ok?0:1;
}
