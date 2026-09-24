#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace GGPKExp {

class IViewer {
public:
    virtual ~IViewer() = default;

    // Core interface -- all viewers must implement
    virtual void LoadData(std::vector<uint8_t> data, const std::string& path) = 0;
    virtual void Clear() = 0;
    virtual void Render() = 0;

    // State queries
    bool IsLoaded() const { return m_loaded; }
    bool HasError() const { return !m_error.empty(); }
    const std::string& GetError() const { return m_error; }

    // Optional: navigation support for viewers that can navigate to other files
    // (TextViewer hyperlinks, DatViewer foreign keys, MtpViewer path refs)
    // Default: no navigation. Override in viewers that support it.
    virtual bool HasPendingNavigation() const { return false; }
    virtual std::string TakePendingNavigation() { return {}; }
    virtual int TakePendingNavRow() { return -1; }

protected:
    bool m_loaded = false;
    std::string m_error;
};

} // namespace GGPKExp
