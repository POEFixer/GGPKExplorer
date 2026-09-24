#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace GGPKExp {

class BinaryReader {
public:
    BinaryReader(const uint8_t* data, size_t size);

    template<typename T>
    T Read() {
        T val{};
        if (m_pos + sizeof(T) > m_size) {
            m_valid = false;
            return val;
        }
        std::memcpy(&val, m_data + m_pos, sizeof(T));
        m_pos += sizeof(T);
        return val;
    }

    std::string ReadString(size_t len);
    std::u16string ReadWString(size_t len);
    std::vector<uint8_t> ReadBytes(size_t count);
    void Skip(size_t bytes);
    void Seek(size_t pos);

    const uint8_t* CurrentPtr() const;
    size_t Position() const;
    size_t Remaining() const;
    size_t Size() const;
    bool IsValid() const;

private:
    const uint8_t* m_data;
    size_t m_size;
    size_t m_pos = 0;
    bool m_valid = true;
};

} // namespace GGPKExp
