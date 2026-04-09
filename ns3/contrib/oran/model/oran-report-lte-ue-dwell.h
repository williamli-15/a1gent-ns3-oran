// oran-report-lte-ue-dwell.h
#pragma once
#include "oran-report.h"
namespace ns3 {
class OranReportLteUeDwell : public OranReport
{
public:
  static TypeId GetTypeId();
  OranReportLteUeDwell() = default;

  void SetUeId(uint64_t v)     { m_ueid = v; }
  void SetCellId(uint16_t v)   { m_cellId = v; }
  void SetDwellSeconds(double v){ m_dwell = v; }

  uint64_t GetUeId()    const { return m_ueid; }
  uint16_t GetCellId()  const { return m_cellId; }
  double   GetDwellSeconds() const { return m_dwell; }

private:
  uint64_t m_ueid = 0;
  uint16_t m_cellId = 0;
  double   m_dwell = 0.0;
};
} // ns3
