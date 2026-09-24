#pragma once
#include <string>
#include <utility>

namespace GGPKExp {

template<typename T>
struct Result {
    bool success = false;
    std::string error;
    T value{};

    static Result Ok(T val = {}) { return {true, {}, std::move(val)}; }
    static Result Fail(std::string err) { return {false, std::move(err), {}}; }
    explicit operator bool() const { return success; }
};

template<>
struct Result<void> {
    bool success = false;
    std::string error;

    static Result Ok() { return {true, {}}; }
    static Result Fail(std::string err) { return {false, std::move(err)}; }
    explicit operator bool() const { return success; }
};

} // namespace GGPKExp
