// oran-reporter-lte-ho-and-dwell.cc
#include "oran-reporter-lte-ho-and-dwell.h"
#include "oran-report-lte-ho-event.h"
#include "oran-report-lte-ue-dwell.h"
#include "oran-near-rt-ric.h"
#include "oran-data-repository.h"
#include "oran-e2-node-terminator.h"


#include <ns3/pointer.h>   // PointerValue, MakePointerAccessor, MakePointerChecker
#include <ns3/uinteger.h>  // UintegerValue
#include <ns3/nstime.h>    // TimeValue
#include <ns3/config.h>
#include <ns3/log.h>
#include <algorithm>

namespace ns3 {
NS_LOG_COMPONENT_DEFINE("OranReporterLteHoAndDwell");
NS_OBJECT_ENSURE_REGISTERED(OranReporterLteHoAndDwell);

TypeId
OranReporterLteHoAndDwell::GetTypeId()
{
  static TypeId tid =
    TypeId("ns3::OranReporterLteHoAndDwell")
      .SetParent<OranReporter>()
      .AddConstructor<OranReporterLteHoAndDwell>()
      .AddAttribute("NearRtRic",
        "Near-RT RIC to reach the data repository for UE ID mapping",
        PointerValue(),
        MakePointerAccessor(&OranReporterLteHoAndDwell::m_nearRtRic),
        MakePointerChecker<OranNearRtRic>());
  return tid;
}

OranReporterLteHoAndDwell::OranReporterLteHoAndDwell()
  : OranReporter()
{}

void OranReporterLteHoAndDwell::Activate()
{
  if (m_active) return;
  NS_ABORT_MSG_IF(GetTerminator()==nullptr, "Attach to a Terminator first");
  EnsureConnected();
  OranReporter::Activate();
}

void OranReporterLteHoAndDwell::Deactivate()
{
  if (!m_active) return;
  Disconnect();
  OranReporter::Deactivate();
}

void OranReporterLteHoAndDwell::DoDispose()
{
  Disconnect();
  m_pendingEvents.clear();
  m_dwell.clear();
  m_imsi2ueid.clear();
  m_pendingAttach.clear();
  m_lastHoStart.clear();
  OranReporter::DoDispose();
}

void OranReporterLteHoAndDwell::EnsureConnected()
{
  if (m_connected) return;

  // Initial attach (set dwell baseline)
  Config::Connect("/NodeList/*/DeviceList/*/LteEnbRrc/ConnectionEstablished",
                  MakeBoundCallback(&OranReporterLteHoAndDwell::ConnEstCb, this));

  // HO start / end
  Config::Connect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverStart",
                  MakeBoundCallback(&OranReporterLteHoAndDwell::HoStartCb, this));
  Config::Connect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverEndOk",
                  MakeBoundCallback(&OranReporterLteHoAndDwell::HoEndOkCb, this));

  // Failures → log as error events
  Config::Connect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverFailureNoPreamble",
                  MakeBoundCallback(&OranReporterLteHoAndDwell::HoFailNoPreamble, this));
  Config::Connect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverFailureMaxRach",
                  MakeBoundCallback(&OranReporterLteHoAndDwell::HoFailMaxRach, this));
  Config::Connect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverFailureLeaving",
                  MakeBoundCallback(&OranReporterLteHoAndDwell::HoFailLeaving, this));
  Config::Connect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverFailureJoining",
                  MakeBoundCallback(&OranReporterLteHoAndDwell::HoFailJoining, this));

  m_connected = true;
}

void OranReporterLteHoAndDwell::Disconnect()
{
  if (!m_connected) return;

  Config::Disconnect("/NodeList/*/DeviceList/*/LteEnbRrc/ConnectionEstablished",
                     MakeBoundCallback(&OranReporterLteHoAndDwell::ConnEstCb, this));
  Config::Disconnect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverStart",
                     MakeBoundCallback(&OranReporterLteHoAndDwell::HoStartCb, this));
  Config::Disconnect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverEndOk",
                     MakeBoundCallback(&OranReporterLteHoAndDwell::HoEndOkCb, this));

  Config::Disconnect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverFailureNoPreamble",
                     MakeBoundCallback(&OranReporterLteHoAndDwell::HoFailNoPreamble, this));
  Config::Disconnect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverFailureMaxRach",
                     MakeBoundCallback(&OranReporterLteHoAndDwell::HoFailMaxRach, this));
  Config::Disconnect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverFailureLeaving",
                     MakeBoundCallback(&OranReporterLteHoAndDwell::HoFailLeaving, this));
  Config::Disconnect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverFailureJoining",
                     MakeBoundCallback(&OranReporterLteHoAndDwell::HoFailJoining, this));

  m_connected = false;
}

uint64_t
OranReporterLteHoAndDwell::MapUe(uint16_t cellId, uint16_t rnti, uint64_t imsi) const
{
  uint64_t ueid = 0;
  if (m_nearRtRic && m_nearRtRic->Data())
  {
    ueid = m_nearRtRic->Data()->GetLteUeE2NodeIdFromCellInfo(cellId, rnti);
  }
  if (ueid == 0)
  {
    auto it = m_imsi2ueid.find(imsi);
    if (it != m_imsi2ueid.end()) ueid = it->second;
  }
  return ueid;
}

// ---- Callbacks ----
void
OranReporterLteHoAndDwell::ConnEstCb(OranReporterLteHoAndDwell* self, std::string,
                                     uint64_t imsi, uint16_t cellId, uint16_t rnti)
{
  const Time now = Simulator::Now();
  const uint64_t u = self->MapUe(cellId, rnti, imsi);

  if (u == 0)
  {
    // Mapping not ready yet: cache and retry in GenerateReports().
    // Do not overwrite an earlier baseline if we already have one.
    auto it = self->m_pendingAttach.find(imsi);
    if (it == self->m_pendingAttach.end() || now < it->second.t)
    {
      self->m_pendingAttach[imsi] = PendingAttach{imsi, cellId, rnti, now};
    }
    return;
  }

  // If it was pending before, clear it.
  self->m_pendingAttach.erase(imsi);

  self->m_imsi2ueid[imsi] = u;

  auto &st = self->m_dwell[u];
  st.imsi = imsi;
  st.ueid = u;
  st.currentCell = cellId;
  st.lastChange = now;
}

void
OranReporterLteHoAndDwell::HoStartCb(OranReporterLteHoAndDwell* self, std::string,
                                     uint64_t imsi, uint16_t srcCell, uint16_t rnti, uint16_t targetCell)
{
  const uint64_t u = self->MapUe(srcCell, rnti, imsi);
  if (u == 0) return;

  self->m_imsi2ueid[imsi] = u;

  // NEW: remember Start so EndOk can backfill srcCell if dwell state isn't seeded yet
  self->m_lastHoStart[imsi] = {srcCell, targetCell};

  EventRow e{imsi, u, srcCell, targetCell, "Start"};
  self->m_pendingEvents.push_back(std::move(e));
}

void
OranReporterLteHoAndDwell::HoEndOkCb(OranReporterLteHoAndDwell* self, std::string,
                                     uint64_t imsi, uint16_t cellId, uint16_t rnti)
{
  const uint64_t u = self->MapUe(cellId, rnti, imsi);
  if (u == 0) return;

  self->m_imsi2ueid[imsi] = u;

  auto &st = self->m_dwell[u];       // creates entry if missing
  uint16_t srcCell = st.currentCell; // may be 0 if first time seen

  // NEW: if dwell wasn't seeded yet, recover srcCell from last HO Start
  if (srcCell == 0)
  {
    auto hit = self->m_lastHoStart.find(imsi);
    if (hit != self->m_lastHoStart.end())
    {
      srcCell = hit->second.first; // src from Start
    }
  }

  // Update dwell state (this is the actual "arrived to cellId" moment)
  st.imsi = imsi;
  st.ueid = u;
  st.currentCell = cellId;
  st.lastChange  = Simulator::Now();

  // Emit EndOk only if we know srcCell
  if (srcCell != 0)
  {
    EventRow e{imsi, u, srcCell, cellId, "EndOk"};
    self->m_pendingEvents.push_back(std::move(e));
  }

  // consume the cached start (whether or not we emitted EndOk)
  self->m_lastHoStart.erase(imsi);
}





// Failure traces provide (imsi, srcCell, rnti); no target cell argument.
#define HO_FAIL_CB(name, ev) \
void OranReporterLteHoAndDwell::name(OranReporterLteHoAndDwell* s, std::string, \
                                     uint64_t imsi, uint16_t srcCell, uint16_t rnti) \
{ \
  const uint64_t u = s->MapUe(srcCell, rnti, imsi); \
  if (u == 0) return; \
  s->m_imsi2ueid[imsi] = u; \
  EventRow e{imsi, u, srcCell, 0, ev}; \
  s->m_pendingEvents.push_back(std::move(e)); \
}

HO_FAIL_CB(HoFailNoPreamble, "EndError:NoPreamble")
HO_FAIL_CB(HoFailMaxRach,     "EndError:MaxRach")
HO_FAIL_CB(HoFailLeaving,     "EndError:Leaving")
HO_FAIL_CB(HoFailJoining,     "EndError:Joining")
#undef HO_FAIL_CB

// ---- GenerateReports: flush events, output dwell snapshot ----
std::vector< Ptr<OranReport> >
OranReporterLteHoAndDwell::GenerateReports()
{
  std::vector< Ptr<OranReport> > out;
  if (!m_active) return out;

  const Time now = Simulator::Now();
  const uint64_t e2 = GetTerminator()->GetE2NodeId();

  for (auto it = m_pendingAttach.begin(); it != m_pendingAttach.end(); )
  {
    const PendingAttach &p = it->second;
    const uint64_t u = MapUe(p.cellId, p.rnti, p.imsi);
    if (u == 0)
    {
      ++it;
      continue;
    }

    m_imsi2ueid[p.imsi] = u;

    // Only seed if this UE is not tracked yet.
    if (m_dwell.find(u) == m_dwell.end())
    {
      DwellState st;
      st.imsi = p.imsi;
      st.ueid = u;
      st.currentCell = p.cellId;
      st.lastChange = p.t; // baseline: use ConnEst time
      m_dwell.emplace(u, st);
    }

    it = m_pendingAttach.erase(it);
  }


  // HO events
  for (const auto& e : m_pendingEvents)
  {
    Ptr<OranReportLteHoEvent> r = CreateObject<OranReportLteHoEvent>();
    r->SetAttribute("ReporterE2NodeId", UintegerValue(e2));
    r->SetAttribute("Time", TimeValue(now));
    r->SetImsi(e.imsi);
    r->SetUeId(e.ueid);
    r->SetSrcCell(e.src);
    r->SetDstCell(e.dst);
    r->SetEvent(e.ev);
    out.push_back(r);
  }
  m_pendingEvents.clear();

  // Dwell snapshot
  for (auto &kv : m_dwell)
  {
    const auto &st = kv.second;
    double dwellSec = (now - st.lastChange).GetSeconds();

    Ptr<OranReportLteUeDwell> dr = CreateObject<OranReportLteUeDwell>();
    dr->SetAttribute("ReporterE2NodeId", UintegerValue(e2));
    dr->SetAttribute("Time", TimeValue(now));
    dr->SetUeId(st.ueid);
    dr->SetCellId(st.currentCell);
    dr->SetDwellSeconds(dwellSec);
    out.push_back(dr);
  }

  return out;
}

} // ns3
