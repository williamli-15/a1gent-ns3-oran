// oran-report-lte-ue-sinr.h
#pragma once
#include "oran-report.h"
#include "ns3/type-id.h"
#include <cstdint>

namespace ns3 {

/**
 * One UE SINR/RSRP report (aggregated over the reporter window).
 */
class OranReportLteUeSinr : public OranReport
{
public:
  static TypeId GetTypeId (void);
  OranReportLteUeSinr ();
  ~OranReportLteUeSinr () override;

  void SetCellId (uint16_t cellId) { m_cellId = cellId; }
  void SetRnti   (uint16_t rnti)   { m_rnti   = rnti;   }

  void SetSinrLinear (double v) { m_sinrLin = v; }
  void SetSinrDb     (double v) { m_sinrDb  = v; }

  void SetRsrpMw     (double v) { m_rsrpMw  = v; }
  void SetRsrpDbm    (double v) { m_rsrpDbm = v; }

  uint16_t GetCellId ()   const { return m_cellId; }
  uint16_t GetRnti   ()   const { return m_rnti;   }
  double   GetSinrLinear () const { return m_sinrLin; }
  double   GetSinrDb     () const { return m_sinrDb;  }
  double   GetRsrpMw     () const { return m_rsrpMw;  }
  double   GetRsrpDbm    () const { return m_rsrpDbm; }

private:
  uint16_t m_cellId {0};
  uint16_t m_rnti   {0};
  double   m_sinrLin {0.0};
  double   m_sinrDb  {0.0};
  double   m_rsrpMw  {0.0};
  double   m_rsrpDbm {0.0};
};

} // namespace ns3
