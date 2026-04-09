// oran-reporter-lte-ue-prb.cc
#include "oran-reporter-lte-ue-prb.h"
#include "oran-report-lte-ue-prb.h"
#include "oran-near-rt-ric.h"
#include "oran-data-repository.h"

#include <ns3/pointer.h>
#include <ns3/string.h>
#include "ns3/config.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/lte-enb-mac.h"
#include "ns3/lte-enb-net-device.h"
#include "ns3/type-id.h"

namespace ns3 {
NS_LOG_COMPONENT_DEFINE("OranReporterLteUePrb");
NS_OBJECT_ENSURE_REGISTERED(OranReporterLteUePrb);

static uint16_t
CellIdFromEnbMacPath(const std::string& path)
{
  // path example:
  // /NodeList/7/DeviceList/1/ComponentCarrierMap/0/LteEnbMac/DlScheduling
  std::string devPath = path;

  auto p = devPath.find("/ComponentCarrierMap");
  if (p != std::string::npos)
  {
    devPath = devPath.substr(0, p);   // -> /NodeList/7/DeviceList/1
  }
  else
  {
    // fallback if format differs
    p = devPath.find("/LteEnbMac");
    if (p != std::string::npos)
    {
      devPath = devPath.substr(0, p);
    }
  }

  Config::MatchContainer m = Config::LookupMatches(devPath);
  if (m.GetN() == 0)
  {
    return 0;
  }

  Ptr<LteEnbNetDevice> enbNet = m.Get(0)->GetObject<LteEnbNetDevice>();
  if (!enbNet)
  {
    return 0;
  }
  return enbNet->GetCellId();
}

TypeId OranReporterLteUePrb::GetTypeId()
{
  static TypeId tid = TypeId("ns3::OranReporterLteUePrb")
    .SetParent<OranReporter>()
    .AddConstructor<OranReporterLteUePrb>()
    .AddAttribute("NearRtRic",
      "Near-RT RIC instance (used to map (cellId,rnti)->UE E2 node Id).",
      PointerValue(),
      MakePointerAccessor(&OranReporterLteUePrb::m_nearRtRic),
      MakePointerChecker<OranNearRtRic>());
  return tid;
}

OranReporterLteUePrb::OranReporterLteUePrb()
{
  m_amc = CreateObject<LteAmc>();
}

OranReporterLteUePrb::~OranReporterLteUePrb() { }

void OranReporterLteUePrb::DoDispose()
{
  Disconnect();
  m_agg.clear();
  m_ueManagerPathByCellIdRnti.clear();
  m_amc = nullptr;
  OranReporter::DoDispose();
}

void OranReporterLteUePrb::Activate()
{
  if (m_active) return;
  NS_ABORT_MSG_IF(GetTerminator()==nullptr, "Attach to a Terminator first");
  m_windowStart = Simulator::Now();
  EnsureConnected();
  SeedExistingUeMappings();
  OranReporter::Activate();
}

void OranReporterLteUePrb::Deactivate()
{
  if (!m_active) return;
  Disconnect();
  OranReporter::Deactivate();
}

void OranReporterLteUePrb::EnsureConnected()
{
  if (m_tracesConnected) return;

  // For cellId/rnti bookkeeping
  Config::Connect("/NodeList/*/DeviceList/*/LteEnbRrc/NewUeContext",
    MakeBoundCallback(&OranReporterLteUePrb::NotifyNewUeContextEnb, this));

  // DL/UL scheduling taps
  Config::Connect("/NodeList/*/DeviceList/*/ComponentCarrierMap/*/LteEnbMac/DlScheduling",
    MakeBoundCallback(&OranReporterLteUePrb::DlSchedCb, this));
  Config::Connect("/NodeList/*/DeviceList/*/ComponentCarrierMap/*/LteEnbMac/UlScheduling",
    MakeBoundCallback(&OranReporterLteUePrb::UlSchedCb, this));

  m_tracesConnected = true;
}

void OranReporterLteUePrb::Disconnect()
{
  if (!m_tracesConnected) return;

  Config::Disconnect("/NodeList/*/DeviceList/*/LteEnbRrc/NewUeContext",
    MakeBoundCallback(&OranReporterLteUePrb::NotifyNewUeContextEnb, this));
  Config::Disconnect("/NodeList/*/DeviceList/*/ComponentCarrierMap/*/LteEnbMac/DlScheduling",
    MakeBoundCallback(&OranReporterLteUePrb::DlSchedCb, this));
  Config::Disconnect("/NodeList/*/DeviceList/*/ComponentCarrierMap/*/LteEnbMac/UlScheduling",
    MakeBoundCallback(&OranReporterLteUePrb::UlSchedCb, this));

  m_tracesConnected = false;
}

void OranReporterLteUePrb::SeedExistingUeMappings()
{
  Config::MatchContainer existing =
      Config::LookupMatches("/NodeList/*/DeviceList/*/LteEnbRrc/UeMap/*");
  NS_LOG_INFO ("SeedExistingUeMappings: found " << existing.GetN()
               << " existing UE manager entries");
  for (uint32_t i = 0; i < existing.GetN(); ++i)
  {
    std::string path = existing.GetMatchedPath(i);
    auto slashPos = path.find_last_of('/');
    if (slashPos == std::string::npos)
    {
      continue;
    }
    std::string rntiStr = path.substr(slashPos + 1);
    uint16_t rnti = static_cast<uint16_t>(std::stoul(rntiStr));

    std::string enbDevicePath = path.substr(0, path.find("/LteEnbRrc"));
    Config::MatchContainer enbMatch = Config::LookupMatches(enbDevicePath);
    if (enbMatch.GetN() == 0)
    {
      continue;
    }
    Ptr<LteEnbNetDevice> enbNet = enbMatch.Get(0)->GetObject<LteEnbNetDevice>();
    if (!enbNet)
    {
      continue;
    }
    uint16_t cellId = enbNet->GetCellId();
    CellIdRnti key{cellId, rnti};
    if (m_ueManagerPathByCellIdRnti.find(key) == m_ueManagerPathByCellIdRnti.end())
    {
      m_ueManagerPathByCellIdRnti[key] = path;
      NS_LOG_DEBUG ("SeedExistingUeMappings: cached cell=" << cellId
                    << " rnti=" << rnti << " path=" << path);
    }
  }
}

 void OranReporterLteUePrb::NotifyNewUeContextEnb(OranReporterLteUePrb* self,
                                                  std::string ctx,
                                                  uint16_t cellId, uint16_t rnti)
 {
  const std::string ueMgrPath =
    ctx.substr(0, ctx.rfind('/')) + "/UeMap/" + std::to_string(rnti);
  self->m_ueManagerPathByCellIdRnti[{cellId,rnti}] = ueMgrPath;
  self->StoreUeManagerPath(ctx, cellId, rnti);
 }

void OranReporterLteUePrb::StoreUeManagerPath(const std::string& ctx,
                                              uint16_t cellId,
                                              uint16_t rnti)
{
  const std::string ueMgrPath =
      ctx.substr(0, ctx.rfind('/')) + "/UeMap/" + std::to_string(rnti);
  m_ueManagerPathByCellIdRnti[{cellId, rnti}] = ueMgrPath;
  NS_LOG_DEBUG ("StoreUeManagerPath: cell=" << cellId << " rnti=" << rnti
                << " path=" << ueMgrPath);
}

uint16_t OranReporterLteUePrb::GuessCellIdFromRnti(uint16_t rnti) const
{
  Ptr<OranDataRepository> repo = m_nearRtRic ? m_nearRtRic->Data() : nullptr;
  uint16_t fallback = 0;
  for (const auto& kv : m_ueManagerPathByCellIdRnti)
  {
    if (kv.first.rnti != rnti)
    {
      continue;
    }
    uint16_t candidate = kv.first.cellId;
    if (!repo || repo->GetLteUeE2NodeIdFromCellInfo(candidate, rnti) != 0)
    {
      return candidate;
    }
    if (fallback == 0)
    {
      fallback = candidate;
    }
  }
  return fallback;
}

uint64_t OranReporterLteUePrb::MapUeToE2Id(uint16_t cellId, uint16_t rnti) const
{
  if (!m_nearRtRic) return 0;
  Ptr<OranDataRepository> repo = m_nearRtRic->Data();
  if (!repo) return 0;
  return repo->GetLteUeE2NodeIdFromCellInfo(cellId, rnti);
}

int OranReporterLteUePrb::MinPrbForTb(bool dl, uint8_t mcs, uint32_t sizeBytes) const
{
  if (mcs == 0 || sizeBytes == 0) return 0;
  const int bits = static_cast<int>(sizeBytes) * 8;
  // LTE max PRBs 110 (20 MHz); loop up to this hard cap safely
  for (int n = 1; n <= 110; ++n)
  {
    int tb = dl ? m_amc->GetDlTbSizeFromMcs(mcs, n)
                : m_amc->GetUlTbSizeFromMcs(mcs, n);
    if (tb >= bits) return n;
  }
  // Fallback (shouldn't happen)
  return 0;
}

void OranReporterLteUePrb::DlSchedCb(OranReporterLteUePrb* self,
                                     std::string path,
                                     DlSchedulingCallbackInfo info)
{
  // derive cellId from our UE map
  uint16_t cellId = CellIdFromEnbMacPath(path);
  if (cellId == 0) { NS_LOG_WARN("DlSchedCb: cannot parse cellId from path="<<path); return; }

  uint64_t ueid = self->MapUeToE2Id(cellId, info.rnti);
  if (ueid == 0) return;

  // Compute minimal PRB to support BOTH TBs (same PRBs used for both CWs in MIMO):
  int n1 = self->MinPrbForTb(true /*dl*/, info.mcsTb1, info.sizeTb1);
  int n2 = self->MinPrbForTb(true /*dl*/, info.mcsTb2, info.sizeTb2);
  int n  = std::max(n1, n2);

  if (n > 0)
  {
    Key key{ueid, cellId};
    self->m_agg[key].dlPrb += static_cast<uint64_t>(n);
  }
}

void OranReporterLteUePrb::UlSchedCb(OranReporterLteUePrb* self,
                                     std::string path,
                                     uint32_t /*frame*/, uint32_t /*subframe*/,
                                     uint16_t rnti, uint8_t mcs, uint16_t size, uint8_t /*ccId*/)
{
  uint16_t cellId = CellIdFromEnbMacPath(path);
  if (cellId == 0) { NS_LOG_WARN("UlSchedCb: cannot parse cellId from path="<<path); return; }

  uint64_t ueid = self->MapUeToE2Id(cellId, rnti);
  if (ueid == 0) return;

  int n = self->MinPrbForTb(false /*ul*/, mcs, size);
  if (n > 0)
  {
    Key key{ueid, cellId};
    self->m_agg[key].ulPrb += static_cast<uint64_t>(n);
  }
}

std::vector< Ptr<OranReport> > OranReporterLteUePrb::GenerateReports()
{
  std::vector< Ptr<OranReport> > out;
  if (!m_active) return out;

  const Time now = Simulator::Now();
  for (auto& kv : m_agg)
  {
    Ptr<OranReportLteUePrb> rpt = CreateObject<OranReportLteUePrb>();
    rpt->SetAttribute("ReporterE2NodeId", UintegerValue(GetTerminator()->GetE2NodeId()));
    rpt->SetAttribute("Time", TimeValue(now));
    rpt->SetUeId(kv.first.ueid);
    rpt->SetCellId(kv.first.cellId);
    rpt->SetDlPrbs(kv.second.dlPrb);
    rpt->SetUlPrbs(kv.second.ulPrb);
    out.push_back(rpt);

    kv.second.dlPrb = 0;
    kv.second.ulPrb = 0;
  }
  m_windowStart = now;
  return out;
}

} // ns3
