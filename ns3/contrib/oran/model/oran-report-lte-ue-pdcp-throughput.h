#pragma once
#include "oran-report.h"
#include <ns3/uinteger.h>

namespace ns3 {

/**
 * Report: UE PDCP delivered throughput (per UE)
 * - dl/ul Mbps over the reporter's current window
 */
class OranReportLteUePdcpThroughput : public OranReport
{
public:
  static TypeId GetTypeId (void);
  OranReportLteUePdcpThroughput ();
  ~OranReportLteUePdcpThroughput () override = default;

  void SetUeId (uint64_t id)     { m_ueId = id; }
  void SetCellId (uint16_t cid)  { m_cellId = cid; }
  void SetDlMbps (double v)      { m_dlMbps = v; }
  void SetUlMbps (double v)      { m_ulMbps = v; }

  uint64_t GetUeId () const      { return m_ueId; }
  uint16_t GetCellId () const    { return m_cellId; }
  double GetDlMbps () const      { return m_dlMbps; }
  double GetUlMbps () const      { return m_ulMbps; }


private:
  uint64_t m_ueId {0};
  uint16_t m_cellId {0};
  double   m_dlMbps {0.0};
  double   m_ulMbps {0.0};
};

} // namespace ns3
