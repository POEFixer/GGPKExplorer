#pragma once
#include <string>
#include <vector>

namespace GGPKExp::Dev {

struct RoundTripResult {
    std::string fileName;
    bool parsed = false;
    bool serialized = false;
    bool reparsed = false;
    bool rowCountMatches = false;
    bool allCellsMatch = false;
    size_t firstDiffRow = 0;
    size_t firstDiffCol = 0;
    std::string detail;
};

// Runs Parse -> Serialize -> Parse on a single .datc64 file.
// schemaJsonPath must be the full path to schema.min.json.
RoundTripResult RunRoundTrip(const std::string& datPath, const std::string& schemaJsonPath);

// Runs RunRoundTrip on 6 representative files from D:\POE2Files\data\balance\.
// Returns all results in order; caller is expected to print summary.
std::vector<RoundTripResult> RunAllRoundTrips(const std::string& schemaJsonPath);

} // namespace GGPKExp::Dev
