#pragma once
#include "oran-report.h"

namespace ns3 {

class OranReportLteEnbMcs : public OranReport
{
public:
  static TypeId GetTypeId();
  OranReportLteEnbMcs();

  void SetCellId(uint16_t v) { m_cellId = v; }
  void SetDlMean(double v) { m_dlMean = v; }  void SetDlP50(double v){ m_dlP50=v; }  void SetDlP95(double v){ m_dlP95=v; }
  void SetUlMean(double v) { m_ulMean = v; }  void SetUlP50(double v){ m_ulP50=v; }  void SetUlP95(double v){ m_ulP95=v; }

  // --- getters needed by ReceiveReport() ---
  uint16_t GetCellId() const { return m_cellId; }
  double GetDlMean() const { return m_dlMean; }
  double GetDlP50() const { return m_dlP50; }
  double GetDlP95() const { return m_dlP95; }
  double GetUlMean() const { return m_ulMean; }
  double GetUlP50() const { return m_ulP50; }
  double GetUlP95() const { return m_ulP95; }

  // serialize to storage (RIC will call)
  std::string ToJson() const; // optional helper; not overriding base

private:
  uint16_t m_cellId {0};
  double m_dlMean {0}, m_dlP50 {0}, m_dlP95 {0};
  double m_ulMean {0}, m_ulP50 {0}, m_ulP95 {0};
};

} // namespace ns3
