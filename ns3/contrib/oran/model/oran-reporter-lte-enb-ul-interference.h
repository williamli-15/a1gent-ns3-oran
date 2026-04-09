#pragma once

#include "oran-reporter.h"
#include <ns3/spectrum-value.h>
#include <map>
#include <vector>

namespace ns3 {

class OranNearRtRic;  // <-- add this forward declaration


/**
 * R5: UL wideband interference p95 per cell, 1 s window.
 * Hooks: /LteEnbPhy/ReportInterference.
 */
class OranReporterLteEnbUlInterference : public OranReporter
{
public:
  static TypeId GetTypeId();

  OranReporterLteEnbUlInterference();
  ~OranReporterLteEnbUlInterference() override = default;

  void Activate() override;
  void Deactivate() override;
  void DoDispose() override;

  std::vector< Ptr<OranReport> > GenerateReports() override;

private:
  // Trace connection
  void EnsureConnected();
  void Disconnect();

  static void InterfCb(OranReporterLteEnbUlInterference* self,
                       std::string path,
                       uint16_t cellId,
                       Ptr<SpectrumValue> interference);

  static inline double LinearToDbm(double mW)
  {
    if (mW <= 0) return -300.0;
    return 10.0 * std::log10(mW);
  }

  bool m_connected = false;

  // Per-cell 1s window samples (mW)
  std::map<uint16_t, std::vector<double>> m_samplesMw;

  // scoping to the eNB primary cell
  Ptr<OranNearRtRic> m_nearRtRic;
  uint16_t m_myCellId{0};
};

} // namespace ns3
