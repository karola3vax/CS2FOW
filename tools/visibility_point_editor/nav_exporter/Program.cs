using System.Security.Cryptography;
using System.Text.Json;
using System.Numerics;
using ValveResourceFormat.NavMesh;

if (args.Length != 3)
{
    Console.Error.WriteLine("usage: NavExporter <map.nav> <map-name> <output.json>");
    return 2;
}

static float[] Vector(Vector3 value) => [value.X, value.Y, value.Z];
static uint? AreaId(NavMeshArea? area) => area?.AreaId;

var input = Path.GetFullPath(args[0]);
var mapName = args[1];
var output = Path.GetFullPath(args[2]);
var nav = new NavMeshFile();
nav.Read(input);

var areas = nav.Areas.Values
    .OrderBy(area => area.AreaId)
    .Select(area => new
    {
        id = area.AreaId,
        hull = area.HullIndex,
        flags = $"0x{unchecked((ulong)(long)area.DynamicAttributeFlags):x16}",
        corners = area.Corners.Select(Vector).ToArray(),
        center = new[]
        {
            area.Corners.Average(point => point.X),
            area.Corners.Average(point => point.Y),
            area.Corners.Average(point => point.Z),
        },
        connections = area.Connections.SelectMany((edgeConnections, sourceEdge) =>
            edgeConnections.Select(connection => new
            {
                area = connection.AreaId,
                source_edge = sourceEdge,
                target_edge = connection.EdgeId,
            })).ToArray(),
        ladders_above = area.LaddersAbove,
        ladders_below = area.LaddersBelow,
    }).ToArray();

var ladders = nav.Ladders.OrderBy(ladder => ladder.Id).Select(ladder => new
{
    id = ladder.Id,
    ladder.Width,
    ladder.Length,
    top = Vector(ladder.Top),
    bottom = Vector(ladder.Bottom),
    direction = (uint)ladder.Direction,
    areas = new
    {
        top_forward = AreaId(ladder.TopForwardArea),
        top_left = AreaId(ladder.TopLeftArea),
        top_right = AreaId(ladder.TopRightArea),
        top_behind = AreaId(ladder.TopBehindArea),
        bottom = AreaId(ladder.BottomArea),
        bottom_left = AreaId(ladder.BottomLeftArea),
        bottom_right = AreaId(ladder.BottomRightArea),
    },
}).ToArray();

var hulls = nav.GenerationParams?.HullParams.Select((hull, index) => new
{
    index,
    hull.Enabled,
    hull.Radius,
    hull.Height,
    hull.ShortHeightEnabled,
    hull.ShortHeight,
    hull.MaxClimb,
    hull.MaxSlope,
    hull.MaxJumpDownDist,
    hull.MaxJumpHorizDistBase,
    hull.MaxJumpUpDist,
}).ToArray() ?? [];

var document = new
{
    version = 1,
    map = mapName,
    source_sha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(input))).ToLowerInvariant(),
    nav_version = nav.Version,
    nav_subversion = nav.SubVersion,
    analyzed = nav.IsAnalyzed,
    hulls,
    areas,
    ladders,
};

Directory.CreateDirectory(Path.GetDirectoryName(output)!);
await File.WriteAllTextAsync(output, JsonSerializer.Serialize(document, new JsonSerializerOptions { WriteIndented = true }) + "\n");
Console.WriteLine(JsonSerializer.Serialize(new { map = mapName, areas = areas.Length, ladders = ladders.Length, output }));
return 0;
