#pragma once

namespace live {
struct StreamPreset {
    const char *id, *name;
    int fps, jpegQuality, rate;
};
inline constexpr StreamPreset streamPresets[] = {
    {"hd30", "高清", 30, 80, 2000000},
    {"balanced30", "均衡", 30, 70, 2000000},
    {"low24", "低带宽", 24, 40, 1000000},
    {"saver30", "省流", 30, 20, 800000},
};
} // namespace live
