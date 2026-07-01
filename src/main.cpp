// wggrow — D3D12 Work Graphs (2024) による再帰プロシージャル生成 + SDF レイマーチ。
//
// 種から Work Graphs が枝(capsule)を再帰的に自己生成し、珊瑚/樹/雷が3Dで育つ。
// 生成した capsule 群の union SDF をレイマーチして描画する。
// WGM1: まず再帰生成が効くかを、capsule 本数の readback で検証する。
//
// ヘッドレス capture: --out <png> (M2 以降) / M1 は本数を print。

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
#include <cstring>
#include <vector>
#include <string>
#include <stdexcept>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 615; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

static void chk(HRESULT hr, const char* w){ if(FAILED(hr)){ char b[256]; std::snprintf(b,sizeof(b),"%s hr=0x%08lx",w,(unsigned long)hr); throw std::runtime_error(b);} }
static std::string exeDir(){ char p[MAX_PATH]; GetModuleFileNameA(nullptr,p,MAX_PATH); std::string s=p; auto i=s.find_last_of("\\/"); return i==std::string::npos?std::string("."):s.substr(0,i); }
static std::vector<std::uint8_t> readFile(const char* name){
    std::string p = exeDir()+"\\"+name;   // exe と同じ場所から読む (cwd 非依存)
    FILE* f=std::fopen(p.c_str(),"rb"); if(!f) throw std::runtime_error(std::string("open ")+p);
    std::fseek(f,0,SEEK_END); long n=std::ftell(f); std::fseek(f,0,SEEK_SET);
    std::vector<std::uint8_t> b(n); std::fread(b.data(),1,n,f); std::fclose(f); return b;
}

struct GenParams { std::uint32_t maxCaps, branchN; float spreadAngle, lenScale; float radScale, twist; std::uint32_t rootDepth; float pad0;
                   float rootCol[3]; float pad1; float tipCol[3]; float pad2; };
struct BranchRec { float pos[3]; float dir[3]; float len; float radius; std::uint32_t depth; std::uint32_t seed; };
struct alignas(16) CamP { float camPos[3]; std::int32_t W; float camTar[3]; std::int32_t H; float light[3]; std::int32_t count; float fovTan; float emissive; float growth; float pad; float tail[48]; };
static_assert(sizeof(CamP)==256, "CamP 256");

class App
{
protected:
    static constexpr UINT kMaxCaps = 30000;

    void dumpMessages()
    {
        if(!m_iq) return;
        UINT64 n=m_iq->GetNumStoredMessages();
        for(UINT64 i=0;i<n;++i){ SIZE_T len=0; m_iq->GetMessage(i,nullptr,&len);
            std::vector<char> buf(len); auto* m=reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            m_iq->GetMessage(i,m,&len); std::printf("  [dbg] %s\n", m->pDescription); }
        m_iq->ClearStoredMessages();
    }
    void createDevice()
    {
        ComPtr<ID3D12Debug> dbg; if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
        ComPtr<IDXGIFactory6> factory; chk(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
        ComPtr<IDXGIAdapter1> a, chosen;
        for(UINT i=0; factory->EnumAdapters1(i,&a)!=DXGI_ERROR_NOT_FOUND; ++i){
            DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
            if(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if(std::wstring(d.Description).find(L"NVIDIA")!=std::wstring::npos){ chosen=a; break; }
            if(!chosen) chosen=a;
        }
        chk(D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_dev)), "device9");
        m_dev.As(&m_iq);   // debug info queue (デバッグレイヤ有効時のみ成功)
        D3D12_FEATURE_DATA_D3D12_OPTIONS21 o{}; m_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21,&o,sizeof(o));
        if(o.WorkGraphsTier==D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED) throw std::runtime_error("Work Graphs not supported");
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        chk(m_dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&m_q)),"queue");
        chk(m_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&m_alloc)),"alloc");
        chk(m_dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,m_alloc.Get(),nullptr,IID_PPV_ARGS(&m_cl)),"cl");
        m_cl->Close();
        chk(m_dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&m_fence)),"fence");
        m_ev=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    }

    void createGraph()
    {
        // root sig: b0 CBV, u0 Caps, u1 Counter
        CD3DX12_ROOT_PARAMETER1 rp[3];
        rp[0].InitAsConstantBufferView(0);
        rp[1].InitAsUnorderedAccessView(0);
        rp[2].InitAsUnorderedAccessView(1);
        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsd; rsd.Init_1_1(3, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err; chk(D3D12SerializeVersionedRootSignature(&rsd,&sig,&err),"serialize rs");
        chk(m_dev->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&m_rootSig)),"rootsig");

        auto dxil = readFile("wg_grow.dxil");
        CD3DX12_STATE_OBJECT_DESC so(D3D12_STATE_OBJECT_TYPE_EXECUTABLE);
        auto lib = so.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
        D3D12_SHADER_BYTECODE bc{dxil.data(), dxil.size()}; lib->SetDXILLibrary(&bc);
        auto grs = so.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>(); grs->SetRootSignature(m_rootSig.Get());
        auto wg = so.CreateSubobject<CD3DX12_WORK_GRAPH_SUBOBJECT>(); wg->IncludeAllAvailableNodes(); wg->SetProgramName(L"Grow");
        HRESULT hrSO=m_dev->CreateStateObject(so, IID_PPV_ARGS(&m_so)); dumpMessages(); chk(hrSO,"CreateStateObject");
        chk(m_so.As(&m_soProps),"props1"); chk(m_so.As(&m_wgProps),"wgprops");
        m_wgIndex = m_wgProps->GetWorkGraphIndex(L"Grow");
        m_wgProps->GetWorkGraphMemoryRequirements(m_wgIndex, &m_memReq);
        m_pid = m_soProps->GetProgramIdentifier(L"Grow");
    }

    ComPtr<ID3D12Resource> makeBuf(UINT64 bytes, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st, D3D12_HEAP_TYPE heap=D3D12_HEAP_TYPE_DEFAULT)
    {
        CD3DX12_HEAP_PROPERTIES hp(heap); CD3DX12_RESOURCE_DESC rd=CD3DX12_RESOURCE_DESC::Buffer(bytes, fl);
        ComPtr<ID3D12Resource> r; chk(m_dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,st,nullptr,IID_PPV_ARGS(&r)),"buf");
        return r;
    }
    void createBuffers()
    {
        m_caps    = makeBuf(UINT64(kMaxCaps)*48, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_counter = makeBuf(16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if(m_memReq.MaxSizeInBytes) m_backing = makeBuf(m_memReq.MaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_cb      = makeBuf(256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);
        m_zero    = makeBuf(16, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);
        void* z=nullptr; D3D12_RANGE none{0,0}; m_zero->Map(0,&none,&z); std::memset(z,0,16); m_zero->Unmap(0,nullptr);
    }

    // プリセット = いろんな生成物 (樹/珊瑚/藪/稲妻)。gen params + 色 + 発光 + 根の初期値
    void applyPreset(const std::string& name)
    {
        // 既定 = 樹
        m_gp = { kMaxCaps, 2u, 0.6f, 0.80f, 0.72f, 0.5f, 9u, 0.0f,
                 {0.45f,0.28f,0.16f}, 0.0f, {0.35f,0.95f,0.55f}, 0.0f };
        m_rootLen=0.32f; m_rootRad=0.045f; m_emissive=0.0f;
        if(name=="coral"){
            m_gp.branchN=3; m_gp.spreadAngle=0.85f; m_gp.lenScale=0.82f; m_gp.radScale=0.80f; m_gp.twist=1.3f; m_gp.rootDepth=6;
            m_gp.rootCol[0]=0.85f;m_gp.rootCol[1]=0.30f;m_gp.rootCol[2]=0.38f; m_gp.tipCol[0]=1.0f;m_gp.tipCol[1]=0.72f;m_gp.tipCol[2]=0.72f;
            m_rootLen=0.30f; m_rootRad=0.05f;
        } else if(name=="bush"){
            m_gp.branchN=3; m_gp.spreadAngle=1.1f; m_gp.lenScale=0.74f; m_gp.radScale=0.72f; m_gp.twist=2.2f; m_gp.rootDepth=6;
            m_gp.rootCol[0]=0.25f;m_gp.rootCol[1]=0.45f;m_gp.rootCol[2]=0.18f; m_gp.tipCol[0]=0.55f;m_gp.tipCol[1]=1.0f;m_gp.tipCol[2]=0.45f;
            m_rootLen=0.24f; m_rootRad=0.04f;
        } else if(name=="lightning"){
            m_gp.branchN=2; m_gp.spreadAngle=0.32f; m_gp.lenScale=0.92f; m_gp.radScale=0.86f; m_gp.twist=0.35f; m_gp.rootDepth=10;
            m_gp.rootCol[0]=0.55f;m_gp.rootCol[1]=0.7f;m_gp.rootCol[2]=1.0f; m_gp.tipCol[0]=0.85f;m_gp.tipCol[1]=0.92f;m_gp.tipCol[2]=1.0f;
            m_rootLen=0.42f; m_rootRad=0.02f; m_emissive=3.0f;
        }
        void* p=nullptr; D3D12_RANGE none{0,0}; m_cb->Map(0,&none,&p); std::memcpy(p,&m_gp,sizeof(m_gp)); m_cb->Unmap(0,nullptr);
    }

    void generate()
    {
        m_alloc->Reset(); m_cl->Reset(m_alloc.Get(), nullptr);
        // counter を 0 クリア
        auto b1=CD3DX12_RESOURCE_BARRIER::Transition(m_counter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        m_cl->ResourceBarrier(1,&b1);
        m_cl->CopyBufferRegion(m_counter.Get(),0,m_zero.Get(),0,16);
        auto b2=CD3DX12_RESOURCE_BARRIER::Transition(m_counter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_cl->ResourceBarrier(1,&b2);

        m_cl->SetComputeRootSignature(m_rootSig.Get());
        m_cl->SetComputeRootConstantBufferView(0, m_cb->GetGPUVirtualAddress());
        m_cl->SetComputeRootUnorderedAccessView(1, m_caps->GetGPUVirtualAddress());
        m_cl->SetComputeRootUnorderedAccessView(2, m_counter->GetGPUVirtualAddress());
        D3D12_SET_PROGRAM_DESC sp{}; sp.Type=D3D12_PROGRAM_TYPE_WORK_GRAPH;
        sp.WorkGraph.ProgramIdentifier=m_pid; sp.WorkGraph.Flags=D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
        if(m_backing) sp.WorkGraph.BackingMemory={ m_backing->GetGPUVirtualAddress(), m_memReq.MaxSizeInBytes };
        m_cl->SetProgram(&sp);

        BranchRec root{ {0,0,0}, {0,1,0}, m_rootLen, m_rootRad, m_gp.rootDepth, 1234u };
        D3D12_DISPATCH_GRAPH_DESC dg{}; dg.Mode=D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
        dg.NodeCPUInput.EntrypointIndex=0; dg.NodeCPUInput.NumRecords=1;
        dg.NodeCPUInput.pRecords=&root; dg.NodeCPUInput.RecordStrideInBytes=sizeof(BranchRec);
        m_cl->DispatchGraph(&dg);
        chk(m_cl->Close(),"close gen"); submit();
    }

    void submit()
    {
        ID3D12CommandList* l[]={m_cl.Get()}; m_q->ExecuteCommandLists(1,l);
        const UINT64 v=++m_fenceVal; m_q->Signal(m_fence.Get(),v);
        if(m_fence->GetCompletedValue()<v){ m_fence->SetEventOnCompletion(v,m_ev); WaitForSingleObject(m_ev,INFINITE); }
    }

    std::uint32_t readbackCount()
    {
        auto rb=makeBuf(16, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK);
        m_alloc->Reset(); m_cl->Reset(m_alloc.Get(),nullptr);
        auto b=CD3DX12_RESOURCE_BARRIER::Transition(m_counter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_cl->ResourceBarrier(1,&b);
        m_cl->CopyBufferRegion(rb.Get(),0,m_counter.Get(),0,16);
        auto b3=CD3DX12_RESOURCE_BARRIER::Transition(m_counter.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_cl->ResourceBarrier(1,&b3);
        chk(m_cl->Close(),"close rbc"); submit();
        std::uint32_t* mp=nullptr; D3D12_RANGE rr{0,4}; rb->Map(0,&rr,reinterpret_cast<void**>(&mp));
        std::uint32_t c=mp[0]; rb->Unmap(0,nullptr); return c;
    }

    void printSampleCapsules(int n)
    {
        const UINT64 bytes=UINT64(kMaxCaps)*48;
        auto rb=makeBuf(bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK);
        m_alloc->Reset(); m_cl->Reset(m_alloc.Get(),nullptr);
        auto b=CD3DX12_RESOURCE_BARRIER::Transition(m_caps.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_cl->ResourceBarrier(1,&b);
        m_cl->CopyBufferRegion(rb.Get(),0,m_caps.Get(),0,bytes);
        chk(m_cl->Close(),"close caps"); submit();
        float* mp=nullptr; D3D12_RANGE rr{0,SIZE_T(n*12*4)}; rb->Map(0,&rr,reinterpret_cast<void**>(&mp));
        for(int i=0;i<n;++i){ float* c=mp+i*12;
            std::printf("  cap[%d] a=(%.2f,%.2f,%.2f) rA=%.3f  b=(%.2f,%.2f,%.2f) depth=%.0f\n",
                        i, c[0],c[1],c[2], c[3], c[4],c[5],c[6], c[11]); }
        rb->Unmap(0,nullptr);
    }
    // ── レイマーチ描画 (WGM2) ────────────────────────────────────────────
    void createRaymarch()
    {
        CD3DX12_DESCRIPTOR_RANGE1 uavR(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
        CD3DX12_ROOT_PARAMETER1 rp[3];
        rp[0].InitAsConstantBufferView(0);      // b0 camera
        rp[1].InitAsShaderResourceView(0);      // t0 capsules (root SRV)
        rp[2].InitAsDescriptorTable(1, &uavR);  // u0 out tex
        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsd; rsd.Init_1_1(3, rp, 0, nullptr);
        ComPtr<ID3DBlob> sig, err; chk(D3D12SerializeVersionedRootSignature(&rsd,&sig,&err),"rm rs");
        chk(m_dev->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&m_rmRoot)),"rm rootsig");
        auto cs = readFile("raymarch.dxil");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=m_rmRoot.Get(); pd.CS={cs.data(),cs.size()};
        chk(m_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_rmPso)),"rm pso");

        // composite root sig: b0 CBV + table(u0=out, u1=hdr)
        CD3DX12_DESCRIPTOR_RANGE1 cr(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0);
        CD3DX12_ROOT_PARAMETER1 cp2[2]; cp2[0].InitAsConstantBufferView(0); cp2[1].InitAsDescriptorTable(1,&cr);
        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC crsd; crsd.Init_1_1(2, cp2, 0, nullptr);
        ComPtr<ID3DBlob> csig, cerr; chk(D3D12SerializeVersionedRootSignature(&crsd,&csig,&cerr),"comp rs");
        chk(m_dev->CreateRootSignature(0,csig->GetBufferPointer(),csig->GetBufferSize(),IID_PPV_ARGS(&m_compRoot)),"comp rootsig");
        auto ccs = readFile("composite.dxil");
        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{}; cpd.pRootSignature=m_compRoot.Get(); cpd.CS={ccs.data(),ccs.size()};
        chk(m_dev->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&m_compPso)),"comp pso");

        CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_DEFAULT);
        auto mkTex=[&](DXGI_FORMAT f){ CD3DX12_RESOURCE_DESC td=CD3DX12_RESOURCE_DESC::Tex2D(f, kImgW, kImgH, 1,1);
            td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; ComPtr<ID3D12Resource> r;
            chk(m_dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&r)),"tex"); return r; };
        m_out = mkTex(DXGI_FORMAT_R8G8B8A8_UNORM);
        m_hdr = mkTex(DXGI_FORMAT_R16G16B16A16_FLOAT);
        // heap: [0]=out UAV, [1]=hdr UAV
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors=2; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        chk(m_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap)),"srvheap");
        m_srvInc=m_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpuH=[&](UINT i){ auto h=m_srvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr+=SIZE_T(i)*m_srvInc; return h; };
        D3D12_UNORDERED_ACCESS_VIEW_DESC uo{}; uo.Format=DXGI_FORMAT_R8G8B8A8_UNORM; uo.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        m_dev->CreateUnorderedAccessView(m_out.Get(),nullptr,&uo, cpuH(0));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uh{}; uh.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; uh.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        m_dev->CreateUnorderedAccessView(m_hdr.Get(),nullptr,&uh, cpuH(1));
        m_camCb = makeBuf(256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu(UINT i){ auto h=m_srvHeap->GetGPUDescriptorHandleForHeapStart(); h.ptr+=UINT64(i)*m_srvInc; return h; }

    void render(float growth=1.0f)
    {
        CamP cp{}; cp.W=kImgW; cp.H=kImgH; cp.count=(std::int32_t)m_count; cp.fovTan=0.5f; cp.emissive=m_emissive; cp.growth=growth;
        cp.camPos[0]=1.75f; cp.camPos[1]=0.85f; cp.camPos[2]=1.75f;
        cp.camTar[0]=0.0f;  cp.camTar[1]=0.62f; cp.camTar[2]=0.0f;
        cp.light[0]=0.55f;  cp.light[1]=0.8f;   cp.light[2]=0.42f;
        void* p=nullptr; D3D12_RANGE none{0,0}; m_camCb->Map(0,&none,&p); std::memcpy(p,&cp,sizeof(cp)); m_camCb->Unmap(0,nullptr);

        m_alloc->Reset(); m_cl->Reset(m_alloc.Get(), nullptr);
        ID3D12DescriptorHeap* heaps[]={m_srvHeap.Get()}; m_cl->SetDescriptorHeaps(1,heaps);
        // raymarch → hdr (table u0 = hdr = slot1)
        m_cl->SetComputeRootSignature(m_rmRoot.Get()); m_cl->SetPipelineState(m_rmPso.Get());
        m_cl->SetComputeRootConstantBufferView(0, m_camCb->GetGPUVirtualAddress());
        m_cl->SetComputeRootShaderResourceView(1, m_caps->GetGPUVirtualAddress());
        m_cl->SetComputeRootDescriptorTable(2, srvGpu(1));
        m_cl->Dispatch((kImgW+7)/8, (kImgH+7)/8, 1);
        auto hb=CD3DX12_RESOURCE_BARRIER::UAV(m_hdr.Get()); m_cl->ResourceBarrier(1,&hb);
        // composite (hdr → out, bloom+tonemap; table u0=out u1=hdr = slot0..1)
        m_cl->SetComputeRootSignature(m_compRoot.Get()); m_cl->SetPipelineState(m_compPso.Get());
        m_cl->SetComputeRootConstantBufferView(0, m_camCb->GetGPUVirtualAddress());
        m_cl->SetComputeRootDescriptorTable(1, srvGpu(0));
        m_cl->Dispatch((kImgW+7)/8, (kImgH+7)/8, 1);
        auto b=CD3DX12_RESOURCE_BARRIER::Transition(m_out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_cl->ResourceBarrier(1,&b);
        chk(m_cl->Close(),"close render"); submit();
    }

    void savePNG(const std::string& out)
    {
        const UINT rowBytes=kImgW*4, rowPitch=(rowBytes+255)&~255u; const UINT64 total=UINT64(rowPitch)*kImgH;
        auto rb=makeBuf(total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK);
        m_alloc->Reset(); m_cl->Reset(m_alloc.Get(),nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=rb.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format=DXGI_FORMAT_R8G8B8A8_UNORM; dst.PlacedFootprint.Footprint.Width=kImgW;
        dst.PlacedFootprint.Footprint.Height=kImgH; dst.PlacedFootprint.Footprint.Depth=1; dst.PlacedFootprint.Footprint.RowPitch=rowPitch;
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=m_out.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex=0;
        m_cl->CopyTextureRegion(&dst,0,0,0,&src,nullptr); chk(m_cl->Close(),"close save"); submit();
        std::uint8_t* mp=nullptr; D3D12_RANGE rr{0,SIZE_T(total)}; rb->Map(0,&rr,reinterpret_cast<void**>(&mp));
        std::vector<std::uint8_t> tight(size_t(kImgW)*kImgH*4);
        for(int y=0;y<kImgH;++y) std::memcpy(&tight[size_t(y)*rowBytes], mp+size_t(y)*rowPitch, rowBytes);
        rb->Unmap(0,nullptr);
        if(!stbi_write_png(out.c_str(), kImgW, kImgH, 4, tight.data(), kImgW*4)) throw std::runtime_error("png");
    }

public:
    void runRender(const std::string& out, const std::string& preset)
    {
        createDevice(); createGraph(); createBuffers();
        applyPreset(preset); generate(); m_count=readbackCount();
        std::printf("[wggrow] preset=%s capsules=%u -> raymarch %dx%d\n", preset.c_str(), m_count, kImgW, kImgH);
        createRaymarch(); render(1.0f); savePNG(out);
        std::printf("[wggrow] wrote %s\n", out.c_str());
    }

    // 成長アニメ連番 (GPU 再帰生成した構造が根→先端へ育つ)
    void runSeq(const std::string& dir, int frames, const std::string& preset)
    {
        createDevice(); createGraph(); createBuffers();
        applyPreset(preset); generate(); m_count=readbackCount(); createRaymarch();
        for(int f=0; f<frames; ++f){
            float g = float(f)/float(frames-1);      // 0→1 で成長
            render(g);
            char path[512]; std::snprintf(path,sizeof(path),"%s/f_%04d.png",dir.c_str(),f);
            savePNG(path);
        }
        std::printf("[wggrow] preset=%s wrote %d frames to %s (caps=%u)\n", preset.c_str(), frames, dir.c_str(), m_count);
    }

private:
    static constexpr int kImgW=720, kImgH=720;
    ComPtr<ID3D12Device9> m_dev; ComPtr<ID3D12CommandQueue> m_q;
    ComPtr<ID3D12CommandAllocator> m_alloc; ComPtr<ID3D12GraphicsCommandList10> m_cl;
    ComPtr<ID3D12Fence> m_fence; HANDLE m_ev=nullptr; UINT64 m_fenceVal=0; ComPtr<ID3D12InfoQueue> m_iq;
    ComPtr<ID3D12RootSignature> m_rootSig; ComPtr<ID3D12StateObject> m_so;
    ComPtr<ID3D12StateObjectProperties1> m_soProps; ComPtr<ID3D12WorkGraphProperties> m_wgProps;
    UINT m_wgIndex=0; D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS m_memReq{}; D3D12_PROGRAM_IDENTIFIER m_pid{};
    ComPtr<ID3D12Resource> m_caps, m_counter, m_backing, m_cb, m_zero;
    GenParams m_gp{}; std::uint32_t m_count=0;
    float m_emissive=0.0f, m_rootLen=0.32f, m_rootRad=0.045f;
    ComPtr<ID3D12RootSignature> m_rmRoot; ComPtr<ID3D12PipelineState> m_rmPso;
    ComPtr<ID3D12RootSignature> m_compRoot; ComPtr<ID3D12PipelineState> m_compPso;
    ComPtr<ID3D12Resource> m_out, m_hdr, m_camCb; ComPtr<ID3D12DescriptorHeap> m_srvHeap; UINT m_srvInc=0;
};

int main(int argc, char** argv)
{
    std::string out="wg.png", preset="tree", seq="";
    int frames=1;
    for(int i=1;i<argc;++i){ std::string a=argv[i];
        if(a=="--out"&&i+1<argc) out=argv[++i];
        else if(a=="--preset"&&i+1<argc) preset=argv[++i];
        else if(a=="--seq"&&i+1<argc) seq=argv[++i];
        else if(a=="--frames"&&i+1<argc) frames=std::atoi(argv[++i]); }
    try { App app; if(!seq.empty()) app.runSeq(seq, frames, preset); else app.runRender(out, preset); }
    catch(const std::exception& e){ std::fprintf(stderr,"[wggrow] ERROR: %s\n", e.what()); return 1; }
    return 0;
}
