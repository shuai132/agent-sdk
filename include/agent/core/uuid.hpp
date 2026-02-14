#pragma once

#include <string>
#include <random>
#include <sstream>
#include <iomanip>

namespace agent {

// Simple UUID v4 generator
class UUID {
public:
    static std::string generate() {
        static std::random_device rd;
        static std::mt19937_64 gen(rd());
        static std::uniform_int_distribution<uint64_t> dist;

        uint64_t ab = dist(gen);
        uint64_t cd = dist(gen);

        // Set version to 4 (random)
        ab = (ab & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
        // Set variant to RFC 4122
        cd = (cd & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        ss << std::setw(8) << (ab >> 32) << "-";
        ss << std::setw(4) << ((ab >> 16) & 0xFFFF) << "-";
        ss << std::setw(4) << (ab & 0xFFFF) << "-";
        ss << std::setw(4) << (cd >> 48) << "-";
        ss << std::setw(12) << (cd & 0x0000FFFFFFFFFFFFULL);
        return ss.str();
    }

    static std::string short_id(size_t length = 8) {
        static const char charset[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<size_t> dist(0, sizeof(charset) - 2);

        std::string result;
        result.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            result += charset[dist(gen)];
        }
        return result;
    }
};

}  // namespace agent
