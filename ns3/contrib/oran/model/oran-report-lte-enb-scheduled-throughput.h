#pragma once
#include "oran-report.h"

namespace ns3 {

/**
 * Report: LTE eNB scheduled throughput (per cell, per window).
 * Units: dl/ul in Mbps; window_ms and ttis_observed are metadata.
 */
class OranReportLteEnbScheduledThroughput : public OranReport
{
public:
  static TypeId GetTypeId (void);
  OranReportLteEnbScheduledThroughput ();
  ~OranReportLteEnbScheduledThroughput () override = default;

  void SetCellId (uint16_t v)          { m_cellId = v; }
  void SetDlMbps (double v)            { m_dlMbps = v; }
  void SetUlMbps (double v)            { m_ulMbps = v; }
  void SetWindowMs (uint32_t v)        { m_windowMs = v; }
  void SetTtisObserved (uint32_t v)    { m_ttisObserved = v; }

  uint16_t GetCellId () const          { return m_cellId; }
  double   GetDlMbps () const          { return m_dlMbps; }
  double   GetUlMbps () const          { return m_ulMbps; }
  uint32_t GetWindowMs () const        { return m_windowMs; }
  uint32_t GetTtisObserved () const    { return m_ttisObserved; }

private:
  uint16_t m_cellId {0};
  double   m_dlMbps {0.0};
  double   m_ulMbps {0.0};
  uint32_t m_windowMs {0};
  uint32_t m_ttisObserved {0};
};

} // namespace ns3
