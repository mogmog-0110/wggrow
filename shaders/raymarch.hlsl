// WGM2: Work Graphs が生成した capsule 群の union SDF をスフィアトレースで描画。
// 法線 + soft shadow + AO で立体的に見せる。

cbuffer P : register(b0) {
    float3 camPos; int W;
    float3 camTar; int H;
    float3 light;  int count;
    float  fovTan; float emissive; float growth; int rW;   // rW/rH = 内部描画解像度 (窓より低くしてアップスケール)
    int    rH;     float3 pad;
};
struct Capsule { float4 a; float4 b; float4 col; };  // a.xyz,a.w=rA / b.xyz,b.w=rB / col.rgb,col.w=depth
StructuredBuffer<Capsule> Caps : register(t0);
RWTexture2D<float4> Hdr : register(u0);   // HDR 出力 (トーンマップは composite で)

float sdCap(float3 p, float3 a, float3 b, float ra, float rb){
    float3 pa=p-a, ba=b-a; float h=saturate(dot(pa,ba)/max(dot(ba,ba),1e-6));
    return length(pa-ba*h) - lerp(ra,rb,h);
}
float mapD(float3 p, out float3 col){
    float d=1e9; col=float3(0.6,0.6,0.6);
    for(int i=0;i<count;++i){ Capsule c=Caps[i];
        float g = saturate((growth*1.2 - c.col.w)/0.2);   // 根→先端へ伸びる成長前線
        if(g<=0.002) continue;
        float3 bb = lerp(c.a.xyz, c.b.xyz, g);
        float dc = sdCap(p, c.a.xyz, bb, c.a.w, lerp(c.a.w, c.b.w, g));
        if(dc<d){ d=dc; col=c.col.rgb; } }
    return d;
}
float mapOnly(float3 p){ float3 c; return mapD(p,c); }
float3 calcN(float3 p){ float e=0.0025;
    return normalize(float3(
        mapOnly(p+float3(e,0,0))-mapOnly(p-float3(e,0,0)),
        mapOnly(p+float3(0,e,0))-mapOnly(p-float3(0,e,0)),
        mapOnly(p+float3(0,0,e))-mapOnly(p-float3(0,0,e)))); }

[numthreads(8,8,1)] void CSMain(uint3 id:SV_DispatchThreadID){
    if((int)id.x>=rW||(int)id.y>=rH) return;
    float2 ndc=float2((id.x+0.5)/rW*2-1, 1-(id.y+0.5)/rH*2);
    float aspect=float(rW)/rH;
    float3 fwd=normalize(camTar-camPos), rgt=normalize(cross(float3(0,1,0),fwd)), up=cross(fwd,rgt);
    float3 rd=normalize(fwd + ndc.x*fovTan*aspect*rgt + ndc.y*fovTan*up), ro=camPos;
    float3 bg=lerp(float3(0.03,0.04,0.06), float3(0.09,0.11,0.15), saturate(ndc.y*0.5+0.5));
    float3 col=bg; float t=0.02; bool hit=false; float3 hitcol=0; float3 p=ro;
    [loop] for(int s=0;s<160;++s){ p=ro+rd*t; float3 cc; float d=mapD(p,cc);
        if(d<0.0014){ hit=true; hitcol=cc; break; } t+=d; if(t>7.0) break; }
    if(hit && emissive>0.0){
        // 発光プリセット (稲妻): ライティングせず光らせる。芯を白飛びさせて縁に色
        float3 n=calcN(p); float fres=pow(saturate(1.0+dot(rd,n)),1.5);
        col = hitcol*emissive*(0.6+1.4*fres) + float3(0.9,0.95,1.0)*emissive*0.5;
        float fog=saturate(t/7.0); col=lerp(col,bg,fog*0.25);
    } else if(hit){
        float3 n=calcN(p); float3 L=normalize(light);
        float dif=saturate(dot(n,L));
        float sh=1.0, tt=0.02;
        [loop] for(int s=0;s<16;++s){ float d=mapOnly(p+n*0.01+L*tt); sh=min(sh, 14.0*d/tt); if(d<0.001||tt>3.0) break; tt+=max(d,0.02); }
        sh=saturate(sh);
        float ao=0, sca=1;
        [unroll] for(int k=1;k<=4;++k){ float hh=0.018*k; float dd=mapOnly(p+n*hh); ao+=(hh-dd)*sca; sca*=0.6; }
        ao=saturate(1.0-2.2*ao);
        float3 amb=float3(0.22,0.26,0.32)*ao;
        col = hitcol*(amb + dif*sh*float3(1.05,0.98,0.88));
        col += pow(saturate(1.0+dot(rd,n)),3.0)*0.12;   // rim
        float fog=saturate(t/7.0); col=lerp(col,bg,fog*0.35);
    }
    Hdr[id.xy]=float4(col,1.0);   // raw HDR (内部解像度で書く)
}
