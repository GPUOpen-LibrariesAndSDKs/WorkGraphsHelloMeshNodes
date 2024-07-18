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

// This file contains the entire work graph HLSL source code as a single resource string
// The shader code will be compiled twice:
//  - with target lib_6_9 for all work graph nodes, including the two mesh nodes for drawing
//  - with target ps_6_9 for the pixel shader. Pixel shader cannot be included in the library object and need to be compiled separately.

namespace shader {
    static const char* workGraphSource = R"(
// =========================
// Work graph record structs

// Record used for recursively generating & drawing lines
struct LineRecord
{
    float2 start;
    float2 end;
};

// Record used to draw a single triangle
struct TriangleDrawRecord
{
    float2 verts[3];
    uint   depth;
};

// Number of Koch iterations
static const uint maxSnowflakeRecursions = 3;

// This node creates the triangle base for the Koch snowflake.
[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("thread")]
void EntryNode(
    // Start recursive Koch fractal on each of the three sides of the triangle
    [MaxRecords(3)]NodeOutput<LineRecord> SnowflakeNode,
    // Fill triangle
    [MaxRecords(1)]NodeOutput<TriangleDrawRecord> TriangleMeshNode)
{
    ThreadNodeOutputRecords<LineRecord> snowflakeRecords    = SnowflakeNode.GetThreadNodeOutputRecords(3);
    ThreadNodeOutputRecords<TriangleDrawRecord> drawRecords = TriangleMeshNode.GetThreadNodeOutputRecords(1);

    const float2 v0 = float2(0., .9);
    const float2 v1 = float2(+sqrt(3) * .45, -.45);
    const float2 v2 = float2(-sqrt(3) * .45, -.45);

    // Line v0 -> v1
    snowflakeRecords.Get(0).start = v0;
    snowflakeRecords.Get(0).end   = v1;

    // Line v1 -> v2
    snowflakeRecords.Get(1).start = v1;
    snowflakeRecords.Get(1).end   = v2;

    // Line v2 -> v0
    snowflakeRecords.Get(2).start = v2;
    snowflakeRecords.Get(2).end   = v0;

    // Triangle record
    drawRecords.Get(0).depth    = 0;
    drawRecords.Get(0).verts[0] = v0;
    drawRecords.Get(0).verts[1] = v1;
    drawRecords.Get(0).verts[2] = v2;

    snowflakeRecords.OutputComplete();
    drawRecords.OutputComplete();
};

[Shader("node")]
[NodeLaunch("thread")]
[NodeMaxRecursionDepth(maxSnowflakeRecursions)]
void SnowflakeNode(
    ThreadNodeInputRecord<LineRecord> record,
    // Koch fractal recursively splits line into 4 new line segments
    [MaxRecords(4)]NodeOutput<LineRecord> SnowflakeNode,
    // Two of the recursive lines form edges of a triangles, which needs to be filled
    [MaxRecords(1)]NodeOutput<TriangleDrawRecord> TriangleMeshNode,
    // If recursion is not possible, draw a single line
    [MaxRecords(1)]NodeOutput<LineRecord> LineMeshNode
) {
    const float2 start = record.Get().start;
    const float2 end   = record.Get().end;

    const bool hasOutput = GetRemainingRecursionLevels() != 0;

    ThreadNodeOutputRecords<LineRecord> snowflakeRecords  = SnowflakeNode.GetThreadNodeOutputRecords(hasOutput * 4);
    ThreadNodeOutputRecords<TriangleDrawRecord> triRecord = TriangleMeshNode.GetThreadNodeOutputRecords(hasOutput);
    ThreadNodeOutputRecords<LineRecord> lineRecord        = LineMeshNode.GetThreadNodeOutputRecords(!hasOutput);
    
    if (hasOutput) {
        const float2 perpendicular = float2(start.y - end.y, end.x - start.x) * sqrt(3) / 6;

        const float2 triangleLeft  = lerp(start, end, 1./3.);
        const float2 triangleMid   = lerp(start, end, .5) + perpendicular;
        const float2 triangleRight = lerp(start, end, 2./3.);

        snowflakeRecords.Get(0).start = start;
        snowflakeRecords.Get(0).end   = triangleLeft;
        snowflakeRecords.Get(1).start = triangleLeft;
        snowflakeRecords.Get(1).end   = triangleMid;
        snowflakeRecords.Get(2).start = triangleMid;
        snowflakeRecords.Get(2).end   = triangleRight;
        snowflakeRecords.Get(3).start = triangleRight;
        snowflakeRecords.Get(3).end   = end;
        
        triRecord.Get(0).depth        = 1 + (maxSnowflakeRecursions - GetRemainingRecursionLevels());
        triRecord.Get(0).verts[0]     = triangleLeft;
        triRecord.Get(0).verts[1]     = triangleMid;
        triRecord.Get(0).verts[2]     = triangleRight;
    } else {
        lineRecord.Get(0).start        = start;
        lineRecord.Get(0).end          = end;
    }

    snowflakeRecords.OutputComplete();
    lineRecord.OutputComplete();
    triRecord.OutputComplete();
}

// =======================================================
// Vertex and primitive attribute structs for mesh shaders
struct Vertex
{
    float4 position : SV_POSITION;
};

struct Primitive {
    float4 color : COLOR0;
};

// ==========
// Mesh Nodes

// Mesh shader to draw a line between a start and end position.
// As lines X degree angles, we cannot draw lines a simple 2D boxes.
// 
//
//     v2----------v3
//    /              \
//  v1                v4
//    \              /
//     v0-----------v5
//
// Triangulation:
//  - v0 -> v1 -> v2
//  - v0 -> v2 -> v3
//  - v0 -> v3 -> v4
//  - v0 -> v4 -> v5
[Shader("node")]
// Indicate that we are defining a mesh node
[NodeLaunch("mesh")]
// Mesh nodes do not automatically use the function name of the node as their node id.
// If we want to automatically add the generic program created with this mesh node to the work graph,
// we need to explicitly define a node id for it.
[NodeId("LineMeshNode", 0)]
// Mesh nodes can use [NodeDispatchGrid(...)] and [NodeMaxDispatchGrid(...)] in combination with SV_DispatchGrid.
[NodeDispatchGrid(1, 1, 1)]
// The rest of the attributes are the same as for "normal" mesh shaders.
[NumThreads(32, 1, 1)]
[OutputTopology("triangle")]
void LineMeshShader(
    uint gtid : SV_GroupThreadID,
    DispatchNodeInputRecord<LineRecord> inputRecord,
    out indices uint3 triangles[4],
    out primitives Primitive prims[4],
    out vertices Vertex verts[6])
{
    const LineRecord record = inputRecord.Get();
    SetMeshOutputCounts(6, 4);
    
    // Output triangles based on triangulation above
    if (gtid < 4)
    {
        triangles[gtid]   = uint3(0, gtid + 1, gtid + 2);
        prims[gtid].color = float4(0.03, 0.19, 0.42, 1.0);
    }

    // Output vertices
    if (gtid < 6) {
        const float2 direction     = normalize(record.end - record.start);
        const float2 perpendicular = float2(direction.y, -direction.x);

        const float lineWidth = 0.0075;

        // Offsets for outer triangle shape
        //
        //     offsets[2] ---- ...
        //    /
        //  offsets[1]
        //    \
        //     offsets[0] ---- ...
        // 
        // direction <---+
        //               |
        //               v
        //          prependicular
        //
        const float2 offsets[3] = {
            perpendicular,
            direction * sqrt(3) / 3.0,
            -perpendicular,
        };

        // Shift entire line end outwards by sqrt(3) / 3.0 to align with connecting line
        const float2 offset   = (direction * sqrt(3) / 3.0) + offsets[gtid % 3];
        const float2 position = (gtid < 3)? record.start - offset * lineWidth
                                          : record.end   + offset * lineWidth;

        verts[gtid].position = float4(position, 0.25, 1.0);
    }
}

// Color palette for different depth levels
float4 GetTriangleColor(in uint depth) {
    switch (depth % 4) {
        case 0: return float4(0.13, 0.44, 0.71, 1.0); // Triangle Recursion 0
        case 1: return float4(0.42, 0.68, 0.84, 1.0); // Triangle Recursion 1 
        case 2: return float4(0.74, 0.84, 0.91, 1.0); // Triangle Recursion 2 
        case 3: return float4(0.94, 0.95, 1.00, 1.0); // Triangle Recursion 3
        default: return 0;
    }
}

[Shader("node")]
[NodeLaunch("mesh")]
// To demonstrate how to override a mesh node id when creating the work graph, we don't specify a node id for this mesh shader.
// The node id will be set using a mesh node launch override when creating the work graph state object (see HelloMeshNodes.cpp:160)
// [NodeId("TriangleMeshNode", 0)]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(3, 1, 1)]
[OutputTopology("triangle")]
void TriangleMeshShader(
    uint gtid : SV_GroupThreadID,
    DispatchNodeInputRecord<TriangleDrawRecord> inputRecord,
    out indices uint3 triangles[1],
    out primitives Primitive prims[1],
    out vertices Vertex verts[3])
{
    const TriangleDrawRecord record = inputRecord.Get();

    SetMeshOutputCounts(3, 1);
    
    if (gtid < 1)
    {
        triangles[0]   = uint3(0, 1, 2);
        prims[0].color = GetTriangleColor(record.depth);
    }
  
    if (gtid < 3)
    {
        verts[gtid].position = float4(record.verts[gtid], 0.5, 1);
    }
}

// ================================
// Pixel Shader for both mesh nodes

float4 MeshNodePixelShader(in float4 color : COLOR0) : SV_TARGET
{
    return color;
}
    )";
}