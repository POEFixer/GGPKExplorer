#include "BinaryReader.h"
#include <algorithm>

namespace GGPKExp {

BinaryReader::BinaryReader(const uint8_t* data, size_t size)
    : m_data(data), m_size(size) {}

std::string BinaryReader::ReadString(size_t len) {
    if (m_pos + len > m_size) {
        m_valid = false;
        return {};
    }
    std::string result(reinterpret_cast<const char*>(m_data + m_pos), len);
    m_pos += len;
    return result;
}

std::u16string BinaryReader::ReadWString(size_t len) {
    size_t byteLen = len * sizeof(char16_t);
    if (m_pos + byteLen > m_size) {
        m_valid = false;
        return {};
    }
    std::u16string result(len, u'\0');
    std::memcpy(result.data(), m_data + m_pos, byteLen);
    m_pos += byteLen;
    return result;
}

std::vector<uint8_t> BinaryReader::ReadBytes(size_t count) {
    if (m_pos + count > m_size) {
        m_valid = false;
        return {};
    }
    std::vector<uint8_t> result(m_data + m_pos, m_data + m_pos + count);
    m_pos += count;
    return result;
}

void BinaryReader::Skip(size_t bytes) {
    if (m_pos + bytes > m_size) {
        m_valid = false;
        m_pos = m_size;
        return;
    }
    m_pos += bytes;
}

void BinaryReader::Seek(size_t pos) {
    if (pos > m_size) {
        m_valid = false;
        return;
    }
    m_pos = pos;
}

const uint8_t* BinaryReader::CurrentPtr() const {
    return m_data + m_pos;
}

size_t BinaryReader::Position() const { return m_pos; }
size_t BinaryReader::Remaining() const { return m_pos < m_size ? m_size - m_pos : 0; }
size_t BinaryReader::Size() const { return m_size; }
bool BinaryReader::IsValid() const { return m_valid; }

} // namespace GGPKExp
