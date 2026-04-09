#pragma once
#include "oran-report.h"

namespace ns3 {

class OranReportLteEnbPrbUtilization : public OranReport
{
public:
  static TypeId GetTypeId (void);
  OranReportLteEnbPrbUtilization ();
  ~OranReportLteEnbPrbUtilization () override;

  void SetCellId (uint16_t cellId)      { m_cellId = cellId; }
  void SetDlUtilization (double u)      { m_dlUtil = u; }
  void SetUlUtilization (double u)      { m_ulUtil = u; }

  // NEW
  void SetWindowMs(uint32_t ms)         { m_windowMs = ms; }
  void SetTtisObserved(uint32_t ttis)   { m_ttisObserved = ttis; }

  uint16_t GetCellId ()           const { return m_cellId; }
  double   GetDlUtilization ()    const { return m_dlUtil; }
  double   GetUlUtilization ()    const { return m_ulUtil; }

  // NEW
  uint32_t GetWindowMs()          const { return m_windowMs; }
  uint32_t GetTtisObserved()      const { return m_ttisObserved; }

private:
  uint16_t m_cellId {0};
  double   m_dlUtil {0.0}; // 0..1
  double   m_ulUtil {0.0}; // 0..1

  // NEW
  uint32_t m_windowMs {0};
  uint32_t m_ttisObserved {0};
};

} // namespace ns3
