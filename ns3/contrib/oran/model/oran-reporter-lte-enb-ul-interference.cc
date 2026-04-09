#include "oran-reporter-lte-enb-ul-interference.h"
#include "oran-report-lte-enb-ul-interf.h"
#include "oran-e2-node-terminator.h"
#include "oran-near-rt-ric.h"       // <-- add
#include "oran-data-repository.h"   // <-- add

#include <ns3/pointer.h>            // <-- add (PointerValue, MakePointerAccessor/Checker)
#include <ns3/uinteger.h>  // UintegerValue
#include <ns3/nstime.h>    // TimeValue
#include <ns3/config.h>
#include <ns3/simulator.h>
#include <ns3/log.h>

#include <tuple>           // <-- add this (for std::tie in Activate)
#include <algorithm>
#include <cmath>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("OranReporterLteEnbUlInterference");
NS_OBJECT_ENSURE_REGISTERED(OranReporterLteEnbUlInterference);

TypeId
OranReporterLteEnbUlInterference::GetTypeId()
{
  static TypeId tid =
    TypeId("ns3::OranReporterLteEnbUlInterference")
      .SetParent<OranReporter>()
      .AddConstructor<OranReporterLteEnbUlInterference>()
      .AddAttribute("NearRtRic",
        "Near-RT RIC (used to resolve this eNB's primary cellId).",
        PointerValue(),
        MakePointerAccessor(&OranReporterLteEnbUlInterference::m_nearRtRic),
        MakePointerChecker<OranNearRtRic>());
  return tid;
}

OranReporterLteEnbUlInterference::OranReporterLteEnbUlInterference()
  : OranReporter()
{
}

void
OranReporterLteEnbUlInterference::Activate()
{
  if (m_active) return;
  NS_ABORT_MSG_IF(GetTerminator() == nullptr,
    "Reporter must be attached to a Terminator before Activate");

  // Resolve this eNB's primary cellId once (strict scoping)
  if (m_nearRtRic && m_nearRtRic->Data())
  {
    bool ok; uint16_t cid;
    std::tie(ok, cid) =
      m_nearRtRic->Data()->GetLteEnbCellInfo(GetTerminator()->GetE2NodeId());
    if (ok) m_myCellId = cid;
  }

  EnsureConnected();
  OranReporter::Activate();
}

void
OranReporterLteEnbUlInterference::Deactivate()
{
  if (!m_active) return;
  Disconnect();
  OranReporter::Deactivate();
}

void
OranReporterLteEnbUlInterference::DoDispose()
{
  Disconnect();
  m_samplesMw.clear();
  OranReporter::DoDispose();
}

void
OranReporterLteEnbUlInterference::EnsureConnected()
{
  if (m_connected) return;

  // eNB UL interference
  Config::Connect(
    "/NodeList/*/DeviceList/*/ComponentCarrierMap/*/LteEnbPhy/ReportInterference",
    MakeBoundCallback(&OranReporterLteEnbUlInterference::InterfCb, this));

  m_connected = true;
}

void
OranReporterLteEnbUlInterference::Disconnect()
{
  if (!m_connected) return;

  Config::Disconnect(
    "/NodeList/*/DeviceList/*/ComponentCarrierMap/*/LteEnbPhy/ReportInterference",
    MakeBoundCallback(&OranReporterLteEnbUlInterference::InterfCb, this));

  m_connected = false;
}

void
OranReporterLteEnbUlInterference::InterfCb(OranReporterLteEnbUlInterference* self,
                                          std::string /*path*/,
                                          uint16_t cellId,
                                          Ptr<SpectrumValue> interference)
{
  // Lazy resolve this eNB's primary cellId from the repo (retry until ready).
  if (self->m_myCellId == 0 && self->m_nearRtRic && self->m_nearRtRic->Data())
  {
    bool ok; uint16_t cid;
    std::tie(ok, cid) =
      self->m_nearRtRic->Data()->GetLteEnbCellInfo(self->GetTerminator()->GetE2NodeId());
    if (ok) self->m_myCellId = cid;
  }

  // If still unknown, do not "adopt" a cellId (avoid masking wiring/config issues).
  if (self->m_myCellId == 0)
  {
    return;
  }

  // Strict scope: only collect samples for this reporter's cell.
  if (cellId != self->m_myCellId)
  {
    return;
  }

  // Compute per-RB AVERAGE interference power (mW).
  // SpectrumValue entries are PSD in W/Hz per RB-sized bin.
  static const double RB_BW_HZ = 180000.0;

  double sumPsd = 0.0;
  uint32_t n = 0;
  for (auto it = interference->ConstValuesBegin(); it != interference->ConstValuesEnd(); ++it)
  {
    sumPsd += *it;  // W/Hz
    ++n;
  }
  if (n == 0)
  {
    return;
  }

  const double meanPsd_WperHz = sumPsd / double(n);
  const double meanMw = meanPsd_WperHz * RB_BW_HZ * 1e3; // (W/Hz * Hz) -> W -> mW
  if (meanMw <= 0.0)
  {
    return;
  }

  // Store under m_myCellId to avoid accidental key drift.
  self->m_samplesMw[self->m_myCellId].push_back(meanMw);
}



std::vector<Ptr<OranReport>>
OranReporterLteEnbUlInterference::GenerateReports()
{
  std::vector<Ptr<OranReport>> out;
  if (!m_active || m_myCellId == 0) return out;

  const Time now = Simulator::Now();
  const uint64_t e2 = GetTerminator()->GetE2NodeId();

  auto it = m_samplesMw.find(m_myCellId);
  if (it != m_samplesMw.end())
  {
    auto &vec = it->second;
    if (!vec.empty())
    {
      std::sort(vec.begin(), vec.end());
      const size_t n   = vec.size();
      const size_t idx = std::min(n - 1, size_t(std::ceil(0.95 * n) - 1));
      const double p95mw  = vec[idx];
      const double p95dbm = LinearToDbm(p95mw);

      Ptr<OranReportLteEnbUlInterf> r = CreateObject<OranReportLteEnbUlInterf>();
      r->SetAttribute("ReporterE2NodeId", UintegerValue(e2));
      r->SetAttribute("Time", TimeValue(now));
      r->SetCellId(m_myCellId);
      r->SetP95Mw(p95mw);
      r->SetP95Dbm(p95dbm);
      out.push_back(r);
    }
    vec.clear();
  }
  return out;
}



} // namespace ns3
