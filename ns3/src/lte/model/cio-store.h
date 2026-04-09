#pragma once
#include <cstdint>

namespace ns3 {

class CioStore
{
public:
  // Set the CIO (in dB, can be positive or negative) from src to dst cell
  static void Set(uint16_t srcCellId, uint16_t dstCellId, double offsetDb);

  // Get the CIO from src to dst cell; return 0 if not configured
  static double Get(uint16_t srcCellId, uint16_t dstCellId);

  // Clear all CIO entries associated with a given source cell
  // (recommended if only one neighbor should be biased at a time)
  static void ClearSrc(uint16_t srcCellId);

  static void ClearAll();
};

} // namespace ns3
