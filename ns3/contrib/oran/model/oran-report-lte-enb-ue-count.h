// oran-report-lte-enb-ue-count.h
#pragma once
#include "oran-report.h"

namespace ns3 {
class OranReportLteEnbUeCount : public OranReport
{
public:
  static TypeId GetTypeId();
  OranReportLteEnbUeCount() = default;

  void     SetCellId(uint16_t v)   { m_cellId = v; }
  void     SetUeCount(uint32_t v)  { m_ueCount = v; }

  uint16_t GetCellId()  const { return m_cellId; }
  uint32_t GetUeCount() const { return m_ueCount; }

private:
  uint16_t m_cellId = 0;
  uint32_t m_ueCount = 0;
};
}
