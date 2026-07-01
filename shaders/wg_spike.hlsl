// 最小 work graph: 1 つの broadcasting ノードが UAV に書く (feasibility 確認用)。
RWStructuredBuffer<uint> Out : register(u0);

struct Rec { uint seed; };

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1,1,1)]
[NodeIsProgramEntry]
[numthreads(64,1,1)]
void MainNode(uint gtid : SV_GroupThreadID, DispatchNodeInputRecord<Rec> ir)
{
    Out[gtid] = gtid + ir.Get().seed;
}
