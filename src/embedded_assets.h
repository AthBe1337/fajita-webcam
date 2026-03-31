#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

struct EmbeddedAsset {
    std::string_view path;
    const std::uint8_t* data;
    std::size_t size;
};

const std::vector<EmbeddedAsset>& embedded_assets();
