#include "cio-store.h"
#include <unordered_map>
#include <algorithm>

namespace ns3 {

static inline uint32_t Key(uint16_t src, uint16_t dst)
{
  return (uint32_t(src) << 16) | uint32_t(dst);
}

static std::unordered_map<uint32_t, double> g_cio;

void CioStore::Set(uint16_t srcCellId, uint16_t dstCellId, double offsetDb)
{
  // Clamp to an LTE-friendly CIO range
  offsetDb = std::max(-24.0, std::min(24.0, offsetDb));
  g_cio[Key(srcCellId, dstCellId)] = offsetDb;
}

double CioStore::Get(uint16_t srcCellId, uint16_t dstCellId)
{
  auto it = g_cio.find(Key(srcCellId, dstCellId));
  if (it != g_cio.end())
  {
    return it->second;
  }
  return 0.0;
}

void CioStore::ClearSrc(uint16_t srcCellId)
{
  // O(n) iteration is acceptable for small-scale simulations
  for (auto it = g_cio.begin(); it != g_cio.end(); )
  {
    uint16_t src = uint16_t(it->first >> 16);
    if (src == srcCellId)
    {
      it = g_cio.erase(it);
    }
    else
    {
      ++it;
    }
  }
}

void CioStore::ClearAll()
{
  g_cio.clear();
}

} // namespace ns3
