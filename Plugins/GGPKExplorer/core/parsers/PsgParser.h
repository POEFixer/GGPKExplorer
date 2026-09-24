#pragma once

#include "../Result.h"
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>

namespace GGPKExp { namespace Parsers {

struct PsgEdge {
    uint32_t targetHash = 0;
    uint32_t extra = 0;
};

struct PsgNode {
    uint32_t hash = 0;
    uint32_t orbit = 0;
    uint32_t orbitIndex = 0;
    std::vector<PsgEdge> edges;
    float worldX = 0.0f;
    float worldY = 0.0f;
};

struct PsgGroup {
    float centreX = 0.0f;
    float centreY = 0.0f;
    uint32_t assocKey = 0;
    uint32_t bgOverride = 0;
    bool isJewel = false;
    std::vector<PsgNode> nodes;
};

struct PsgData {
    uint8_t version = 0;
    uint8_t type = 0;           // 0=Character, 1=Atlas
    std::vector<uint8_t> orbits;
    std::vector<uint32_t> rootNodes;
    std::vector<PsgGroup> groups;
    std::unordered_map<uint32_t, std::pair<size_t, size_t>> nodeIndex;
    uint32_t totalNodes = 0;
    uint32_t totalEdges = 0;

    const PsgNode* FindNode(uint32_t hash) const {
        auto it = nodeIndex.find(hash);
        if (it == nodeIndex.end()) return nullptr;
        return &groups[it->second.first].nodes[it->second.second];
    }
};

inline constexpr float ORBIT_RADII[] = {0, 82, 162, 335, 493, 662, 846, 251, 1020, 1200};
inline constexpr size_t ORBIT_RADII_COUNT = 10;

Result<PsgData> ParsePsg(const uint8_t* data, size_t size);

}} // namespace GGPKExp::Parsers
