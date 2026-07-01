// WGM3: HDR + 簡易 bloom + トーンマップ。発光プリセット(稲妻)が光る。
cbuffer P : register(b0) {
    float3 camPos; int W;
    float3 camTar; int H;
    float3 light;  int count;
    float  fovTan; float emissive; float2 pad;
};
RWTexture2D<float4> Out : register(u0);
RWTexture2D<float4> Hdr : register(u1);

[numthreads(8,8,1)] void CSMain(uint3 id:SV_DispatchThreadID){
    if((int)id.x>=W||(int)id.y>=H) return;
    float3 c = Hdr[id.xy].rgb;
    float3 bloom = 0; const int N = 28;
    for(int i=0;i<N;++i){
        float a = float(i)*2.39996; float r = (float(i)/N)*20.0;
        int2 q = clamp(int2(id.xy)+int2(cos(a)*r, sin(a)*r), int2(0,0), int2(W-1,H-1));
        bloom += max(Hdr[q].rgb - 0.9, 0.0);
    }
    bloom /= N;
    float3 col = c + bloom*1.6;
    col = 1.0 - exp(-col*1.25);
    Out[id.xy] = float4(col, 1.0);
}
