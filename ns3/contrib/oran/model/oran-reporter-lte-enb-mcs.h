#pragma once
#include "oran-reporter.h"
#include "oran-near-rt-ric.h"
#include "oran-report.h" // base
#include <ns3/lte-enb-mac.h>
#include <ns3/config.h>
#include <ns3/simulator.h>
#include <ns3/log.h>
#include <array>
#include <unordered_map>

namespace ns3 {

class OranReportLteEnbMcs; // fwd

class OranReporterLteEnbMcs : public OranReporter
{
public:
  static TypeId GetTypeId();
  OranReporterLteEnbMcs();
  ~OranReporterLteEnbMcs() override;

  // attrs
  void SetNearRtRic(Ptr<OranNearRtRic> ric) { m_nearRtRic = ric; }

  // OranReporter
  void Activate() override;
  void Deactivate() override;
  void DoDispose() override;
  std::vector<Ptr<OranReport>> GenerateReports() override;

private:
  // trace wiring
  void EnsureConnected();
  void Disconnect();

  // traces
  static void OnNewUeContext(OranReporterLteEnbMcs* self,
                             std::string ctx,
                             uint16_t cellId,
                             uint16_t rnti);

  static void OnDlScheduling(OranReporterLteEnbMcs* self,
                             std::string path,
                             DlSchedulingCallbackInfo info);

  static void OnUlScheduling(OranReporterLteEnbMcs* self,
                             std::string path,
                             uint32_t frameNo,
                             uint32_t subframeNo,
                             uint16_t rnti,
                             uint8_t  mcs,
                             uint16_t tbs,
                             uint8_t  ccId);

  // helpers
  struct Hist {
    std::array<uint64_t, 33> bins{}; // defensive up to 32
    uint64_t n = 0;
    uint64_t sum = 0;
  };
  static void Add(Hist& h, uint8_t mcs);
  static double Pxx(const Hist& h, double q);   // 0.5, 0.95
  static double Mean(const Hist& h);

  // scope filter & maps
  bool      m_tracesConnected {false};
  Time      m_windowStart;
  Ptr<OranNearRtRic> m_nearRtRic;  // for cellId lookup
  uint16_t  m_thisCellId {0};      // primary cell of this eNB (repo)

  // only rnti(s) belonging to this eNB primary cell are recorded
  std::unordered_map<uint16_t/*rnti*/, uint16_t/*cellId*/> m_rntiToCell;

  Hist m_dl; // keyed only by this reporter’s cell (aggregated)
  Hist m_ul;
};

} // namespace ns3
