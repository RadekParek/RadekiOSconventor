#pragma once
#include "json.hpp"
namespace radek {
/**
 * Parse a Mach-O image. The default produces the full symbol/reconstruction
 * inventory used by the host tools. Device import analysis can disable large
 * local-symbol/export tables while retaining imports and loader facts.
 */
Json analyze(const std::vector<uint8_t> &data, bool includeSymbolDetails = true);
}
