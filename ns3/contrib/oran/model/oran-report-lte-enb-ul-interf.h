#pragma once

#include "oran-report.h"
#include <ns3/type-id.h>

namespace ns3 {

class OranReportLteEnbUlInterf : public OranReport
{
public:
  static TypeId GetTypeId();

  OranReportLteEnbUlInterf() = default;

  void   SetCellId(uint16_t v)   { m_cellId = v; }
  void   SetP95Mw(double v)      { m_p95Mw = v; }
  void   SetP95Dbm(double v)     { m_p95Dbm = v; }

  uint16_t GetCellId()  const { return m_cellId; }
  double   GetP95Mw()   const { return m_p95Mw; }
  double   GetP95Dbm()  const { return m_p95Dbm; }

private:
  uint16_t m_cellId = 0;
  double   m_p95Mw  = 0.0;
  double   m_p95Dbm = -300.0;
};

} // namespace ns3
