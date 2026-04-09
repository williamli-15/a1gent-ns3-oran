// oran-report-lte-ue-prb.h
#pragma once
#include "oran-report.h"
#include "ns3/uinteger.h"

namespace ns3 {

class OranReportLteUePrb : public OranReport
{
public:
  static TypeId GetTypeId();

  OranReportLteUePrb() = default;

  void SetUeId(uint64_t u)   { m_ueid = u; }
  void SetCellId(uint16_t c) { m_cellId = c; }
  void SetDlPrbs(uint64_t v) { m_dlPrbs = v; }
  void SetUlPrbs(uint64_t v) { m_ulPrbs = v; }

  uint64_t GetUeId()   const { return m_ueid; }
  uint16_t GetCellId() const { return m_cellId; }
  uint64_t GetDlPrbs() const { return m_dlPrbs; }
  uint64_t GetUlPrbs() const { return m_ulPrbs; }

private:
  uint64_t m_ueid   = 0;
  uint16_t m_cellId = 0;
  uint64_t m_dlPrbs = 0;
  uint64_t m_ulPrbs = 0;
};

} // ns3
