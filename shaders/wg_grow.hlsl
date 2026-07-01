// WGM1: 再帰 Work Graph が枝(capsule)を自己生成する。
// Branch ノードが 1 本の枝を capsule バッファへ atomic append し、depth>0 なら
// 円錐状に散らした子 record を自分自身へ emit する (NodeMaxRecursionDepth)。
// これが Work Graphs の殺し技 = データ依存で仕事が再帰的に増える。

struct BranchRec {
    float3 pos; float3 dir; float len; float radius; uint depth; uint seed;
};
struct Capsule {
    float4 a;   // xyz=端点A, w=半径A
    float4 b;   // xyz=端点B, w=半径B
    float4 col; // rgb=色, w=depth
};

RWStructuredBuffer<Capsule> Caps    : register(u0);
RWByteAddressBuffer         Counter : register(u1);

cbuffer P : register(b0) {
    uint  maxCaps; uint branchN; float spreadAngle; float lenScale;
    float radScale; float twist; uint rootDepth; float pad0;
    float3 rootCol; float pad1;
    float3 tipCol;  float pad2;
};

uint hashu(uint x){ x^=x>>16; x*=0x7feb352du; x^=x>>15; x*=0x846ca68bu; x^=x>>16; return x; }
float rnd(uint s){ return (hashu(s) & 0xffffffu) / 16777216.0; }

[Shader("node")]
[NodeLaunch("thread")]
[NodeIsProgramEntry]
[NodeMaxRecursionDepth(16)]
void Branch(ThreadNodeInputRecord<BranchRec> input,
            [MaxRecords(3)] NodeOutput<BranchRec> Branch)
{
    BranchRec r = input.Get();
    float3 endp = r.pos + r.dir * r.len;

    uint idx;
    Counter.InterlockedAdd(0, 1, idx);
    if (idx < maxCaps) {
        float t = 1.0 - float(r.depth) / float(max(rootDepth,1u));   // 0=根 1=先端
        float3 color = lerp(rootCol, tipCol, t);
        Caps[idx].a   = float4(r.pos,  r.radius);
        Caps[idx].b   = float4(endp,   r.radius * radScale);
        Caps[idx].col = float4(color,  t);   // col.w = birth 割合 (0=根 1=先端) 成長アニメ用
    }

    if (r.depth > 0 && GetRemainingRecursionLevels() > 0) {
        uint n = min(branchN, 3u);
        ThreadNodeOutputRecords<BranchRec> outs = Branch.GetThreadNodeOutputRecords(n);
        float3 up   = abs(r.dir.y) < 0.99 ? float3(0,1,0) : float3(1,0,0);
        float3 side = normalize(cross(up, r.dir));
        float3 fwd  = cross(r.dir, side);
        for (uint i = 0; i < n; ++i) {
            float phi = (float(i) + rnd(r.seed*7u + i)) * (6.2831853/float(n))
                        + twist * float(rootDepth - r.depth);
            float ang = spreadAngle * (0.7 + 0.6*rnd(r.seed*13u + i));
            float3 cd = normalize(cos(ang)*r.dir + sin(ang)*(cos(phi)*side + sin(phi)*fwd));
            outs[i].pos    = endp;
            outs[i].dir    = cd;
            outs[i].len    = r.len * lenScale;
            outs[i].radius = r.radius * radScale;
            outs[i].depth  = r.depth - 1;
            outs[i].seed   = hashu(r.seed*1664525u + i*1013904223u + 1u);
        }
        outs.OutputComplete();
    }
}
