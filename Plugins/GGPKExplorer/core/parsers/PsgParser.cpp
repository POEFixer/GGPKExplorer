#include "PsgParser.h"
#include "../BinaryReader.h"

namespace GGPKExp { namespace Parsers {

static void ComputeNodePositions(PsgData& data) {
    constexpr float PI2 = 6.283185307f;
    data.totalNodes = 0;
    data.totalEdges = 0;

    for (size_t gi = 0; gi < data.groups.size(); ++gi) {
        auto& group = data.groups[gi];
        for (size_t ni = 0; ni < group.nodes.size(); ++ni) {
            auto& node = group.nodes[ni];
            data.nodeIndex[node.hash] = {gi, ni};

            if (node.orbit == 0 || node.orbit >= data.orbits.size()) {
                node.worldX = group.centreX;
                node.worldY = group.centreY;
            } else {
                float radius = (node.orbit < ORBIT_RADII_COUNT) ? ORBIT_RADII[node.orbit] : 0.0f;
                uint8_t slots = data.orbits[node.orbit];
                float angle = (slots > 0) ? PI2 * node.orbitIndex / slots : 0.0f;
                node.worldX = group.centreX + radius * std::sin(angle);
                node.worldY = group.centreY - radius * std::cos(angle);
            }

            data.totalNodes++;
            data.totalEdges += static_cast<uint32_t>(node.edges.size());
        }
    }
}

Result<PsgData> ParsePsg(const uint8_t* data, size_t size) {
    if (!data || size < 4)
        return Result<PsgData>::Fail("Data too small for PSG header");

    BinaryReader reader(data, size);
    PsgData result;

    result.version = reader.Read<uint8_t>();
    result.type = reader.Read<uint8_t>();
    uint8_t orbitCount = reader.Read<uint8_t>();
    if (!reader.IsValid())
        return Result<PsgData>::Fail("Failed to read PSG header");

    result.orbits.resize(orbitCount);
    for (uint8_t i = 0; i < orbitCount; ++i)
        result.orbits[i] = reader.Read<uint8_t>();

    uint32_t rootCount = reader.Read<uint32_t>();
    if (!reader.IsValid() || rootCount > 100)
        return Result<PsgData>::Fail("Invalid root edge count: " + std::to_string(rootCount));

    result.rootNodes.reserve(rootCount);
    for (uint32_t i = 0; i < rootCount; ++i) {
        uint32_t hash = reader.Read<uint32_t>();
        reader.Read<uint32_t>(); // reserved
        result.rootNodes.push_back(hash);
    }

    uint32_t groupCount = reader.Read<uint32_t>();
    if (!reader.IsValid() || groupCount > 50000)
        return Result<PsgData>::Fail("Invalid group count: " + std::to_string(groupCount));

    result.groups.reserve(groupCount);
    for (uint32_t g = 0; g < groupCount; ++g) {
        PsgGroup group;
        group.centreX = reader.Read<float>();
        group.centreY = reader.Read<float>();
        group.assocKey = reader.Read<uint32_t>();
        group.bgOverride = reader.Read<uint32_t>();
        group.isJewel = reader.Read<uint8_t>() != 0;

        uint32_t nodeCount = reader.Read<uint32_t>();
        if (!reader.IsValid() || nodeCount > 1000)
            return Result<PsgData>::Fail("Invalid node count in group " + std::to_string(g));

        group.nodes.reserve(nodeCount);
        for (uint32_t n = 0; n < nodeCount; ++n) {
            PsgNode node;
            node.hash = reader.Read<uint32_t>();
            node.orbit = reader.Read<uint32_t>();
            node.orbitIndex = reader.Read<uint32_t>();

            uint32_t edgeCount = reader.Read<uint32_t>();
            if (!reader.IsValid() || edgeCount > 500)
                return Result<PsgData>::Fail("Invalid edge count in group " + std::to_string(g) + " node " + std::to_string(n));

            node.edges.resize(edgeCount);
            for (uint32_t e = 0; e < edgeCount; ++e) {
                node.edges[e].targetHash = reader.Read<uint32_t>();
                node.edges[e].extra = reader.Read<uint32_t>();
            }
            group.nodes.push_back(std::move(node));
        }
        result.groups.push_back(std::move(group));
    }

    if (!reader.IsValid())
        return Result<PsgData>::Fail("Data truncated during parsing");

    ComputeNodePositions(result);
    return Result<PsgData>::Ok(std::move(result));
}

}} // namespace GGPKExp::Parsers
