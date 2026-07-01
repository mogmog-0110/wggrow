// WGM3: HDR(内部解像度 rW×rH) を窓解像度 W×H へアップスケール + 簡易 bloom + トーンマップ。
cbuffer P : register(b0) {
    float3 camPos; int W;
    float3 camTar; int H;
    float3 light;  int count;
    float  fovTan; float emissive; float growth; int rW;
    int    rH;     float3 pad;
};
RWTexture2D<float4> Out : register(u0);
RWTexture2D<float4> Hdr : register(u1);   // 有効領域は [0,rW)x[0,rH)

// 窓ピクセル → hdr 内部解像度をバイリニアサンプル
float3 sampleHdr(float2 uv){
    float2 fp = uv*float2(rW,rH) - 0.5;
    int2 ip = (int2)floor(fp); float2 f = fp - ip;
    int2 mx = int2(rW-1,rH-1);
    float3 c00=Hdr[clamp(ip+int2(0,0),int2(0,0),mx)].rgb;
    float3 c10=Hdr[clamp(ip+int2(1,0),int2(0,0),mx)].rgb;
    float3 c01=Hdr[clamp(ip+int2(0,1),int2(0,0),mx)].rgb;
    float3 c11=Hdr[clamp(ip+int2(1,1),int2(0,0),mx)].rgb;
    return lerp(lerp(c00,c10,f.x), lerp(c01,c11,f.x), f.y);
}

[numthreads(8,8,1)] void CSMain(uint3 id:SV_DispatchThreadID){
    if((int)id.x>=W||(int)id.y>=H) return;
    float2 uv = (float2(id.xy)+0.5)/float2(W,H);
    float3 c = sampleHdr(uv);
    float3 bloom = 0; const int N = 24;
    float2 texel = 1.0/float2(rW,rH);
    for(int i=0;i<N;++i){
        float a = float(i)*2.39996; float r = (float(i)/N)*18.0;
        bloom += max(sampleHdr(uv + float2(cos(a),sin(a))*r*texel) - 0.9, 0.0);
    }
    bloom /= N;
    float3 col = c + bloom*1.6;
    col = 1.0 - exp(-col*1.25);
    Out[id.xy] = float4(col, 1.0);
}
