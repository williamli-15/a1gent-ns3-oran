// oran-reporter-lte-enb-ue-count.cc
#include "oran-reporter-lte-enb-ue-count.h"
#include "oran-report-lte-enb-ue-count.h"
#include "oran-near-rt-ric.h"
#include "oran-data-repository.h"
#include "oran-e2-node-terminator.h"

#include <ns3/pointer.h>   // PointerValue, MakePointerAccessor, MakePointerChecker
#include <ns3/uinteger.h>  // UintegerValue
#include <ns3/nstime.h>    // TimeValue
#include <ns3/config.h>
#include <ns3/log.h>
#include <ns3/simulator.h>

#include <tuple>

namespace ns3 {
NS_LOG_COMPONENT_DEFINE("OranReporterLteEnbUeCount");
NS_OBJECT_ENSURE_REGISTERED(OranReporterLteEnbUeCount);

TypeId
OranReporterLteEnbUeCount::GetTypeId()
{
  static TypeId tid = TypeId("ns3::OranReporterLteEnbUeCount")
    .SetParent<OranReporter>()
    .AddConstructor<OranReporterLteEnbUeCount>()
    .AddAttribute("NearRtRic",
      "Near-RT RIC to access the Data Repository (UEID mapping).",
      PointerValue(),
      MakePointerAccessor(&OranReporterLteEnbUeCount::m_nearRtRic),
      MakePointerChecker<OranNearRtRic>());
  return tid;
}

OranReporterLteEnbUeCount::OranReporterLteEnbUeCount() : OranReporter() {}

void OranReporterLteEnbUeCount::Activate()
{
  if (m_active) return;
  NS_ABORT_MSG_IF(GetTerminator()==nullptr, "Attach to a Terminator first");

  // Ask the repo which cell this eNB id maps to
  if (m_nearRtRic && m_nearRtRic->Data())
  {
    bool ok; uint16_t cid;
    std::tie(ok, cid) = m_nearRtRic->Data()->GetLteEnbCellInfo(GetTerminator()->GetE2NodeId());
    if (ok) m_myCellId = cid;
  }

  EnsureConnected();
  OranReporter::Activate();
}

void OranReporterLteEnbUeCount::Deactivate()
{
  if (!m_active) return;
  Disconnect();
  OranReporter::Deactivate();
}

void OranReporterLteEnbUeCount::DoDispose()
{
  Disconnect();
  m_cellRntis.clear();
  m_rntiCell.clear();
  OranReporter::DoDispose();
}

void OranReporterLteEnbUeCount::EnsureConnected()
{
  if (m_connected) return;
  Config::Connect("/NodeList/*/DeviceList/*/LteEnbRrc/ConnectionEstablished",
                  MakeBoundCallback(&OranReporterLteEnbUeCount::ConnEstCb, this));
  Config::Connect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverEndOk",
                  MakeBoundCallback(&OranReporterLteEnbUeCount::HoEndOkCb, this));
  Config::Connect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverStart",
                MakeBoundCallback(&OranReporterLteEnbUeCount::HoStartCb, this));
  m_connected = true;
}

void OranReporterLteEnbUeCount::Disconnect()
{
  if (!m_connected) return;
  Config::Disconnect("/NodeList/*/DeviceList/*/LteEnbRrc/ConnectionEstablished",
                     MakeBoundCallback(&OranReporterLteEnbUeCount::ConnEstCb, this));
  Config::Disconnect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverEndOk",
                     MakeBoundCallback(&OranReporterLteEnbUeCount::HoEndOkCb, this));
  Config::Disconnect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverStart",
                    MakeBoundCallback(&OranReporterLteEnbUeCount::HoStartCb, this));
  m_connected = false;
}

// ---- Trace sinks ----
void
OranReporterLteEnbUeCount::ConnEstCb(OranReporterLteEnbUeCount* self, std::string,
                                    uint64_t /*imsi*/, uint16_t cellId, uint16_t rnti)
{
  // Try repo first (lazy resolve until ready)
  if (self->m_myCellId == 0 && self->m_nearRtRic && self->m_nearRtRic->Data())
  {
    bool ok; uint16_t cid;
    std::tie(ok, cid) =
      self->m_nearRtRic->Data()->GetLteEnbCellInfo(self->GetTerminator()->GetE2NodeId());
    if (ok) self->m_myCellId = cid;
  }

  // Repo still not ready -> do NOT "adopt" any cellId (avoid poisoning state)
  if (self->m_myCellId == 0)
  {
    return;
  }

  // Only track my own cell
  if (cellId != self->m_myCellId)
  {
    return;
  }

  self->m_cellRntis[self->m_myCellId].insert(rnti);
  self->m_rntiCell[rnti] = self->m_myCellId;
}

void
OranReporterLteEnbUeCount::HoEndOkCb(OranReporterLteEnbUeCount* self, std::string,
                                    uint64_t /*imsi*/, uint16_t targetCell, uint16_t rnti)
{
  // Try repo first (lazy resolve until ready)
  if (self->m_myCellId == 0 && self->m_nearRtRic && self->m_nearRtRic->Data())
  {
    bool ok; uint16_t cid;
    std::tie(ok, cid) =
      self->m_nearRtRic->Data()->GetLteEnbCellInfo(self->GetTerminator()->GetE2NodeId());
    if (ok) self->m_myCellId = cid;
  }

  // Repo still not ready -> do nothing (avoid poisoning state)
  if (self->m_myCellId == 0)
  {
    return;
  }

  // If this HO EndOk is NOT for my cell, it means the UE is leaving my cell
  if (targetCell != self->m_myCellId)
  {
    auto it = self->m_rntiCell.find(rnti);
    if (it != self->m_rntiCell.end() && it->second == self->m_myCellId)
    {
      auto pit = self->m_cellRntis.find(self->m_myCellId);
      if (pit != self->m_cellRntis.end())
      {
        pit->second.erase(rnti);
      }
      self->m_rntiCell.erase(it);
    }
    return;
  }

  // UE arriving to my cell
  self->m_cellRntis[self->m_myCellId].insert(rnti);
  self->m_rntiCell[rnti] = self->m_myCellId;
}

void
OranReporterLteEnbUeCount::HoStartCb(OranReporterLteEnbUeCount* self, std::string,
                                    uint64_t /*imsi*/, uint16_t srcCell, uint16_t rnti, uint16_t /*dstCell*/)
{
  // Try repo first (lazy resolve until ready)
  if (self->m_myCellId == 0 && self->m_nearRtRic && self->m_nearRtRic->Data())
  {
    bool ok; uint16_t cid;
    std::tie(ok, cid) =
      self->m_nearRtRic->Data()->GetLteEnbCellInfo(self->GetTerminator()->GetE2NodeId());
    if (ok) self->m_myCellId = cid;
  }

  // Repo still not ready -> do nothing (avoid poisoning state)
  if (self->m_myCellId == 0)
  {
    return;
  }

  // We only act if we are the source cell
  if (srcCell != self->m_myCellId)
  {
    return;
  }

  auto pit = self->m_cellRntis.find(self->m_myCellId);
  if (pit != self->m_cellRntis.end())
  {
    pit->second.erase(rnti);
  }
  self->m_rntiCell.erase(rnti);
}

// ---- Periodic emission ----
std::vector<Ptr<OranReport>>
OranReporterLteEnbUeCount::GenerateReports()
{
  std::vector<Ptr<OranReport>> out;
  if (!m_active) return out;

  const Time now = Simulator::Now();
  const uint64_t e2 = GetTerminator()->GetE2NodeId();

  if (m_myCellId != 0)
  {
    const auto it = m_cellRntis.find(m_myCellId);
    const uint32_t count = (it == m_cellRntis.end()) ? 0u
                                                     : static_cast<uint32_t>(it->second.size());
    Ptr<OranReportLteEnbUeCount> r = CreateObject<OranReportLteEnbUeCount>();
    r->SetAttribute("ReporterE2NodeId", UintegerValue(e2));
    r->SetAttribute("Time",             TimeValue(now));
    r->SetCellId(m_myCellId);
    r->SetUeCount(count);
    out.push_back(r);
  }
  return out;
}


} // ns3
