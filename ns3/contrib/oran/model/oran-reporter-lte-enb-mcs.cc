#include "oran-reporter-lte-enb-mcs.h"
#include "oran-report-lte-enb-mcs.h"
#include "oran-e2-node-terminator.h"
#include "oran-near-rt-ric.h"
#include "oran-data-repository.h"

#include <ns3/pointer.h>
#include <ns3/string.h>
#include <ns3/log.h>

#include <tuple>

namespace ns3 {
NS_LOG_COMPONENT_DEFINE("OranReporterLteEnbMcs");
NS_OBJECT_ENSURE_REGISTERED(OranReporterLteEnbMcs);

TypeId
OranReporterLteEnbMcs::GetTypeId()
{
  static TypeId tid = TypeId("ns3::OranReporterLteEnbMcs")
    .SetParent<OranReporter>()
    .AddConstructor<OranReporterLteEnbMcs>()
    .AddAttribute("NearRtRic",
      "Near-RT RIC instance for data-repo lookups",
      PointerValue(),
      MakePointerAccessor(&OranReporterLteEnbMcs::m_nearRtRic),
      MakePointerChecker<OranNearRtRic>());
  return tid;
}

OranReporterLteEnbMcs::OranReporterLteEnbMcs() : OranReporter() {}
OranReporterLteEnbMcs::~OranReporterLteEnbMcs() { NS_LOG_FUNCTION(this); }

void OranReporterLteEnbMcs::DoDispose()
{
  Disconnect();
  m_rntiToCell.clear();
  m_dl = Hist{}; m_ul = Hist{};
  OranReporter::DoDispose();
}

void OranReporterLteEnbMcs::Activate()
{
  if (m_active) return;
  auto term = GetTerminator();
  NS_ABORT_MSG_IF(term == nullptr, "Reporter must be attached to a Terminator");

  // Find this eNB’s primary cell via repo (for filtering)
  if (m_nearRtRic && m_nearRtRic->Data())
  {
    bool ok; uint16_t cellId;
    std::tie(ok, cellId) = m_nearRtRic->Data()->GetLteEnbCellInfo(term->GetE2NodeId());
    if (ok) m_thisCellId = cellId;
  }

  m_windowStart = Simulator::Now();
  EnsureConnected();
  OranReporter::Activate();
}

void OranReporterLteEnbMcs::Deactivate()
{
  if (!m_active) return;
  Disconnect();
  OranReporter::Deactivate();
}

void OranReporterLteEnbMcs::EnsureConnected()
{
  if (m_tracesConnected) return;

  // Keep rnti→cellId for this eNB
  Config::Connect("/NodeList/*/DeviceList/*/LteEnbRrc/NewUeContext",
    MakeBoundCallback(&OranReporterLteEnbMcs::OnNewUeContext, this));

  // MAC scheduling traces (per CC)
  Config::Connect("/NodeList/*/DeviceList/*/ComponentCarrierMap/*/LteEnbMac/DlScheduling",
    MakeBoundCallback(&OranReporterLteEnbMcs::OnDlScheduling, this));

  Config::Connect("/NodeList/*/DeviceList/*/ComponentCarrierMap/*/LteEnbMac/UlScheduling",
    MakeBoundCallback(&OranReporterLteEnbMcs::OnUlScheduling, this));

  m_tracesConnected = true;
}

void OranReporterLteEnbMcs::Disconnect()
{
  if (!m_tracesConnected) return;

  Config::Disconnect("/NodeList/*/DeviceList/*/LteEnbRrc/NewUeContext",
    MakeBoundCallback(&OranReporterLteEnbMcs::OnNewUeContext, this));

  Config::Disconnect("/NodeList/*/DeviceList/*/ComponentCarrierMap/*/LteEnbMac/DlScheduling",
    MakeBoundCallback(&OranReporterLteEnbMcs::OnDlScheduling, this));

  Config::Disconnect("/NodeList/*/DeviceList/*/ComponentCarrierMap/*/LteEnbMac/UlScheduling",
    MakeBoundCallback(&OranReporterLteEnbMcs::OnUlScheduling, this));

  m_tracesConnected = false;
}

// 1) NewUeContext: record the mapping only. DO NOT touch m_thisCellId here.
void
OranReporterLteEnbMcs::OnNewUeContext(OranReporterLteEnbMcs* self,
                                      std::string /*ctx*/,
                                      uint16_t cellId,
                                      uint16_t rnti)
{
  self->m_rntiToCell[rnti] = cellId;
}

// 2) DlScheduling
void
OranReporterLteEnbMcs::OnDlScheduling(OranReporterLteEnbMcs* self,
                                      std::string /*path*/,
                                      DlSchedulingCallbackInfo info)
{
  // Lazy resolve the primary cellId for this eNB reporter
  if (self->m_thisCellId == 0 && self->m_nearRtRic && self->m_nearRtRic->Data())
  {
    bool ok; uint16_t cid;
    std::tie(ok, cid) =
        self->m_nearRtRic->Data()->GetLteEnbCellInfo(self->GetTerminator()->GetE2NodeId());
    if (ok) self->m_thisCellId = cid;
  }

  // If still unknown, do not attempt to infer; wait for repo mapping.
  if (self->m_thisCellId == 0)
  {
    return;
  }

  // Filter by rnti->cell mapping (learned from NewUeContext)
  auto it = self->m_rntiToCell.find(info.rnti);
  if (it == self->m_rntiToCell.end() || it->second != self->m_thisCellId)
  {
    return;
  }

  if (info.sizeTb1 > 0) OranReporterLteEnbMcs::Add(self->m_dl, info.mcsTb1);
  if (info.sizeTb2 > 0) OranReporterLteEnbMcs::Add(self->m_dl, info.mcsTb2);
}

// 3) UlScheduling
void
OranReporterLteEnbMcs::OnUlScheduling(OranReporterLteEnbMcs* self,
                                      std::string /*path*/,
                                      uint32_t /*frameNo*/,
                                      uint32_t /*subframeNo*/,
                                      uint16_t rnti,
                                      uint8_t  mcs,
                                      uint16_t tbs,
                                      uint8_t  /*ccId*/)
{
  // Lazy resolve the primary cellId for this eNB reporter
  if (self->m_thisCellId == 0 && self->m_nearRtRic && self->m_nearRtRic->Data())
  {
    bool ok; uint16_t cid;
    std::tie(ok, cid) =
        self->m_nearRtRic->Data()->GetLteEnbCellInfo(self->GetTerminator()->GetE2NodeId());
    if (ok) self->m_thisCellId = cid;
  }

  if (self->m_thisCellId == 0)
  {
    return;
  }

  // Filter by rnti->cell mapping
  auto it = self->m_rntiToCell.find(rnti);
  if (it == self->m_rntiToCell.end() || it->second != self->m_thisCellId)
  {
    return;
  }

  if (tbs > 0) OranReporterLteEnbMcs::Add(self->m_ul, mcs);
}

// bins helpers
void OranReporterLteEnbMcs::Add(Hist& h, uint8_t mcs)
{
  if (mcs >= h.bins.size()) return;
  h.bins[mcs]++; h.n++; h.sum += mcs;
}
double OranReporterLteEnbMcs::Mean(const Hist& h)
{
  return (h.n == 0) ? 0.0 : static_cast<double>(h.sum) / static_cast<double>(h.n);
}
double OranReporterLteEnbMcs::Pxx(const Hist& h, double q)
{
  if (h.n == 0) return 0.0;
  const uint64_t tgt = static_cast<uint64_t>(std::ceil(q * h.n));
  uint64_t acc = 0;
  for (size_t m = 0; m < h.bins.size(); ++m)
  {
    acc += h.bins[m];
    if (acc >= tgt) return static_cast<double>(m);
  }
  return static_cast<double>(h.bins.size()-1);
}

std::vector<Ptr<OranReport>>
OranReporterLteEnbMcs::GenerateReports()
{
  std::vector<Ptr<OranReport>> out;
  if (!m_active) return out;

  // NEW: do not emit a row for cell 0, or when no samples
  if (m_thisCellId == 0 || (m_dl.n == 0 && m_ul.n == 0)) {
    m_dl = Hist{}; m_ul = Hist{};
    m_windowStart = Simulator::Now();
    return out;
  }

  const Time now = Simulator::Now();
  const double dlMean = Mean(m_dl), dlP50 = Pxx(m_dl, 0.50), dlP95 = Pxx(m_dl, 0.95);
  const double ulMean = Mean(m_ul), ulP50 = Pxx(m_ul, 0.50), ulP95 = Pxx(m_ul, 0.95);

  Ptr<OranReportLteEnbMcs> rpt = CreateObject<OranReportLteEnbMcs>();
  rpt->SetAttribute("ReporterE2NodeId", UintegerValue(GetTerminator()->GetE2NodeId()));
  rpt->SetAttribute("Time",             TimeValue(now));
  rpt->SetCellId(m_thisCellId);
  rpt->SetDlMean(dlMean); rpt->SetDlP50(dlP50); rpt->SetDlP95(dlP95);
  rpt->SetUlMean(ulMean); rpt->SetUlP50(ulP50); rpt->SetUlP95(ulP95);
  out.push_back(rpt);

  m_dl = Hist{}; m_ul = Hist{};
  m_windowStart = now;
  return out;
}


} // namespace ns3
