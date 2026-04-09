#pragma once
#include "oran-reporter.h"

#include <ns3/ptr.h>
#include <ns3/nstime.h>
#include <ns3/lte-enb-net-device.h>
#include <ns3/lte-enb-mac.h>

#include <cstdint>

namespace ns3 {

/**
 * Reporter: aggregates eNB MAC scheduled TB bytes per TTI into DL/UL Mbps.
 * Window = trigger interval (e.g., 1.0 s). Emits OranReportLteEnbScheduledThroughput.
 */
class OranReporterLteEnbScheduledThroughput : public OranReporter
{
public:
  static TypeId GetTypeId (void);
  OranReporterLteEnbScheduledThroughput ();
  ~OranReporterLteEnbScheduledThroughput () override;

  void Activate () override;
  void Deactivate () override;

protected:
  void DoDispose () override;
  std::vector<Ptr<OranReport>> GenerateReports () override;

private:
  void ConnectTraces ();
  void DisconnectTraces ();

  // MAC trace sinks
  void NotifyDlScheduling (DlSchedulingCallbackInfo info);
  void NotifyUlScheduling (uint32_t frameNo,
                           uint32_t subframeNo,
                           uint16_t rnti,
                           uint8_t mcs,
                           uint16_t sizeBytes,
                           uint8_t componentCarrierId);

  // state
  Ptr<LteEnbNetDevice> m_enbDev {nullptr};
  Ptr<LteEnbMac>       m_enbMac {nullptr};
  uint16_t             m_cellId {0};
  bool                 m_tracesConnected {false};

  // window accounting
  Time     m_windowStart;
  uint64_t m_dlBytes {0};
  uint64_t m_ulBytes {0};
};

} // namespace ns3
