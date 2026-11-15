#pragma once
#include <string>
#include <cstdint>
#include <cstring>
using namespace std;


struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits

    FuncEntry () {
        funcName = "NULL";
        byteOffsetInResolveBin = 0x0;
    }

    FuncEntry (string _funcName, int64_t __byte) {
        funcName = _funcName;
        byteOffsetInResolveBin = __byte;
    }
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};

inline uint32_t addToChecksum(uint32_t sum, const char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        sum = sum * 31 + (unsigned char)buf[i];
    }
    return sum;
}

struct TrailerChecker
{
    string held;
    uint32_t sum;

    TrailerChecker() {
        sum = 0;
    }

    string feed(const char *buf, size_t n)
    {
        held.append(buf, n);
        if (held.size() <= 4) {
            return "";
        }
        size_t cut = held.size() - 4;
        string out = held.substr(0, cut);
        held = held.substr(cut);
        sum = addToChecksum(sum, out.c_str(), out.size());
        return out;
    }

    bool finish()
    {
        if (held.size() != 4) {
            return false;
        }
        uint32_t got;
        memcpy(&got, held.c_str(), 4);
        return got == sum;
    }
};