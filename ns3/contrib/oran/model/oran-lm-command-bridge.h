#ifndef ORAN_LM_COMMAND_BRIDGE_H
#define ORAN_LM_COMMAND_BRIDGE_H

#include "oran-lm.h"
#include "ns3/nstime.h"
#include "ns3/node-container.h"
#include "ns3/node-list.h"
#include "ns3/lte-enb-net-device.h"
#include "ns3/lte-handover-algorithm.h"
#include "ns3/uinteger.h"
#include "json.hpp"

#include <fstream>
#include <sys/stat.h>
#include <map>

namespace ns3 {

/**
 * Logic Module that reads a JSON file (commands.json by default) and applies actions:
 *  - HO: emit an OranCommandLte2LteHandover (E2-style control) to move a UE to a target cell.
 *  - SET_CIO: update a per-neighbour Cell Individual Offset (CIO) in CioStore (O1/slow-control style),
 *             which is consumed by UE RRC measurement event evaluation (A3/A4/A5) via Ocn.
 *  - CELL_SLEEP / CELL_WAKE: emulate energy saving by reducing/restoring eNB TxPower.
 *
 * Note: CIO here is a true per-(src,dst) bias used in UE-side event triggering, not a per-cell HO hysteresis knob.
 */

class OranLmCommandBridge : public OranLm
{
public:
  static TypeId GetTypeId();
  OranLmCommandBridge();
  ~OranLmCommandBridge() override = default;

  std::vector<Ptr<OranCommand>> Run() override;

protected:
  void DoDispose() override;

private:
  using json = nlohmann::json;

  // attrs
  std::string m_commandsPath;   // e.g., "commands.json"
  bool        m_applyCioInline; // true = apply CIO immediately (no OranCommand)
  Time        m_ignoreOlderThan; // ignore commands ts older than now - window
  double      m_sleepTxPowerDbm; // Tx power to apply when sleeping a cell

  // file-change detection
  time_t      m_lastMtime = 0;

  // helpers
  Ptr<OranNearRtRic> GetRic();
  bool ReadJson(json& out);
  bool ExecuteHo(uint64_t ueE2Id, uint16_t targetCellId, std::vector<Ptr<OranCommand>>& out);
  bool ApplyCio(uint16_t srcCellId, uint16_t nbrCellId, double offsetDb, double* appliedDb);
  Ptr<LteHandoverAlgorithm> GetEnbHoAlgByCell(uint16_t cellId);
  Ptr<LteEnbNetDevice> GetEnbByCell(uint16_t cellId);
  bool SetCellSleep(uint16_t cellId, std::string* reason);
  bool SetCellWake(uint16_t cellId, std::string* reason);
  bool FindUeCellInfo(uint64_t ueE2Id, uint16_t& cellId, uint16_t& rnti);
  bool FindEnbE2NodeIdByCell(uint16_t cellId, uint64_t& enbE2Id);

  std::map<uint16_t, double> m_originalTxPower;
  std::map<uint16_t, bool> m_cellSleeping;
};

} // namespace ns3

#endif // ORAN_LM_COMMAND_BRIDGE_H
