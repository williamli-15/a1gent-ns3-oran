#include "oran-reporter-lte-ue-pdcp-throughput.h"
#include "oran-report-lte-ue-pdcp-throughput.h"
#include "oran-e2-node-terminator.h"
#include "oran-near-rt-ric.h"
#include "oran-data-repository.h"

#include <ns3/config.h>
#include <ns3/log.h>
#include <ns3/pointer.h>
#include <ns3/string.h>
#include <ns3/simulator.h>
#include <ns3/nstime.h>
#include <ns3/uinteger.h>
#include <ns3/lte-enb-net-device.h>
#include <ns3/lte-ue-rrc.h>

#include <algorithm>
#include <sstream>
#include <tuple>
#include <array>


namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("OranReporterLteUePdcpThroughput");
NS_OBJECT_ENSURE_REGISTERED (OranReporterLteUePdcpThroughput);

TypeId
OranReporterLteUePdcpThroughput::GetTypeId ()
{
  static TypeId tid = TypeId ("ns3::OranReporterLteUePdcpThroughput")
    .SetParent<OranReporter> ()
    .AddConstructor<OranReporterLteUePdcpThroughput> ()
    .AddAttribute ("NearRtRic",
                   "Near-RT RIC instance so we can access the Data Repository",
                   PointerValue (),  // default nullptr
                   MakePointerAccessor (&OranReporterLteUePdcpThroughput::m_nearRtRic),
                   MakePointerChecker<OranNearRtRic> ())
    ;
  return tid;
}

OranReporterLteUePdcpThroughput::OranReporterLteUePdcpThroughput ()
  : OranReporter (),
    m_nearRtRic(nullptr)
{
}

OranReporterLteUePdcpThroughput::~OranReporterLteUePdcpThroughput ()
{
  NS_LOG_FUNCTION (this);
}

void
OranReporterLteUePdcpThroughput::Activate ()
{
  NS_LOG_FUNCTION (this);
  if (m_active)
    return;

  Ptr<OranE2NodeTerminator> term = GetTerminator ();
  NS_ABORT_MSG_IF (term == nullptr, "Reporter must be attached to a Terminator before Activate");

  m_windowStart = Simulator::Now ();
  EnsureConnected ();

  if (m_knownCellIds.empty() && m_nearRtRic)
  {
    Ptr<OranDataRepository> repo = m_nearRtRic->Data ();
    if (repo)
    {
      std::vector<uint64_t> enbIds = repo->GetLteEnbE2NodeIds ();
      for (uint64_t enbId : enbIds)
      {
        auto info = repo->GetLteEnbCellInfo (enbId);
        if (std::get<0> (info))
        {
          uint16_t cellId = std::get<1> (info);
          if (std::find (m_knownCellIds.begin (), m_knownCellIds.end (), cellId) == m_knownCellIds.end ())
          {
            m_knownCellIds.push_back (cellId);
          }
        }
      }
    }
  }

  Config::MatchContainer existing =
      Config::LookupMatches ("/NodeList/*/DeviceList/*/LteEnbRrc/UeMap/*");
  Config::MatchContainer ueRrcMatches =
      Config::LookupMatches ("/NodeList/*/DeviceList/*/LteUeRrc");
  for (uint32_t i = 0; i < existing.GetN (); ++i)
  {
    std::string path = existing.GetMatchedPath (i);
    auto slashPos = path.find_last_of ('/');
    if (slashPos == std::string::npos)
    {
      continue;
    }
    std::string rntiStr = path.substr (slashPos + 1);
    uint16_t rnti = static_cast<uint16_t> (std::stoi (rntiStr));

    std::string enbDevicePath = path.substr (0, path.find ("/LteEnbRrc"));
    Config::MatchContainer enbMatch = Config::LookupMatches (enbDevicePath);
    if (enbMatch.GetN () == 0)
    {
      continue;
    }
    Ptr<LteEnbNetDevice> enbNet = enbMatch.Get (0)->GetObject<LteEnbNetDevice> ();
    if (!enbNet)
    {
      continue;
    }
    uint16_t cellId = enbNet->GetCellId ();
    if (m_nearRtRic)
    {
      Ptr<OranDataRepository> repo = m_nearRtRic->Data ();
      // Adjust cellId if orthogonal SCells are reported as 1..N inside the UE manager
      if (repo)
      {
        uint64_t mapId = repo->GetLteUeE2NodeIdFromCellInfo (cellId, rnti);
        if (mapId == 0)
        {
          // try other known cells
          for (uint16_t candidate : m_knownCellIds)
          {
            if (repo->GetLteUeE2NodeIdFromCellInfo (candidate, rnti) != 0)
            {
              cellId = candidate;
              break;
            }
          }
        }
      }
    }
    CellIdRnti key{cellId, rnti};
    if (m_ueManagerPathByCellIdRnti.find (key) == m_ueManagerPathByCellIdRnti.end ())
    {
      m_ueManagerPathByCellIdRnti[key] = path;
      Config::Connect (path + "/DrbCreated",
                       MakeBoundCallback (&OranReporterLteUePdcpThroughput::CreatedDrbEnb, this));
    }

    std::string ueRrcPath;
    for (uint32_t j = 0; j < ueRrcMatches.GetN (); ++j)
    {
      Ptr<Object> obj = ueRrcMatches.Get (j);
      Ptr<LteUeRrc> ueRrc = obj->GetObject<LteUeRrc> ();
      if (!ueRrc)
      {
        continue;
      }
      if (ueRrc->GetRnti () == rnti && ueRrc->GetCellId () == cellId)
      {
        ueRrcPath = ueRrcMatches.GetMatchedPath (j);
        break;
      }
    }

    Config::MatchContainer drbExisting =
        Config::LookupMatches (path + "/DataRadioBearerMap/*");
    for (uint32_t k = 0; k < drbExisting.GetN (); ++k)
    {
      std::string drbPath = drbExisting.GetMatchedPath (k);
      auto idxPos = drbPath.find_last_of ('/');
      if (idxPos == std::string::npos)
      {
        continue;
      }
      unsigned idx = static_cast<unsigned> (std::stoul (drbPath.substr (idxPos + 1)));
      uint8_t lcid = static_cast<uint8_t> (idx + 2);
      ConnectDrbEnb (path, cellId, lcid);
      if (!ueRrcPath.empty())
      {
        ConnectDrbUe (ueRrcPath, cellId, lcid);
      }
    }
  }

  OranReporter::Activate ();
}

void
OranReporterLteUePdcpThroughput::Deactivate ()
{
  NS_LOG_FUNCTION (this);
  if (!m_active)
    return;

  Disconnect ();
  OranReporter::Deactivate ();
}

void
OranReporterLteUePdcpThroughput::DoDispose ()
{
  Disconnect ();
  m_agg.clear ();
  m_ueManagerPathByCellIdRnti.clear ();
  OranReporter::DoDispose ();
}

std::vector< Ptr<OranReport> >
OranReporterLteUePdcpThroughput::GenerateReports ()
{
  NS_LOG_FUNCTION (this);
  std::vector< Ptr<OranReport> > out;
  if (!m_active)
    return out;

  const Time now = Simulator::Now ();
  const int64_t elapsedMs = std::max<int64_t> (1, (now - m_windowStart).GetMilliSeconds ());
  const double denom = static_cast<double>(elapsedMs) / 1000.0; // seconds

  for (auto &kv : m_agg)
  {
    const uint64_t ueid = kv.first;
    UeAgg &agg = kv.second;

    const double dlMbps = (agg.dlBytes * 8.0) / denom / 1e6;
    const double ulMbps = (agg.ulBytes * 8.0) / denom / 1e6;

    Ptr<OranReportLteUePdcpThroughput> rpt = CreateObject<OranReportLteUePdcpThroughput> ();
    rpt->SetAttribute ("ReporterE2NodeId", UintegerValue (GetTerminator ()->GetE2NodeId ()));
    rpt->SetAttribute ("Time", TimeValue (now));
    rpt->SetUeId (ueid);
    rpt->SetCellId (agg.cellId);
    rpt->SetDlMbps (dlMbps);
    rpt->SetUlMbps (ulMbps);

    out.push_back (rpt);

    // reset bytes for next window; keep last cellId
    agg.dlBytes = 0;
    agg.ulBytes = 0;
  }

  m_windowStart = now;
  return out;
}

// ----------------- trace hookup -----------------

void
OranReporterLteUePdcpThroughput::EnsureConnected ()
{
  if (m_tracesConnected)
    return;

  // Track UeManager paths (enb side)
  Config::Connect ("/NodeList/*/DeviceList/*/LteEnbRrc/NewUeContext",
                   MakeBoundCallback (&OranReporterLteUePdcpThroughput::NotifyNewUeContextEnb, this));

  // DRB created (UE)
  Config::Connect ("/NodeList/*/DeviceList/*/LteUeRrc/DrbCreated",
                   MakeBoundCallback (&OranReporterLteUePdcpThroughput::CreatedDrbUe, this));

  // DRB created (we connect on the UeManager path, set later in StoreUeManagerPath)
  // See StoreUeManagerPath(): it also hooks "…/DrbCreated" per-RNTI

  m_tracesConnected = true;
}

uint16_t
OranReporterLteUePdcpThroughput::ResolveCellId(uint16_t rnti)
{
  Ptr<OranDataRepository> repo = m_nearRtRic ? m_nearRtRic->Data () : nullptr;
  uint16_t preferredCellId = 0;
  // Prefer mapped cells that the repository already knows about.
  for (const auto& kv : m_ueManagerPathByCellIdRnti)
  {
    if (kv.first.rnti != rnti)
    {
      continue;
    }
    uint16_t candidate = kv.first.cellId;
    if (!repo || repo->GetLteUeE2NodeIdFromCellInfo (candidate, rnti) != 0)
    {
      return candidate;
    }
    if (preferredCellId == 0)
    {
      preferredCellId = candidate;
    }
  }
  if (preferredCellId != 0)
  {
    return preferredCellId;
  }

  uint16_t fallbackCellId = 0;
  std::string fallbackPath;

  std::ostringstream ueMapPath;
  ueMapPath << "/NodeList/*/DeviceList/*/LteEnbRrc/UeMap/" << static_cast<unsigned> (rnti);
  Config::MatchContainer matches = Config::LookupMatches (ueMapPath.str ());
  for (uint32_t i = 0; i < matches.GetN (); ++i)
  {
    std::string path = matches.GetMatchedPath (i);
    std::string enbDevicePath = path.substr (0, path.find ("/LteEnbRrc"));
    Config::MatchContainer enbMatch = Config::LookupMatches (enbDevicePath);
    if (enbMatch.GetN () == 0)
    {
      continue;
    }
    Ptr<Object> enbObj = enbMatch.Get (0);
    Ptr<LteEnbNetDevice> enbNet = enbObj->GetObject<LteEnbNetDevice> ();
    if (!enbNet)
    {
      continue;
    }
    uint16_t cellId = enbNet->GetCellId ();
    if (fallbackCellId == 0)
    {
      fallbackCellId = cellId;
      fallbackPath = path;
    }

    if (repo)
    {
      uint64_t ueid = repo->GetLteUeE2NodeIdFromCellInfo (cellId, rnti);
      if (ueid == 0)
      {
        continue;
      }
    }

    CellIdRnti key{cellId, rnti};
    m_ueManagerPathByCellIdRnti[key] = path;
    return cellId;
  }

  if (fallbackCellId != 0)
  {
    CellIdRnti key{fallbackCellId, rnti};
    m_ueManagerPathByCellIdRnti[key] = fallbackPath;
    return fallbackCellId;
  }

  if (repo)
  {
    for (uint16_t cellId : m_knownCellIds)
    {
      if (repo->GetLteUeE2NodeIdFromCellInfo (cellId, rnti) != 0)
      {
        CellIdRnti key{cellId, rnti};
        m_ueManagerPathByCellIdRnti[key] = "";
        return cellId;
      }
    }
  }

  NS_LOG_WARN ("ResolveCellId: unable to resolve cell for rnti=" << rnti);
  return 0;
}

void
OranReporterLteUePdcpThroughput::Disconnect ()
{
  if (!m_tracesConnected)
    return;

  // Best-effort: global disconnects (safe even if not connected)
  Config::Disconnect ("/NodeList/*/DeviceList/*/LteEnbRrc/NewUeContext",
                      MakeBoundCallback (&OranReporterLteUePdcpThroughput::NotifyNewUeContextEnb, this));
  Config::Disconnect ("/NodeList/*/DeviceList/*/LteUeRrc/DrbCreated",
                      MakeBoundCallback (&OranReporterLteUePdcpThroughput::CreatedDrbUe, this));
  // Note: per-UE UeManager DrbCreated connections are not tracked individually here;
  // safe to leave as-is (noop in ns-3 shutdown), or you can extend to store tokens.

  m_tracesConnected = false;
}

void
OranReporterLteUePdcpThroughput::NotifyNewUeContextEnb (OranReporterLteUePdcpThroughput* self,
                                                        std::string context,
                                                        uint16_t cellId,
                                                        uint16_t rnti)
{
  NS_LOG_DEBUG ("NewUeContext ENB ctx=" << context << " cell=" << cellId << " rnti=" << rnti);
  self->StoreUeManagerPath (context, cellId, rnti);
}

void
OranReporterLteUePdcpThroughput::StoreUeManagerPath (const std::string& enbRrcContext,
                                                     uint16_t cellId,
                                                     uint16_t rnti)
{
  // eNB path comes in as …/LteEnbRrc/NewUeContext
  const std::string ueMgrPath =
    enbRrcContext.substr (0, enbRrcContext.rfind ('/')) + "/UeMap/" + std::to_string (rnti);

  NS_LOG_LOGIC ("ueManagerPath = " << ueMgrPath);
  m_ueManagerPathByCellIdRnti[{cellId, rnti}] = ueMgrPath;
  NS_LOG_DEBUG ("StoreUeManagerPath: cell=" << cellId << " rnti=" << rnti
                << " path=" << ueMgrPath);

  // Connect future DRB creation at this UeManager
  Config::Connect (ueMgrPath + "/DrbCreated",
                   MakeBoundCallback (&OranReporterLteUePdcpThroughput::CreatedDrbEnb, this));
}

void
OranReporterLteUePdcpThroughput::CreatedDrbEnb (OranReporterLteUePdcpThroughput* self,
                                                std::string context,
                                                uint64_t /*imsi*/,
                                                uint16_t cellId,
                                                uint16_t /*rnti*/,
                                                uint8_t  lcid)
{
  // eNB side DRB: connect PDCP RxPDU for UL delivered bytes
  // context example: …/LteEnbRrc/UeMap/<rnti>/DrbCreated
  const std::string ueMgrPath = context.substr (0, context.rfind ('/')); // …/UeMap/<rnti>
  self->ConnectDrbEnb (ueMgrPath, cellId, lcid);
}

void
OranReporterLteUePdcpThroughput::CreatedDrbUe (OranReporterLteUePdcpThroughput* self,
                                               std::string context,
                                               uint64_t /*imsi*/,
                                               uint16_t cellId,
                                               uint16_t /*rnti*/,
                                               uint8_t  lcid)
{
  // UE side DRB: connect PDCP RxPDU for DL delivered bytes
  // context example: …/LteUeRrc/DrbCreated
  const std::string ueRrcPath = context.substr (0, context.rfind ('/')); // …/LteUeRrc
  self->ConnectDrbUe (ueRrcPath, cellId, lcid);
}

void
OranReporterLteUePdcpThroughput::ConnectDrbEnb (const std::string& ueMgrPath,
                                                uint16_t cellId, uint8_t lcid)
{
  // eNB DRB map index is (lcid - 2)
  std::array<std::string, 2> candidates = {
    ueMgrPath + "/DataRadioBearerMap/" + std::to_string(static_cast<unsigned>(lcid - 2)),
    ueMgrPath + "/ComponentCarrierMap/*/DataRadioBearerMap/" +
        std::to_string(static_cast<unsigned>(lcid - 2))
  };

  bool okRx = false;
  for (const auto& base : candidates)
  {
    if (Config::ConnectFailSafe (base + "/LtePdcp/RxPDU",
                                 MakeBoundCallback (&OranReporterLteUePdcpThroughput::EnbUlRxPduCb, this, cellId)))
                                 
    {
      okRx = true;
      NS_LOG_INFO ("Connected UL PDCP trace at " << base << " (cell=" << cellId
                     << " lcid=" << static_cast<unsigned>(lcid) << ")");
      break;
    }
  }
  if (!okRx)
  {
    NS_LOG_WARN ("PDCP not present for UL (cell=" << cellId
                 << " lcid=" << static_cast<unsigned>(lcid)
                 << ") under " << ueMgrPath);
  }
}

void
OranReporterLteUePdcpThroughput::ConnectDrbUe (const std::string& ueRrcPath,
                                               uint16_t cellId, uint8_t lcid)
{
  std::array<std::string, 2> candidates = {
    ueRrcPath + "/DataRadioBearerMap/" + std::to_string(static_cast<unsigned>(lcid)),
    ueRrcPath + "/ComponentCarrierMap/*/DataRadioBearerMap/" +
        std::to_string(static_cast<unsigned>(lcid))
  };

  bool okRx = false;
  for (const auto& base : candidates)
  {
    if (Config::ConnectFailSafe (base + "/LtePdcp/RxPDU",
                                 MakeBoundCallback (&OranReporterLteUePdcpThroughput::UeDlRxPduCb, this, cellId)))
    {
      okRx = true;
      NS_LOG_INFO ("Connected DL PDCP trace at " << base << " (cell=" << cellId
                     << " lcid=" << static_cast<unsigned>(lcid) << ")");
      break;
    }
  }
  if (!okRx)
  {
    NS_LOG_WARN ("PDCP not present for DL (cell=" << cellId
                 << " lcid=" << static_cast<unsigned>(lcid)
                 << ") under " << ueRrcPath);
  }

  // Note: we don’t need TxPDU here.
}

// -------- PDCP Rx callbacks (DL @ UE, UL @ eNB) --------

uint64_t OranReporterLteUePdcpThroughput::MapUeToE2Id(uint16_t cellId, uint16_t rnti) const
{
  if (!m_nearRtRic) return 0;
  Ptr<OranDataRepository> repo = m_nearRtRic->Data();
  if (!repo) return 0;
  return repo->GetLteUeE2NodeIdFromCellInfo(cellId, rnti);
}

void
OranReporterLteUePdcpThroughput::UeDlRxPduCb(OranReporterLteUePdcpThroughput* self,
                                             uint16_t cellId,
                                             std::string /*path*/,
                                             uint16_t rnti, uint8_t /*lcid*/,
                                             uint32_t packetSize, uint64_t /*delay*/)
{
  if (cellId == 0) { NS_LOG_WARN("UeDlRxPduCb: cellId=0 for rnti=" << rnti); return; }

  uint64_t ueid = self->MapUeToE2Id(cellId, rnti);
  if (ueid == 0) { NS_LOG_WARN("UeDlRxPduCb: no E2 UE mapping for cell=" << cellId << " rnti=" << rnti); return; }

  UeAgg& agg = self->m_agg[ueid];
  agg.dlBytes += packetSize;
  agg.cellId = cellId;
  NS_LOG_DEBUG ("DL PDCP rx: ueid=" << ueid << " cell=" << cellId
                << " bytes+=" << packetSize);
}

void
OranReporterLteUePdcpThroughput::EnbUlRxPduCb(OranReporterLteUePdcpThroughput* self,
                                              uint16_t cellId,
                                              std::string /*path*/,
                                              uint16_t rnti, uint8_t /*lcid*/,
                                              uint32_t packetSize, uint64_t /*delay*/)
{
  if (cellId == 0) { NS_LOG_WARN("EnbUlRxPduCb: cellId=0 for rnti=" << rnti); return; }

  uint64_t ueid = self->MapUeToE2Id(cellId, rnti);
  if (ueid == 0) { NS_LOG_WARN("EnbUlRxPduCb: no E2 UE mapping for cell=" << cellId << " rnti=" << rnti); return; }

  UeAgg& agg = self->m_agg[ueid];
  agg.ulBytes += packetSize;
  agg.cellId = cellId;
  NS_LOG_DEBUG ("UL PDCP rx: ueid=" << ueid << " cell=" << cellId
                << " bytes+=" << packetSize);
}


} // namespace ns3
