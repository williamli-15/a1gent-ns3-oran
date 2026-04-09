#include "oran-lm-command-bridge.h"
#include "oran-near-rt-ric.h"
#include "oran-data-repository.h"
#include "oran-command-lte-2-lte-handover.h"

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/pointer.h"
#include "ns3/double.h"
#include "ns3/string.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/lte-enb-phy.h"
#include "ns3/cio-store.h"

#include <cstdio>
#include <sstream>
#include <cmath>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("OranLmCommandBridge");
NS_OBJECT_ENSURE_REGISTERED(OranLmCommandBridge);

TypeId
OranLmCommandBridge::GetTypeId()
{
  static TypeId tid =
    TypeId("ns3::OranLmCommandBridge")
      .SetParent<OranLm>()
      .AddConstructor<OranLmCommandBridge>()
      .AddAttribute("CommandsPath",
        "Path to the JSON file containing commands from Python rApps.",
        StringValue("commands.json"),
        MakeStringAccessor(&OranLmCommandBridge::m_commandsPath),
        MakeStringChecker())
      .AddAttribute("ApplyCioInline",
        "If true, SET_CIO is applied directly here (no OranCommand).",
        BooleanValue(true),
        MakeBooleanAccessor(&OranLmCommandBridge::m_applyCioInline),
        MakeBooleanChecker())
      .AddAttribute("IgnoreOlderThan",
        "Ignore commands whose ts < (now - this window).",
        TimeValue(Seconds(120)),
        MakeTimeAccessor(&OranLmCommandBridge::m_ignoreOlderThan),
        MakeTimeChecker())
      .AddAttribute("SleepTxPowerDbm",
        "Tx power (dBm) to apply to an eNB when processing a cell_sleep command.",
        DoubleValue(0.0),
        MakeDoubleAccessor(&OranLmCommandBridge::m_sleepTxPowerDbm),
        MakeDoubleChecker<double>());
  return tid;
}

OranLmCommandBridge::OranLmCommandBridge()
{
  m_name = "LM_CommandBridge";
}

void
OranLmCommandBridge::DoDispose()
{
  m_originalTxPower.clear();
  m_cellSleeping.clear();
  OranLm::DoDispose();
}

Ptr<OranNearRtRic>
OranLmCommandBridge::GetRic()
{
  PointerValue pv;
  GetAttribute("NearRtRic", pv);
  return pv.Get<OranNearRtRic>();
}

static bool
GetFileMtime(const std::string& path, time_t& out)
{
  struct stat st{};
  if (::stat(path.c_str(), &st) == 0) {
    out = st.st_mtime;
    return true;
  }
  return false;
}

bool
OranLmCommandBridge::ReadJson(json& out)
{
  time_t mt{};
  if (!GetFileMtime(m_commandsPath, mt)) {
    // file not present
    NS_LOG_LOGIC("commands.json missing path=" << m_commandsPath);
    return false;
  }
  if (mt == m_lastMtime) {
    // no change
    NS_LOG_LOGIC("commands.json unchanged mtime=" << mt);
    return false;
  }

  std::ifstream f(m_commandsPath);
  if (!f.good()) {
    NS_LOG_WARN("commands.json open failed");
    return false;
  }
  try {
    out = json::parse(f);
    m_lastMtime = mt;
    NS_LOG_LOGIC("commands.json parsed mtime=" << mt);
    return true;
  } catch (const std::exception& e) {
    NS_LOG_WARN("JSON parse error: " << e.what());
    return false;
  }
}

std::vector<Ptr<OranCommand>>
OranLmCommandBridge::Run()
{
  NS_LOG_FUNCTION(this);

  std::vector<Ptr<OranCommand>> cmds;

  json doc;
  if (!ReadJson(doc)) {
    return cmds; // nothing to do
  }

  // Expected schema:
  // { "ts": <double_sec>, "commands": [ { "type":"ho", ... }, { "type":"set_cio", ... } ] }
  double ts = 0.0;
  try {
    if (doc.contains("ts")) { ts = doc["ts"].get<double>(); }
  } catch (...) {}

  const Time now = Simulator::Now();
  if (ts > 0.0) {
    Time tsec = Seconds(ts);
    if (now - tsec > m_ignoreOlderThan) {
      NS_LOG_INFO("Ignoring commands.json (too old) ts=" << ts << " now=" << now.GetSeconds());
      return cmds;
    }
  }

  if (!doc.contains("commands") || !doc["commands"].is_array()) {
    NS_LOG_INFO("commands.json -> no 'commands' array ts=" << ts);
    return cmds;
  }

  size_t numCmds = doc["commands"].size();
  NS_LOG_INFO("commands.json -> entries=" << numCmds << " ts=" << ts);
  if (numCmds == 0) {
    return cmds;
  }

  Ptr<OranNearRtRic> ric = GetRic();

  for (auto& c : doc["commands"]) {
    try {
      std::string type = c.at("type").get<std::string>();
      if (type == "ho") {
        uint64_t ueE2Id   = c.at("ue_e2id").get<uint64_t>();
        uint16_t targetCi = c.at("target_cellid").get<uint16_t>();
        if (ExecuteHo(ueE2Id, targetCi, cmds)) {
            if (ric && ric->Data()) {
                ric->Data()->LogCommandLm(m_name, cmds.back());
          }
        }
      } else if (type == "set_cio") {
        uint16_t cellId = c.at("cellid").get<uint16_t>();
        double   offDb  = c.at("offset_db").get<double>();

        uint16_t neighborCell = 0;
        bool hasNeighbor = false;
        if (c.contains("neighbor_cellid")) {
          neighborCell = c.at("neighbor_cellid").get<uint16_t>();
          hasNeighbor = true;
        }

        if (!hasNeighbor || neighborCell == 0)
        {
          NS_LOG_WARN("set_cio missing neighbor_cellid; ignoring (cell=" << cellId << ")");
          continue;
        }

        if (m_applyCioInline) {
          double appliedDb = offDb;
          bool ok = ApplyCio(cellId, neighborCell, offDb, &appliedDb);
          if (ok && ric && ric->Data()) {
            std::ostringstream oss;
            oss << "SET_CIO cell=" << cellId
                << " neighbor=" << neighborCell
                << " offset=" << appliedDb << " dB";
            ric->Data()->LogActionLm(m_name, oss.str());
          }
        }
      } else if (type == "cell_sleep") {
        uint16_t cellId = c.at("cellid").get<uint16_t>();
        std::string reason;
        bool ok = SetCellSleep(cellId, &reason);
        if (ok && ric && ric->Data()) {
          std::ostringstream oss;
          oss << "CELL_SLEEP cell=" << cellId;
          if (!reason.empty()) {
            oss << " (" << reason << ")";
          }
          ric->Data()->LogActionLm(m_name, oss.str());
        }
      } else if (type == "cell_wake") {
        uint16_t cellId = c.at("cellid").get<uint16_t>();
        std::string reason;
        bool ok = SetCellWake(cellId, &reason);
        if (ok && ric && ric->Data()) {
          std::ostringstream oss;
          oss << "CELL_WAKE cell=" << cellId;
          if (!reason.empty()) {
            oss << " (" << reason << ")";
          }
          ric->Data()->LogActionLm(m_name, oss.str());
        }
      }
    } catch (const std::exception& e) {
      NS_LOG_WARN("Bad command entry: " << e.what());
    }
  }

  return cmds;
}

bool
OranLmCommandBridge::ExecuteHo(uint64_t ueE2Id, uint16_t targetCellId,
                               std::vector<Ptr<OranCommand>>& out)
{
  NS_LOG_FUNCTION(this << ueE2Id << targetCellId);

  // Lookup current (serving) cell & rnti for UE
  uint16_t servingCell = 0;
  uint16_t rnti = 0;
  if (!FindUeCellInfo(ueE2Id, servingCell, rnti)) {
    NS_LOG_WARN("ExecuteHo: no UE cell info for ueE2Id=" << ueE2Id);
    return false;
  }

  if (targetCellId == 0) {
    NS_LOG_WARN("ExecuteHo: target cell id is zero for ue=" << ueE2Id);
    return false;
  }
  if (targetCellId == servingCell) {
    NS_LOG_INFO("ExecuteHo: target==serving (" << targetCellId << "), skip for ue=" << ueE2Id);
    return false;
  }

  // Wake a sleeping target cell if necessary before issuing the HO command.
  auto sleepIt = m_cellSleeping.find(targetCellId);
  if (sleepIt != m_cellSleeping.end() && sleepIt->second) {
    std::string reason;
    bool woke = SetCellWake(targetCellId, &reason);
    if (woke) {
      Ptr<OranNearRtRic> ric = GetRic();
      if (ric && ric->Data()) {
        std::ostringstream oss;
        oss << "AUTO_WAKE cell=" << targetCellId;
        if (!reason.empty())
        {
          oss << " (" << reason << ")";
        }
        ric->Data()->LogActionLm(m_name, oss.str());
      }
      NS_LOG_INFO("ExecuteHo: auto-woke cell=" << targetCellId << " reason=" << reason);
    } else {
      NS_LOG_WARN("ExecuteHo: failed to wake sleeping target cell=" << targetCellId);
      return false;
    }
  }

  // Find the source eNB's E2 node id by cellId
  uint64_t enbE2Id = 0;
  if (!FindEnbE2NodeIdByCell(servingCell, enbE2Id)) {
    NS_LOG_WARN("ExecuteHo: cannot map cellId->enbE2Id for cell=" << servingCell);
    return false;
  }

  Ptr<OranCommandLte2LteHandover> cmd = CreateObject<OranCommandLte2LteHandover>();
  cmd->SetAttribute("TargetE2NodeId", UintegerValue(enbE2Id));
  cmd->SetAttribute("TargetRnti",      UintegerValue(rnti));
  cmd->SetAttribute("TargetCellId",    UintegerValue(targetCellId));
  NS_LOG_INFO("ExecuteHo: ue=" << ueE2Id << " scell=" << servingCell << " -> target=" << targetCellId);

  out.push_back(cmd);
  return true;
}

bool
OranLmCommandBridge::ApplyCio(uint16_t srcCellId, uint16_t nbrCellId, double offsetDb, double* appliedDb)
{
  NS_LOG_FUNCTION(this << srcCellId << nbrCellId << offsetDb);

  if (nbrCellId == 0)
  {
    NS_LOG_WARN("ApplyCio: neighbor cell id is zero (src=" << srcCellId << ")");
    return false;
  }

  // clamp
  if (offsetDb < -24.0) offsetDb = -24.0;
  if (offsetDb >  24.0) offsetDb =  24.0;

  if (appliedDb) {
    *appliedDb = offsetDb;
  }

  CioStore::ClearSrc(srcCellId);
  CioStore::Set(srcCellId, nbrCellId, offsetDb);

  NS_LOG_INFO("ApplyCio: set CIO src=" << srcCellId
               << " -> nbr=" << nbrCellId
               << " offset=" << offsetDb << " dB");
  return true;
}

Ptr<LteHandoverAlgorithm>
OranLmCommandBridge::GetEnbHoAlgByCell(uint16_t cellId)
{
  for (auto it = NodeList::Begin(); it != NodeList::End(); ++it) {
    Ptr<Node> n = *it;
    for (uint32_t d=0; d<n->GetNDevices(); ++d) {
      Ptr<LteEnbNetDevice> enb = n->GetDevice(d)->GetObject<LteEnbNetDevice>();
      if (!enb) continue;
      if (enb->GetCellId() != cellId) continue;

      PointerValue pv;
      enb->GetAttribute("LteHandoverAlgorithm", pv);
      Ptr<LteHandoverAlgorithm> hoAlg = pv.Get<LteHandoverAlgorithm>();
      if (hoAlg) return hoAlg;
    }
  }
  return nullptr;
}

Ptr<LteEnbNetDevice>
OranLmCommandBridge::GetEnbByCell(uint16_t cellId)
{
  for (auto it = NodeList::Begin(); it != NodeList::End(); ++it) {
    Ptr<Node> n = *it;
    for (uint32_t d = 0; d < n->GetNDevices(); ++d) {
      Ptr<LteEnbNetDevice> enb = n->GetDevice(d)->GetObject<LteEnbNetDevice>();
      if (!enb) {
        continue;
      }
      if (enb->GetCellId() == cellId) {
        return enb;
      }
    }
  }
  return nullptr;
}

bool
OranLmCommandBridge::SetCellSleep(uint16_t cellId, std::string* reason)
{
  Ptr<LteEnbNetDevice> enb = GetEnbByCell(cellId);
  if (!enb) {
    if (reason) {
      *reason = "no enb for cell";
    }
    NS_LOG_WARN("SetCellSleep: no eNB device for cellId=" << cellId);
    return false;
  }

  Ptr<LteEnbPhy> phy = enb->GetPhy();
  if (!phy) {
    if (reason) {
      *reason = "no phy";
    }
    NS_LOG_WARN("SetCellSleep: no PHY for cellId=" << cellId);
    return false;
  }

  DoubleValue txPower;
  phy->GetAttribute("TxPower", txPower);
  double current = txPower.Get();

  auto it = m_originalTxPower.find(cellId);
  if (it == m_originalTxPower.end()) {
    m_originalTxPower[cellId] = current;
  }

  if (current <= m_sleepTxPowerDbm + 1e-3) {
    // Already at or below sleep power
    m_cellSleeping[cellId] = true;
    if (reason) {
      *reason = "already sleeping";
    }
    return false;
  }

  phy->SetAttribute("TxPower", DoubleValue(m_sleepTxPowerDbm));
  m_cellSleeping[cellId] = true;
  if (reason) {
    std::ostringstream oss;
    oss << "tx=" << current << "->" << m_sleepTxPowerDbm << " dBm";
    *reason = oss.str();
  }
  NS_LOG_INFO("SetCellSleep: cell=" << cellId << " TxPower " << current << "->" << m_sleepTxPowerDbm << " dBm");
  return true;
}

bool
OranLmCommandBridge::SetCellWake(uint16_t cellId, std::string* reason)
{
  Ptr<LteEnbNetDevice> enb = GetEnbByCell(cellId);
  if (!enb) {
    if (reason) {
      *reason = "no enb for cell";
    }
    NS_LOG_WARN("SetCellWake: no eNB device for cellId=" << cellId);
    return false;
  }

  Ptr<LteEnbPhy> phy = enb->GetPhy();
  if (!phy) {
    if (reason) {
      *reason = "no phy";
    }
    NS_LOG_WARN("SetCellWake: no PHY for cellId=" << cellId);
    return false;
  }

  double restore = m_sleepTxPowerDbm;
  auto it = m_originalTxPower.find(cellId);
  if (it != m_originalTxPower.end()) {
    restore = it->second;
  } else {
    DoubleValue txPower;
    phy->GetAttribute("TxPower", txPower);
    restore = txPower.Get();
  }

  DoubleValue currentVal;
  phy->GetAttribute("TxPower", currentVal);
  double current = currentVal.Get();

  if (std::abs(current - restore) < 1e-3) {
    m_cellSleeping[cellId] = false;
    if (reason) {
      *reason = "already awake";
    }
    return false;
  }

  phy->SetAttribute("TxPower", DoubleValue(restore));
  m_cellSleeping[cellId] = false;
  if (reason) {
    std::ostringstream oss;
    oss << "tx=" << current << "->" << restore << " dBm";
    *reason = oss.str();
  }
  NS_LOG_INFO("SetCellWake: cell=" << cellId << " TxPower " << current << "->" << restore << " dBm");
  return true;
}

bool
OranLmCommandBridge::FindUeCellInfo(uint64_t ueE2Id, uint16_t& cellId, uint16_t& rnti)
{
  auto ric = GetRic();
  if (!ric || !ric->Data()) return false;
  auto t = ric->Data()->GetLteUeCellInfo(ueE2Id);
  bool ok; std::tie(ok, cellId, rnti) = t;
  return ok;
}

bool
OranLmCommandBridge::FindEnbE2NodeIdByCell(uint16_t cellId, uint64_t& enbE2Id)
{
  enbE2Id = 0;
  auto ric = GetRic();
  if (!ric || !ric->Data()) return false;
  std::vector<uint64_t> v = ric->Data()->GetLteEnbE2NodeIds();
  for (auto id : v) {
    auto ci = ric->Data()->GetLteEnbCellInfo(id);
    bool ok; uint16_t c{};
    std::tie(ok, c) = ci;
    if (ok && c == cellId) { enbE2Id = id; return true; }
  }
  return false;
}

} // namespace ns3
