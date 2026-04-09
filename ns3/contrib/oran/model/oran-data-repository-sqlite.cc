/**
 * NIST-developed software is provided by NIST as a public service. You may
 * use, copy and distribute copies of the software in any medium, provided that
 * you keep intact this entire notice. You may improve, modify and create
 * derivative works of the software or any portion of the software, and you may
 * copy and distribute such modifications or works. Modified works should carry
 * a notice stating that you changed the software and should note the date and
 * nature of any such change. Please explicitly acknowledge the National
 * Institute of Standards and Technology as the source of the software.
 *
 * NIST-developed software is expressly provided "AS IS." NIST MAKES NO
 * WARRANTY OF ANY KIND, EXPRESS, IMPLIED, IN FACT OR ARISING BY OPERATION OF
 * LAW, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTY OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, NON-INFRINGEMENT AND DATA ACCURACY. NIST
 * NEITHER REPRESENTS NOR WARRANTS THAT THE OPERATION OF THE SOFTWARE WILL BE
 * UNINTERRUPTED OR ERROR-FREE, OR THAT ANY DEFECTS WILL BE CORRECTED. NIST
 * DOES NOT WARRANT OR MAKE ANY REPRESENTATIONS REGARDING THE USE OF THE
 * SOFTWARE OR THE RESULTS THEREOF, INCLUDING BUT NOT LIMITED TO THE
 * CORRECTNESS, ACCURACY, RELIABILITY, OR USEFULNESS OF THE SOFTWARE.
 *
 * You are solely responsible for determining the appropriateness of using and
 * distributing the software and you assume all risks associated with its use,
 * including but not limited to the risks and costs of program errors,
 * compliance with applicable laws, damage to or loss of data, programs or
 * equipment, and the unavailability or interruption of operation. This
 * software is not intended to be used in any situation where a failure could
 * cause risk of injury or damage to property. The software developed by NIST
 * employees is not subject to copyright protection within the United States.
 */

#include "oran-data-repository-sqlite.h"

#include "ns3/abort.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/string.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("OranDataRepositorySqlite");

NS_OBJECT_ENSURE_REGISTERED(OranDataRepositorySqlite);

TypeId
OranDataRepositorySqlite::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::OranDataRepositorySqlite")
            .SetParent<OranDataRepository>()
            .AddConstructor<OranDataRepositorySqlite>()
            .AddAttribute("DatabaseFile",
                          "The database file path.",
                          StringValue("oran-repository.db"),
                          MakeStringAccessor(&OranDataRepositorySqlite::m_dbPath),
                          MakeStringChecker())
            .AddTraceSource("QueryRc",
                            "Return code for SQL queries",
                            MakeTraceSourceAccessor(&OranDataRepositorySqlite::m_queryRc),
                            "ns3::OranDataRepositorySqlite::QueryTracedCallback")

        ;

    return tid;
}

OranDataRepositorySqlite::OranDataRepositorySqlite()
    : OranDataRepository(),
      m_db(nullptr)
{
    NS_LOG_FUNCTION(this);

    InitStatements();
}

OranDataRepositorySqlite::~OranDataRepositorySqlite()
{
    NS_LOG_FUNCTION(this);
}

void
OranDataRepositorySqlite::Activate()
{
    NS_LOG_FUNCTION(this);

    OranDataRepository::Activate();

    if (!IsDbOpen())
    {
        OpenDb();
    }
}

void
OranDataRepositorySqlite::Deactivate()
{
    NS_LOG_FUNCTION(this);

    if (IsDbOpen())
    {
        CloseDb();
    }

    OranDataRepository::Deactivate();
}

bool
OranDataRepositorySqlite::IsNodeRegistered(uint64_t e2NodeId)
{
    NS_LOG_FUNCTION(this);

    bool registered = false;
    if (m_active)
    {
        int rc;
        sqlite3_stmt* stmt = nullptr;

        sqlite3_prepare_v2(m_db, m_queryStmtsStrings[CHECK_NODE_REGISTERED].c_str(), -1, &stmt, 0);
        sqlite3_bind_int64(stmt, 1, e2NodeId);

        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        {
            registered = sqlite3_column_int(stmt, 0);
        }

        CheckQueryReturnCode(stmt, rc, FormatBoundArgsList(e2NodeId));
        sqlite3_finalize(stmt);
    }
    return registered;
}

uint64_t
OranDataRepositorySqlite::RegisterNode(OranNearRtRic::NodeType type, uint64_t id)
{
    NS_LOG_FUNCTION(this);

    uint64_t e2NodeId = 0;

    if (m_active)
    {
        int rc;
        sqlite3_stmt* stmt = nullptr;

        if (id == 0)
        {
            // Insert or update the node information
            sqlite3_prepare_v2(m_db, m_queryStmtsStrings[INSERT_NODE_ADD].c_str(), -1, &stmt, 0);

            sqlite3_bind_int(stmt, 1, type);

            rc = sqlite3_step(stmt);

            CheckQueryReturnCode(stmt, rc, FormatBoundArgsList(type));

            e2NodeId = sqlite3_last_insert_rowid(m_db);
        }
        else
        {
            sqlite3_prepare_v2(m_db, m_queryStmtsStrings[INSERT_NODE_UPDATE].c_str(), -1, &stmt, 0);

            sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(id));
            sqlite3_bind_int  (stmt, 2, static_cast<int>(type));

            rc = sqlite3_step(stmt);

            CheckQueryReturnCode(stmt, rc, FormatBoundArgsList(id, type));

            e2NodeId = sqlite3_last_insert_rowid(m_db);
        }

        sqlite3_finalize(stmt);

        // Insert the registration information
        sqlite3_prepare_v2(m_db,
                           m_queryStmtsStrings[INSERT_NODE_REGISTRATION].c_str(),
                           -1,
                           &stmt,
                           0);

        sqlite3_bind_int64(stmt, 1, e2NodeId);
        sqlite3_bind_int(stmt, 2, 1);
        sqlite3_bind_int64(stmt, 3, Simulator::Now().GetTimeStep());

        rc = sqlite3_step(stmt);

        CheckQueryReturnCode(stmt,
                             rc,
                             FormatBoundArgsList(e2NodeId, true, Simulator::Now().GetTimeStep()));

        sqlite3_finalize(stmt);
    }

    return e2NodeId;
}

uint64_t
OranDataRepositorySqlite::RegisterNodeLteUe(uint64_t id, uint64_t imsi)
{
    NS_LOG_FUNCTION(this);
    uint64_t e2NodeId = 0;

    if (m_active)
    {
        int rc;
        sqlite3_stmt* stmt = nullptr;
        e2NodeId = RegisterNode(OranNearRtRic::NodeType::LTEUE, id);

        sqlite3_prepare_v2(m_db, m_queryStmtsStrings[INSERT_LTE_UE_NODE].c_str(), -1, &stmt, 0);

        NS_ABORT_MSG_IF(e2NodeId == 0, "RegisterNodeLteUe: RegisterNode returned e2NodeId==0 (unexpected).");
        sqlite3_bind_int64(stmt, 1, e2NodeId);
        sqlite3_bind_int64(stmt, 2, imsi);

        rc = sqlite3_step(stmt);
        CheckQueryReturnCode(stmt, rc, FormatBoundArgsList(e2NodeId, imsi));
        sqlite3_finalize(stmt);
    }
    return e2NodeId;
}

uint64_t
OranDataRepositorySqlite::RegisterNodeLteEnb(uint64_t id, uint16_t cellId)
{
    NS_LOG_FUNCTION(this << id << cellId);

    uint64_t e2NodeId = 0;

    if (m_active)
    {
        int rc;
        sqlite3_stmt* stmt = nullptr;
        e2NodeId = RegisterNode(OranNearRtRic::NodeType::LTEENB, id);

        sqlite3_prepare_v2(m_db, m_queryStmtsStrings[INSERT_LTE_ENB_NODE].c_str(), -1, &stmt, 0);

        NS_ABORT_MSG_IF(e2NodeId == 0, "RegisterNodeLteEnb: RegisterNode returned e2NodeId==0 (unexpected).");
        sqlite3_bind_int64(stmt, 1, e2NodeId);
        sqlite3_bind_int(stmt, 2, cellId);

        rc = sqlite3_step(stmt);
        CheckQueryReturnCode(stmt, rc, FormatBoundArgsList(e2NodeId, cellId));
        sqlite3_finalize(stmt);
    }
    return e2NodeId;
}

uint64_t
OranDataRepositorySqlite::DeregisterNode(uint64_t e2NodeId)
{
    NS_LOG_FUNCTION(this << e2NodeId);

    uint64_t retVal = 0;
    if (m_active)
    {
        int rc;
        sqlite3_stmt* stmt = nullptr;

        retVal = e2NodeId;

        sqlite3_prepare_v2(m_db,
                           m_queryStmtsStrings[INSERT_NODE_REGISTRATION].c_str(),
                           -1,
                           &stmt,
                           0);

        sqlite3_bind_int64(stmt, 1, e2NodeId);
        sqlite3_bind_int(stmt, 2, false);
        sqlite3_bind_int64(stmt, 3, Simulator::Now().GetTimeStep());

        rc = sqlite3_step(stmt);
        CheckQueryReturnCode(stmt,
                             rc,
                             FormatBoundArgsList(e2NodeId, false, Simulator::Now().GetTimeStep()));
        sqlite3_finalize(stmt);
    }
    return retVal;
}

void
OranDataRepositorySqlite::SavePosition(uint64_t e2NodeId, Vector pos, Time t)
{
    NS_LOG_FUNCTION(this << e2NodeId << pos << t);

    if (m_active)
    {
        if (IsNodeRegistered(e2NodeId))
        {
            int rc;
            sqlite3_stmt* stmt = nullptr;

            sqlite3_prepare_v2(m_db,
                               m_queryStmtsStrings[INSERT_NODE_LOCATION].c_str(),
                               -1,
                               &stmt,
                               0);

            sqlite3_bind_int64(stmt, 1, e2NodeId);
            sqlite3_bind_double(stmt, 2, pos.x);
            sqlite3_bind_double(stmt, 3, pos.y);
            sqlite3_bind_double(stmt, 4, pos.z);
            sqlite3_bind_int64(stmt, 5, t.GetTimeStep());

            rc = sqlite3_step(stmt);
            CheckQueryReturnCode(
                stmt,
                rc,
                FormatBoundArgsList(e2NodeId, pos.x, pos.y, pos.z, t.GetTimeStep()));
            sqlite3_finalize(stmt);
        }
    }
}

void
OranDataRepositorySqlite::SaveLteUeCellInfo(uint64_t e2NodeId,
                                            uint16_t cellId,
                                            uint16_t rnti,
                                            Time t)
{
    NS_LOG_FUNCTION(this << e2NodeId << (uint32_t)cellId << (uint32_t)rnti << t);

    if (m_active)
    {
        if (IsNodeRegistered(e2NodeId))
        {
            int rc;
            sqlite3_stmt* stmt = nullptr;

            sqlite3_prepare_v2(m_db, m_queryStmtsStrings[INSERT_LTE_UE_CELL].c_str(), -1, &stmt, 0);

            sqlite3_bind_int64(stmt, 1, e2NodeId);
            sqlite3_bind_int(stmt, 2, cellId);
            sqlite3_bind_int(stmt, 3, rnti);
            sqlite3_bind_int64(stmt, 4, t.GetTimeStep());

            rc = sqlite3_step(stmt);
            CheckQueryReturnCode(stmt,
                                 rc,
                                 FormatBoundArgsList(e2NodeId, cellId, rnti, t.GetTimeStep()));
            sqlite3_finalize(stmt);
        }
    }
}

void
OranDataRepositorySqlite::SaveAppLoss(uint64_t e2NodeId, double appLoss, Time t)
{
    NS_LOG_FUNCTION(this << e2NodeId << appLoss << t);

    if (m_active)
    {
        if (IsNodeRegistered(e2NodeId))
        {
            int rc;
            std::string query;
            sqlite3_stmt* stmt = nullptr;

            query = "INSERT INTO nodeapploss (nodeid, loss, simulationtime)"
                    " VALUES (?, ?, ?)"
                    ";";

            sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);

            sqlite3_bind_int64(stmt, 1, e2NodeId);
            sqlite3_bind_double(stmt, 2, appLoss);
            sqlite3_bind_int64(stmt, 3, t.GetTimeStep());

            rc = sqlite3_step(stmt);

            CheckQueryReturnCode(stmt, rc, FormatBoundArgsList(e2NodeId, appLoss, t.GetTimeStep()));
            sqlite3_finalize(stmt);
        }
    }
}

void
OranDataRepositorySqlite::SaveLteUeRsrpRsrq(uint64_t e2NodeId,
                                            Time t,
                                            uint16_t rnti,
                                            uint16_t cellId,
                                            double rsrp,
                                            double rsrq,
                                            bool isServing,
                                            uint8_t componentCarrierId)
{
    NS_LOG_FUNCTION(this << e2NodeId << t << +rnti << +cellId << rsrp << rsrq << isServing
                         << +componentCarrierId);

    if (m_active)
    {
        if (IsNodeRegistered(e2NodeId))
        {
            int rc;
            std::string query;
            sqlite3_stmt* stmt = nullptr;

            sqlite3_prepare_v2(m_db,
                               m_queryStmtsStrings[INSERT_LTE_UE_RSRP_RSRQ].c_str(),
                               -1,
                               &stmt,
                               0);

            sqlite3_bind_int64(stmt, 1, e2NodeId);
            sqlite3_bind_int64(stmt, 2, t.GetTimeStep());
            sqlite3_bind_int(stmt, 3, rnti);
            sqlite3_bind_int(stmt, 4, cellId);
            sqlite3_bind_double(stmt, 5, rsrp);
            sqlite3_bind_double(stmt, 6, rsrq);
            sqlite3_bind_int(stmt, 7, isServing);
            sqlite3_bind_int(stmt, 8, componentCarrierId);

            rc = sqlite3_step(stmt);

            CheckQueryReturnCode(stmt,
                                 rc,
                                 FormatBoundArgsList(e2NodeId,
                                                     t.GetTimeStep(),
                                                     rnti,
                                                     cellId,
                                                     rsrp,
                                                     rsrq,
                                                     isServing,
                                                     componentCarrierId));
            sqlite3_finalize(stmt);
        }
    }
}

void
OranDataRepositorySqlite::SaveLteEnbPrbUtilization(uint64_t e2NodeId,
                                                   uint16_t cellId,
                                                   double dlUtil,
                                                   double ulUtil,
                                                   uint32_t windowMs,
                                                   uint32_t ttisObserved,
                                                   Time t)
{
  NS_LOG_FUNCTION(this << e2NodeId << (uint32_t)cellId
                 << dlUtil << ulUtil << windowMs << ttisObserved << t);

  if (!m_active || !IsNodeRegistered(e2NodeId)) return;

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(m_db,
      m_queryStmtsStrings[INSERT_LTE_ENB_PRB_UTIL].c_str(),
      -1, &stmt, 0);

  sqlite3_bind_int64 (stmt, 1, static_cast<sqlite3_int64>(e2NodeId));
  sqlite3_bind_int   (stmt, 2, static_cast<int>(cellId));
  sqlite3_bind_double(stmt, 3, dlUtil);
  sqlite3_bind_double(stmt, 4, ulUtil);
  sqlite3_bind_int   (stmt, 5, static_cast<int>(windowMs));
  sqlite3_bind_int   (stmt, 6, static_cast<int>(ttisObserved));
  sqlite3_bind_int64 (stmt, 7, t.GetTimeStep());                 // simulationtime

  int rc = sqlite3_step(stmt);
  CheckQueryReturnCode(stmt, rc,
      FormatBoundArgsList(e2NodeId, cellId, dlUtil, ulUtil,
                          windowMs, ttisObserved, t.GetTimeStep()));
  sqlite3_finalize(stmt);
}

std::tuple<bool, double, double>
OranDataRepositorySqlite::GetAvgLteEnbPrbUtilization(uint64_t e2NodeId,
                                                     Time tStart, Time tEnd)
{
    NS_LOG_FUNCTION(this << e2NodeId << tStart << tEnd);

    bool have = false;
    double avgDl = 0.0, avgUl = 0.0;

    if (m_active)
    {
        if (IsNodeRegistered(e2NodeId))
        {
            int rc;
            sqlite3_stmt* stmt = nullptr;

            sqlite3_prepare_v2(m_db,
                               m_queryStmtsStrings[GET_AVG_LTE_ENB_PRB_UTIL].c_str(),
                               -1,
                               &stmt,
                               0);

            sqlite3_bind_int64(stmt, 1, e2NodeId);
            sqlite3_bind_int64(stmt, 2, tStart.GetTimeStep());
            sqlite3_bind_int64(stmt, 3, tEnd.GetTimeStep());

            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
            {
                if (sqlite3_column_type(stmt, 0) != SQLITE_NULL &&
                    sqlite3_column_type(stmt, 1) != SQLITE_NULL)
                {
                    avgDl = sqlite3_column_double(stmt, 0);
                    avgUl = sqlite3_column_double(stmt, 1);
                    have = true;
                }
            }

            CheckQueryReturnCode(stmt,
                                 rc,
                                 FormatBoundArgsList(e2NodeId, tStart.GetTimeStep(), tEnd.GetTimeStep()));
            sqlite3_finalize(stmt);
        }
    }

    return std::make_tuple(have, avgDl, avgUl);
}

void
OranDataRepositorySqlite::SaveLteUeSinr(uint64_t e2NodeId, uint16_t cellId, uint16_t rnti,
                                        double sinrLinear, double sinrDb,
                                        double rsrpMw, double rsrpDbm, Time t)
{
    if (!m_active || !IsNodeRegistered(e2NodeId)) return;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db,
        "INSERT INTO lteuesinr (simulationtime,nodeid,cellid,rnti,sinr_linear,sinr_db,rsrp_mw,rsrp_dbm) "
        "VALUES (?,?,?,?,?,?,?,?);",
        -1, &stmt, 0);

    sqlite3_bind_int64 (stmt, 1, t.GetTimeStep());
    sqlite3_bind_int64 (stmt, 2, static_cast<sqlite3_int64>(e2NodeId));
    sqlite3_bind_int   (stmt, 3, static_cast<int>(cellId));
    sqlite3_bind_int   (stmt, 4, static_cast<int>(rnti));
    sqlite3_bind_double(stmt, 5, sinrLinear);
    sqlite3_bind_double(stmt, 6, sinrDb);
    sqlite3_bind_double(stmt, 7, rsrpMw);
    sqlite3_bind_double(stmt, 8, rsrpDbm);

    const int rc = sqlite3_step(stmt);
    CheckQueryReturnCode(stmt, rc, std::string{});
    sqlite3_finalize(stmt);
}

std::tuple<bool,double,double>
OranDataRepositorySqlite::GetAvgLteUeSinr(uint64_t e2NodeId, Time tStart, Time tEnd)
{
    if (!m_active || !IsNodeRegistered(e2NodeId))
        return std::make_tuple(false, 0.0, 0.0);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db,
        "SELECT AVG(sinr_linear), AVG(rsrp_mw) "
        "FROM lteuesinr "
        "WHERE nodeid = ? AND simulationtime >= ? AND simulationtime <= ?;",
        -1, &stmt, 0);

    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(e2NodeId));
    sqlite3_bind_int64(stmt, 2, tStart.GetTimeStep());
    sqlite3_bind_int64(stmt, 3, tEnd.GetTimeStep());

    bool   have  = false;
    double avgLin = 0.0, avgMw = 0.0;

    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL &&
            sqlite3_column_type(stmt, 1) != SQLITE_NULL)
        {
            avgLin = sqlite3_column_double(stmt, 0);
            avgMw  = sqlite3_column_double(stmt, 1);
            have   = true;
        }
    }
    CheckQueryReturnCode(stmt, rc, std::string{});
    sqlite3_finalize(stmt);

    // Convert linear averages to dB/dBm (1 mW = 0 dBm)
    double avgDb  = (avgLin > 0.0) ? 10.0 * std::log10(avgLin)
                                   : -std::numeric_limits<double>::infinity();
    double avgDbm = (avgMw  > 0.0) ? 10.0 * std::log10(avgMw)
                                   : -std::numeric_limits<double>::infinity();

    return std::make_tuple(have, avgDb, avgDbm);
}

void OranDataRepositorySqlite::SaveLteEnbScheduledThroughput(
    uint64_t e2NodeId,
    uint16_t cellId,
    double dlMbps,
    double ulMbps,
    uint32_t windowMs,
    uint32_t ttisObserved,
    Time t)
{
  NS_LOG_FUNCTION(this << e2NodeId << (uint32_t)cellId << dlMbps << ulMbps
                       << windowMs << ttisObserved << t);
  if (!m_active) { return; }
  if (!IsNodeRegistered(e2NodeId)) { return; }

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(m_db, m_queryStmtsStrings[INSERT_LTE_ENB_SCHED_TP].c_str(), -1, &stmt, 0);
  sqlite3_bind_int64(stmt, 1, e2NodeId);
  sqlite3_bind_int  (stmt, 2, cellId);
  sqlite3_bind_double(stmt, 3, dlMbps);
  sqlite3_bind_double(stmt, 4, ulMbps);
  sqlite3_bind_int  (stmt, 5, static_cast<int>(windowMs));
  sqlite3_bind_int  (stmt, 6, static_cast<int>(ttisObserved));
  sqlite3_bind_int64(stmt, 7, t.GetTimeStep());
  int rc = sqlite3_step(stmt);
  CheckQueryReturnCode(stmt, rc, FormatBoundArgsList(e2NodeId, cellId, dlMbps, ulMbps,
                                                     windowMs, ttisObserved, t.GetTimeStep()));
  sqlite3_finalize(stmt);
}

void
OranDataRepositorySqlite::SaveLteUePdcpThroughput (uint64_t ueid,
                                                   uint16_t cellId,
                                                   double dlMbps,
                                                   double ulMbps,
                                                   Time t)
{
  NS_LOG_FUNCTION (this << ueid << (uint32_t)cellId << dlMbps << ulMbps << t);
  if (!m_active) return;

  if (IsNodeRegistered (ueid))
  {
    int rc;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2 (m_db,
                        m_queryStmtsStrings[INSERT_LTE_UE_PDCP_TP].c_str (),
                        -1, &stmt, 0);

    sqlite3_bind_int64 (stmt, 1, (sqlite3_int64)ueid);
    sqlite3_bind_int   (stmt, 2,  cellId);
    sqlite3_bind_double(stmt, 3,  dlMbps);
    sqlite3_bind_double(stmt, 4,  ulMbps);
    sqlite3_bind_int64 (stmt, 5,  t.GetTimeStep ());

    rc = sqlite3_step (stmt);
    CheckQueryReturnCode (stmt, rc,
      FormatBoundArgsList (ueid, cellId, dlMbps, ulMbps, t.GetTimeStep ()));
    sqlite3_finalize (stmt);
  }
}

void
OranDataRepositorySqlite::SaveLteEnbMcs(uint64_t e2nodeid,
                                        uint16_t cellId,
                                        double dlMean, double dlP50, double dlP95,
                                        double ulMean, double ulP50, double ulP95,
                                        Time t)
{
  NS_LOG_FUNCTION(this << e2nodeid << (uint32_t)cellId
                       << dlMean << dlP50 << dlP95
                       << ulMean << ulP50 << ulP95
                       << t);

  if (!m_active) return;

  int rc;
  sqlite3_stmt* stmt = nullptr;

  sqlite3_prepare_v2(m_db,
                     m_queryStmtsStrings[INSERT_LTE_ENB_MCS].c_str(),
                     -1, &stmt, 0);

  sqlite3_bind_int64 (stmt, 1, (sqlite3_int64)e2nodeid);
  sqlite3_bind_int   (stmt, 2,  cellId);
  sqlite3_bind_double(stmt, 3,  dlMean);
  sqlite3_bind_double(stmt, 4,  dlP50);
  sqlite3_bind_double(stmt, 5,  dlP95);
  sqlite3_bind_double(stmt, 6,  ulMean);
  sqlite3_bind_double(stmt, 7,  ulP50);
  sqlite3_bind_double(stmt, 8,  ulP95);
  sqlite3_bind_int64 (stmt, 9,  t.GetTimeStep());

  rc = sqlite3_step(stmt);
  CheckQueryReturnCode(stmt, rc,
    FormatBoundArgsList(e2nodeid, cellId, dlMean, dlP50, dlP95,
                        ulMean, ulP50, ulP95, t.GetTimeStep()));
  sqlite3_finalize(stmt);
}

void
OranDataRepositorySqlite::SaveLteUePrb(uint64_t ueid, uint16_t cellId,
                                       uint64_t dlPrbs, uint64_t ulPrbs, Time t)
{
  NS_LOG_FUNCTION(this << ueid << (uint32_t)cellId << dlPrbs << ulPrbs << t);
  if (!m_active) return;
  if (!IsNodeRegistered(ueid)) return;

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(m_db,
    m_queryStmtsStrings[INSERT_LTE_UE_PRB].c_str(), -1, &stmt, 0);

  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)ueid);
  sqlite3_bind_int  (stmt, 2, (int)cellId);
  sqlite3_bind_int64(stmt, 3, (sqlite3_int64)dlPrbs);
  sqlite3_bind_int64(stmt, 4, (sqlite3_int64)ulPrbs);
  sqlite3_bind_int64(stmt, 5, (sqlite3_int64)t.GetTimeStep());

  int rc = sqlite3_step(stmt);
  CheckQueryReturnCode(stmt, rc,
    FormatBoundArgsList(ueid, cellId, dlPrbs, ulPrbs, t.GetTimeStep()));
  sqlite3_finalize(stmt);
}

void
OranDataRepositorySqlite::SaveLteEnbUlInterference(uint64_t e2nodeid,
                                                   uint16_t cellId,
                                                   double p95Mw,
                                                   double p95Dbm,
                                                   Time t)
{
  if (!m_active) return;

  int rc;
  sqlite3_stmt* stmt = nullptr;

  sqlite3_prepare_v2(m_db,
    m_queryStmtsStrings[INSERT_LTE_ENB_UL_INTERF].c_str(), -1, &stmt, 0);

  sqlite3_bind_int64 (stmt, 1, static_cast<sqlite3_int64>(e2nodeid));
  sqlite3_bind_int   (stmt, 2, static_cast<int>(cellId));
  sqlite3_bind_double(stmt, 3, p95Mw);
  sqlite3_bind_double(stmt, 4, p95Dbm);
  sqlite3_bind_int64 (stmt, 5, static_cast<sqlite3_int64>(t.GetTimeStep()));

  rc = sqlite3_step(stmt);
  CheckQueryReturnCode(stmt, rc,
    FormatBoundArgsList(e2nodeid, cellId, p95Mw, p95Dbm, t.GetTimeStep()));
  sqlite3_finalize(stmt);
}

void OranDataRepositorySqlite::SaveLteHoEvent(uint64_t imsi, uint64_t ueid,
                                              uint16_t srcCell, uint16_t dstCell,
                                              const std::string& event, Time t)
{
  if (!m_active) return;
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(m_db,
    m_queryStmtsStrings[INSERT_LTE_HO_EVENT].c_str(), -1, &stmt, 0);

  sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(imsi));
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(ueid));
  sqlite3_bind_int  (stmt, 3, static_cast<int>(srcCell));
  sqlite3_bind_int  (stmt, 4, static_cast<int>(dstCell));
  sqlite3_bind_text (stmt, 5, event.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(t.GetTimeStep()));

  int rc = sqlite3_step(stmt);
  CheckQueryReturnCode(stmt, rc, FormatBoundArgsList(imsi, ueid, srcCell, dstCell, event, t.GetTimeStep()));
  sqlite3_finalize(stmt);
}

void OranDataRepositorySqlite::SaveLteUeDwell(uint64_t ueid, uint16_t cellId,
                                              double dwellSeconds, Time t)
{
  if (!m_active) return;
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(m_db,
    m_queryStmtsStrings[INSERT_LTE_UE_DWELL].c_str(), -1, &stmt, 0);

  sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(ueid));
  sqlite3_bind_int  (stmt, 2, static_cast<int>(cellId));
  sqlite3_bind_double(stmt, 3, dwellSeconds);
  sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(t.GetTimeStep()));

  int rc = sqlite3_step(stmt);
  CheckQueryReturnCode(stmt, rc, FormatBoundArgsList(ueid, cellId, dwellSeconds, t.GetTimeStep()));
  sqlite3_finalize(stmt);
}

void OranDataRepositorySqlite::SaveLteEnbUeCount(uint64_t e2nodeid,
                                                 uint16_t cellId,
                                                 uint32_t ueCount,
                                                 Time t)
{
  if (!m_active) return;
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(m_db,
    m_queryStmtsStrings[INSERT_LTE_ENB_UECOUNT].c_str(), -1, &stmt, 0);

  sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(e2nodeid));
  sqlite3_bind_int  (stmt, 2, static_cast<int>(cellId));
  sqlite3_bind_int  (stmt, 3, static_cast<int>(ueCount));
  sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(t.GetTimeStep()));

  int rc = sqlite3_step(stmt);
  CheckQueryReturnCode(stmt, rc, FormatBoundArgsList(e2nodeid, cellId, ueCount, t.GetTimeStep()));
  sqlite3_finalize(stmt);
}

std::map<Time, Vector>
OranDataRepositorySqlite::GetNodePositions(uint64_t e2NodeId,
                                           Time fromTime,
                                           Time toTime,
                                           uint64_t maxEntries)
{
    NS_LOG_FUNCTION(this << e2NodeId << fromTime << toTime << maxEntries);

    std::map<Time, Vector> nodePositions;

    if (m_active)
    {
        if (IsNodeRegistered(e2NodeId))
        {
            int rc;
            sqlite3_stmt* stmt = nullptr;

            sqlite3_prepare_v2(m_db,
                               m_queryStmtsStrings[GET_NODE_ALL_POSITIONS].c_str(),
                               -1,
                               &stmt,
                               0);

            sqlite3_bind_int64(stmt, 1, e2NodeId);
            sqlite3_bind_int64(stmt, 2, fromTime.GetTimeStep());
            sqlite3_bind_int64(stmt, 3, toTime.GetTimeStep());
            sqlite3_bind_int64(stmt, 4, maxEntries);

            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
            {
                uint64_t timeStep = sqlite3_column_int64(stmt, 0);
                double x = sqlite3_column_double(stmt, 1);
                double y = sqlite3_column_double(stmt, 2);
                double z = sqlite3_column_double(stmt, 3);

                Time t = Time(timeStep);
                Vector pos = Vector(x, y, z);

                nodePositions[t] = pos;
            }

            CheckQueryReturnCode(
                stmt,
                rc,
                FormatBoundArgsList(e2NodeId, fromTime.GetTimeStep(), toTime.GetTimeStep()));
            sqlite3_finalize(stmt);
        }
    }
    return nodePositions;
}

std::tuple<bool, uint16_t, uint16_t>
OranDataRepositorySqlite::GetLteUeCellInfo(uint64_t e2NodeId)
{
    NS_LOG_FUNCTION(this << e2NodeId);

    auto retVal = std::make_tuple(false, 0, 0);
    if (m_active)
    {
        if (IsNodeRegistered(e2NodeId))
        {
            int rc;
            sqlite3_stmt* stmt = nullptr;

            sqlite3_prepare_v2(m_db,
                               m_queryStmtsStrings[GET_LTE_UE_CELLINFO].c_str(),
                               -1,
                               &stmt,
                               0);
            sqlite3_bind_int64(stmt, 1, e2NodeId);

            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
            {
                uint16_t cellId = sqlite3_column_int(stmt, 0);
                uint16_t rnti = sqlite3_column_int(stmt, 1);
                retVal = std::make_tuple(true, cellId, rnti);
            }

            CheckQueryReturnCode(stmt, rc, FormatBoundArgsList(e2NodeId));
            sqlite3_finalize(stmt);
        }
    }
    return retVal;
}

std::vector<uint64_t>
OranDataRepositorySqlite::GetLteUeE2NodeIds()
{
    NS_LOG_FUNCTION(this);

    std::vector<uint64_t> e2NodeIds;

    if (m_active)
    {
        int rc;
        sqlite3_stmt* stmt = nullptr;

        if (sqlite3_prepare_v2(m_db,
                               m_queryStmtsStrings[GET_LTE_ALL_UE_E2NODEIDS].c_str(),
                               -1,
                               &stmt,
                               0) != SQLITE_OK)
        {
            std::cerr << "SQL Error: " << sqlite3_errmsg(m_db) << std::endl;
        }

        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        {
            e2NodeIds.push_back(sqlite3_column_int64(stmt, 0));
        }

        CheckQueryReturnCode(stmt, rc);
        sqlite3_finalize(stmt);
    }
    return e2NodeIds;
}

double
OranDataRepositorySqlite::GetAppLoss(uint64_t e2NodeId)
{
    NS_LOG_FUNCTION(this << e2NodeId);

    double loss = 0;

    if (m_active)
    {
        if (IsNodeRegistered(e2NodeId))
        {
            int rc;
            std::string query;
            sqlite3_stmt* stmt = nullptr;

            query = "SELECT loss"
                    " FROM nodeapploss"
                    " WHERE nodeid = ?"
                    " ORDER BY entryid DESC LIMIT 1"
                    ";";

            sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);

            sqlite3_bind_int64(stmt, 1, e2NodeId);

            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
            {
                loss = sqlite3_column_double(stmt, 0);
            }

            CheckQueryReturnCode(stmt, rc, FormatBoundArgsList(e2NodeId));
            sqlite3_finalize(stmt);
        }
    }
    return loss;
}

uint64_t
OranDataRepositorySqlite::GetLteUeE2NodeIdFromCellInfo(uint16_t cellId, uint16_t rnti)
{
    NS_LOG_FUNCTION(this << cellId << rnti);

    uint64_t id = 0;
    if (m_active)
    {
        int rc;
        sqlite3_stmt* stmt = nullptr;

        sqlite3_prepare_v2(m_db,
                           m_queryStmtsStrings[GET_LTE_UE_E2NODEID_FROM_CELLINFO].c_str(),
                           -1,
                           &stmt,
                           0);
        sqlite3_bind_int(stmt, 1, cellId);
        sqlite3_bind_int(stmt, 2, rnti);

        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        {
            id = sqlite3_column_int64(stmt, 0);
        }

        CheckQueryReturnCode(stmt, rc, FormatBoundArgsList(cellId, rnti));
        sqlite3_finalize(stmt);
    }
    return id;
}

std::tuple<bool, uint16_t>
OranDataRepositorySqlite::GetLteEnbCellInfo(uint64_t e2NodeId)
{
    NS_LOG_FUNCTION(this << e2NodeId);

    auto retVal = std::make_tuple(false, 0);
    if (m_active)
    {
        if (IsNodeRegistered(e2NodeId))
        {
            int rc;
            sqlite3_stmt* stmt = nullptr;

            sqlite3_prepare_v2(m_db,
                               m_queryStmtsStrings[GET_LTE_CELLID_FROM_E2NODEID].c_str(),
                               -1,
                               &stmt,
                               0);
            sqlite3_bind_int64(stmt, 1, e2NodeId);

            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
            {
                uint16_t cellId = sqlite3_column_int(stmt, 0);
                retVal = std::make_tuple(true, cellId);
            }

            CheckQueryReturnCode(stmt, rc, FormatBoundArgsList(e2NodeId));
            sqlite3_finalize(stmt);
        }
    }
    return retVal;
}

std::vector<uint64_t>
OranDataRepositorySqlite::GetLteEnbE2NodeIds()
{
    NS_LOG_FUNCTION(this);

    std::vector<uint64_t> e2NodeIds;

    if (m_active)
    {
        int rc;
        sqlite3_stmt* stmt = nullptr;

        sqlite3_prepare_v2(m_db,
                           m_queryStmtsStrings[GET_LTE_ALL_ENB_E2NODEIDS].c_str(),
                           -1,
                           &stmt,
                           0);

        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        {
            e2NodeIds.push_back(sqlite3_column_int64(stmt, 0));
        }

        CheckQueryReturnCode(stmt, rc);
        sqlite3_finalize(stmt);
    }
    return e2NodeIds;
}

std::vector<std::tuple<uint64_t, Time>>
OranDataRepositorySqlite::GetLastRegistrationRequests()
{
    NS_LOG_FUNCTION(this);

    std::vector<std::tuple<uint64_t, Time>> requests;
    if (m_active)
    {
        int rc;
        sqlite3_stmt* stmt = nullptr;

        sqlite3_prepare_v2(m_db,
                           m_queryStmtsStrings[GET_ALL_LAST_REGISTRATION_TIMES].c_str(),
                           -1,
                           &stmt,
                           0);

        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        {
            uint64_t e2NodeId = sqlite3_column_int64(stmt, 0);
            Time t = Time(sqlite3_column_int64(stmt, 1));

            requests.push_back(std::make_tuple(e2NodeId, t));
        }

        CheckQueryReturnCode(stmt, rc);
        sqlite3_finalize(stmt);
    }

    return requests;
}

std::vector<std::tuple<uint16_t, uint16_t, double, double, bool, uint8_t>>
OranDataRepositorySqlite::GetLteUeRsrpRsrq(uint64_t e2NodeId)
{
    NS_LOG_FUNCTION(this << e2NodeId);

    std::vector<std::tuple<uint16_t, uint16_t, double, double, bool, uint8_t>> retVal;

    if (m_active)
    {
        if (IsNodeRegistered(e2NodeId))
        {
            int rc;
            sqlite3_stmt* stmt = nullptr;

            sqlite3_prepare_v2(m_db,
                               m_queryStmtsStrings[GET_LTE_UE_RSRP_RSRQ].c_str(),
                               -1,
                               &stmt,
                               0);
            sqlite3_bind_int64(stmt, 1, e2NodeId);
            sqlite3_bind_int64(stmt, 2, e2NodeId);

            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
            {
                uint16_t rnti = sqlite3_column_int(stmt, 0);
                uint16_t cellId = sqlite3_column_int(stmt, 1);
                double rsrp = sqlite3_column_double(stmt, 2);
                double rsrq = sqlite3_column_double(stmt, 3);
                bool isServing = sqlite3_column_int(stmt, 4);
                uint8_t componentCarrierId = sqlite3_column_int(stmt, 5);

                retVal.push_back(
                    std::make_tuple(rnti, cellId, rsrp, rsrq, isServing, componentCarrierId));
            }

            CheckQueryReturnCode(stmt, rc, FormatBoundArgsList(e2NodeId, e2NodeId));
            sqlite3_finalize(stmt);
        }
    }
    return retVal;
}

static inline std::string
SanitizeSqlText(std::string s)
{
  // 1) truncate at first NUL (guarantee no embedded \0)
  s.assign(s.c_str());

  // 2) strip other non-printable control chars except \n \t
  for (char& c : s)
  {
    const unsigned char u = static_cast<unsigned char>(c);
    if ((u < 0x20 && c != '\n' && c != '\t') || u == 0x7F)
    {
      c = ' ';
    }
  }

  // 3) cap length (avoid huge blobs)
  constexpr size_t kMax = 256;
  if (s.size() > kMax) s.resize(kMax);

  return s;
}

void
OranDataRepositorySqlite::LogCommandE2Terminator(Ptr<OranCommand> cmd)
{
    NS_LOG_FUNCTION(this);

    if (m_active)
    {
        if (IsNodeRegistered(cmd->GetTargetE2NodeId()))
        {
            int rc;
            sqlite3_stmt* stmt = nullptr;

            sqlite3_prepare_v2(m_db,
                               m_queryStmtsStrings[LOG_E2TERMINATOR_COMMAND].c_str(),
                               -1,
                               &stmt,
                               0);

            sqlite3_bind_int64(stmt, 1, cmd->GetTargetE2NodeId());
            sqlite3_bind_int64(stmt, 2, Simulator::Now().GetTimeStep());
            std::string cmdStr = SanitizeSqlText(cmd->ToString());
            sqlite3_bind_text(stmt, 3, cmdStr.c_str(), -1, SQLITE_TRANSIENT);

            rc = sqlite3_step(stmt);
            CheckQueryReturnCode(stmt,
                                 rc,
                                 FormatBoundArgsList(cmd->GetTargetE2NodeId(),
                                                     Simulator::Now().GetTimeStep(),
                                                     cmd->ToString()));
            sqlite3_finalize(stmt);
        }
    }
}

void
OranDataRepositorySqlite::LogCommandLm(std::string lm, Ptr<OranCommand> cmd)
{
    NS_LOG_FUNCTION(this);

    if (m_active)
    {
        int rc;
        sqlite3_stmt* stmt = nullptr;

        sqlite3_prepare_v2(m_db, m_queryStmtsStrings[LOG_LM_COMMAND].c_str(), -1, &stmt, 0);

        sqlite3_bind_text(stmt, 1, lm.c_str(), -1, 0);
        sqlite3_bind_int64(stmt, 2, Simulator::Now().GetTimeStep());
        std::string cmdStr = SanitizeSqlText(cmd->ToString());
        sqlite3_bind_text(stmt, 3, cmdStr.c_str(), -1, SQLITE_TRANSIENT);

        rc = sqlite3_step(stmt);
        CheckQueryReturnCode(
            stmt,
            rc,
            FormatBoundArgsList(lm, Simulator::Now().GetTimeStep(), cmd->ToString()));
        sqlite3_finalize(stmt);
    }
}

void
OranDataRepositorySqlite::LogActionLm(std::string lm, std::string logStr)
{
    NS_LOG_FUNCTION(this << lm << logStr);

    if (m_active)
    {
        int rc;
        sqlite3_stmt* stmt = nullptr;

        sqlite3_prepare_v2(m_db, m_queryStmtsStrings[LOG_LM_ACTION].c_str(), -1, &stmt, 0);

        sqlite3_bind_text(stmt, 1, lm.c_str(), -1, 0);
        sqlite3_bind_int64(stmt, 2, Simulator::Now().GetTimeStep());
        sqlite3_bind_text(stmt, 3, logStr.c_str(), -1, 0);

        rc = sqlite3_step(stmt);

        CheckQueryReturnCode(stmt,
                             rc,
                             FormatBoundArgsList(lm, Simulator::Now().GetTimeStep(), logStr));
        sqlite3_finalize(stmt);
    }
}

void
OranDataRepositorySqlite::LogActionCmm(std::string cmm, std::string logStr)
{
    NS_LOG_FUNCTION(this << cmm << logStr);

    if (m_active)
    {
        int rc;
        sqlite3_stmt* stmt = nullptr;

        sqlite3_prepare_v2(m_db, m_queryStmtsStrings[LOG_CMM_ACTION].c_str(), -1, &stmt, 0);

        sqlite3_bind_text(stmt, 1, cmm.c_str(), -1, 0);
        sqlite3_bind_int64(stmt, 2, Simulator::Now().GetTimeStep());
        sqlite3_bind_text(stmt, 3, logStr.c_str(), -1, 0);

        rc = sqlite3_step(stmt);

        CheckQueryReturnCode(stmt,
                             rc,
                             FormatBoundArgsList(cmm, Simulator::Now().GetTimeStep(), logStr));
        sqlite3_finalize(stmt);
    }
}

void
OranDataRepositorySqlite::CheckQueryReturnCode(sqlite3_stmt* stmt,
                                               int rc,
                                               std::string boundParmsStr) const
{
    NS_LOG_FUNCTION(this << stmt << rc);

    // Get the formated string of the prepared statement
    const char* sqlC = sqlite3_sql(stmt);
    std::string stmtStr = sqlC ? std::string(sqlC) : std::string("<null-sql>");

    // Trace the result of the query
    m_queryRc(stmtStr, boundParmsStr, rc);

    if (rc == SQLITE_OK || rc == SQLITE_DONE)
    {
        NS_LOG_INFO("Query SUCCESSFUL: \"" << stmtStr << "\"; " << boundParmsStr);
    }
    else
    {
        NS_ABORT_MSG("Query FAILED: \"" << stmtStr << "\"; (" << boundParmsStr << "); RC = " << rc);
    }
}

void
OranDataRepositorySqlite::CloseDb()
{
    NS_LOG_FUNCTION(this);

    sqlite3_close(m_db);
    m_db = nullptr;
}

void
OranDataRepositorySqlite::DoDispose()
{
    NS_LOG_FUNCTION(this);

    if (IsDbOpen())
    {
        CloseDb();
    }

    OranDataRepository::DoDispose();
}

bool
OranDataRepositorySqlite::IsDbOpen() const
{
    NS_LOG_FUNCTION(this);

    return m_db != nullptr;
}

void
OranDataRepositorySqlite::OpenDb()
{
    NS_LOG_FUNCTION(this);

    // Check for special file names and print a warning if we find them
    if (m_dbPath == ":memory:")
    {
        NS_LOG_WARN("Using in-memory DB for the ORAN Storage. DB will not be saved to disk.");
        std::cerr
            << "WARNING: Using in-memory DB for the ORAN Storage. DB will not be saved to disk."
            << std::endl;
    }
    else
    {
        // Check for DB names that are URIs. We do not support those
        if (m_dbPath.find(":") != std::string::npos)
        {
            NS_ABORT_MSG("File name for the ORAN Storage DB ("
                         << m_dbPath << ") is an URI. URI-named DBs are not supported");
        }
    }

    if (m_dbPath.empty())
    {
        NS_LOG_WARN("Using a randomly named temporary file as DB for the ORAN Storage. DB will not "
                    "be available after simulation ends.");
        std::cerr << "WARNING: Using a randomly named temporary file as DB for the ORAN Storage. "
                     "DB will not be available after simulation ends."
                  << std::endl;
    }

    int error = sqlite3_open(m_dbPath.c_str(), &m_db);
    if (error != 0)
    {
        NS_ABORT_MSG("Could not open database: " << sqlite3_errmsg(m_db));
    }
    NS_LOG_INFO("Oran repository \"" << m_dbPath << "\" connected to successfully!");
    sqlite3_busy_timeout(m_db, 2000); // 2s is usually enough

    auto ExecPragma = [this](const char* sql) {
        char* err = nullptr;
        int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &err);
        if (rc != SQLITE_OK)
        {
            NS_LOG_WARN("sqlite3_exec failed rc=" << rc
                        << " sql=\"" << sql << "\""
                        << " err=\"" << (err ? err : "") << "\"");
        }
        if (err)
        {
            sqlite3_free(err);
            err = nullptr;
        }
    };

    ExecPragma("PRAGMA journal_mode=WAL;");
    ExecPragma("PRAGMA synchronous=NORMAL;");

    InitDb();
}

void
OranDataRepositorySqlite::InitDb()
{
    NS_LOG_FUNCTION(this);

    // E2 Node Table
    RunCreateStatement(m_createStmtsStrings[TABLE_NODE]);
    RunCreateStatement(m_createStmtsStrings[INDEX_NODE]);

    // E2 Node Registration
    RunCreateStatement(m_createStmtsStrings[TABLE_NODE_REGISTRATION]);
    RunCreateStatement(m_createStmtsStrings[INDEX_NODE_REGISTRATION]);

    // E2 Node Location
    RunCreateStatement(m_createStmtsStrings[TABLE_NODE_LOCATION]);
    RunCreateStatement(m_createStmtsStrings[INDEX_NODE_LOCATION]);

    // LTE eNB
    RunCreateStatement(m_createStmtsStrings[TABLE_LTE_ENB]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_ENB_NODEID]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_ENB_CELLID]);

    // LTE UE
    RunCreateStatement(m_createStmtsStrings[TABLE_LTE_UE]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_UE_NODEID]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_UE_IMSI]);

    // LTE UE Cell Information
    RunCreateStatement(m_createStmtsStrings[TABLE_LTE_UE_CELL]);
    RunCreateStatement(m_createStmtsStrings[TABLE_LTE_UE_RSRP_RSRQ]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_UE_CELL_NODEID]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_UE_CELL_CELLID]);

    RunCreateStatement(m_createStmtsStrings[TABLE_APPLOSS_COMMAND]);

    // E2 Terminator Commands
    RunCreateStatement(m_createStmtsStrings[TABLE_TERMINATOR_COMMAND]);

    // LM Commands
    RunCreateStatement(m_createStmtsStrings[TABLE_LM_COMMAND]);

    // LM Actions (Internal Log)
    RunCreateStatement(m_createStmtsStrings[TABLE_LM_ACTION]);

    // CMM Actions (Internal Log)
    RunCreateStatement(m_createStmtsStrings[TABLE_CMM_ACTION]);

    RunCreateStatement(m_createStmtsStrings[TABLE_LTE_ENB_PRB_UTIL]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_ENB_PRB_UTIL_NODEID]);

    RunCreateStatement(m_createStmtsStrings[TABLE_LTE_UE_SINR]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_UE_SINR_NODEID]);

    RunCreateStatement(m_createStmtsStrings[TABLE_LTE_ENB_SCHED_TP]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_ENB_SCHED_TP_NODEID]);

    RunCreateStatement(m_createStmtsStrings[TABLE_LTE_UE_PDCP_TP]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_UE_PDCP_TP_UEID]);

    RunCreateStatement(m_createStmtsStrings[TABLE_LTE_ENB_MCS]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_ENB_MCS_CELL]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_ENB_MCS_E2]);

    RunCreateStatement(m_createStmtsStrings[TABLE_LTE_UE_PRB]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_UE_PRB_UEID]);

    RunCreateStatement(m_createStmtsStrings[TABLE_LTE_ENB_UL_INTERF]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_ENB_UL_INTERF_E2_CELL]);

    RunCreateStatement(m_createStmtsStrings[TABLE_LTE_HO_EVENTS]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_HO_EVENTS_UEID]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_HO_EVENTS_TIME]);

    RunCreateStatement(m_createStmtsStrings[TABLE_LTE_UE_DWELL]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_UE_DWELL_UEID]);

    RunCreateStatement(m_createStmtsStrings[TABLE_LTE_ENB_UECOUNT]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_ENB_UECOUNT_TIME]);
    RunCreateStatement(m_createStmtsStrings[INDEX_LTE_ENB_UECOUNT_CELL]);
}

void
OranDataRepositorySqlite::InitStatements()
{
    NS_LOG_FUNCTION(this);

    // Initialize the create statements
    m_createStmtsStrings[INDEX_LTE_ENB_CELLID] = "CREATE INDEX IF NOT EXISTS "
                                                 "idx_lteenb_cellid ON lteenb(cellid);";

    m_createStmtsStrings[INDEX_LTE_ENB_NODEID] = "CREATE INDEX IF NOT EXISTS "
                                                 "idx_lteenb_nodeid ON lteenb(nodeid);";

    m_createStmtsStrings[INDEX_LTE_UE_CELL_CELLID] = "CREATE INDEX IF NOT EXISTS "
                                                     "idx_lteuecell_cellid ON lteuecell(cellid);";

    m_createStmtsStrings[INDEX_LTE_UE_CELL_NODEID] = "CREATE INDEX IF NOT EXISTS "
                                                     "idx_lteuecell_nodeid ON lteuecell(nodeid);";

    m_createStmtsStrings[INDEX_LTE_UE_IMSI] = "CREATE INDEX IF NOT EXISTS "
                                              "idx_lteue_imsi ON lteue(imsi);";

    m_createStmtsStrings[INDEX_LTE_UE_NODEID] = "CREATE INDEX IF NOT EXISTS "
                                                "idx_lteue_nodeid ON lteue(nodeid);";

    m_createStmtsStrings[INDEX_NODE] = "CREATE INDEX IF NOT EXISTS "
                                       "idx_node_nodeid ON node (nodeid);";

    m_createStmtsStrings[INDEX_NODE_LOCATION] = "CREATE INDEX IF NOT EXISTS "
                                                "idx_nodelocation_nodeid ON nodelocation(nodeid);";

    m_createStmtsStrings[INDEX_NODE_REGISTRATION] =
        "CREATE INDEX IF NOT EXISTS "
        "idx_noderegistration_nodeid ON noderegistration(nodeid);";

    m_createStmtsStrings[TABLE_CMM_ACTION] =
        "CREATE TABLE IF NOT EXISTS cmmaction ("
        "entryid        INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
        "cmmname        TEXT                              NOT NULL, "
        "simulationtime INTEGER                           NOT NULL, "
        "description    TEXT                              NOT NULL);";

    m_createStmtsStrings[TABLE_LM_ACTION] =
        "CREATE TABLE IF NOT EXISTS lmaction ("
        "entryid        INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
        "lmname         TEXT                              NOT NULL, "
        "simulationtime INTEGER                           NOT NULL, "
        "description    TEXT                              NOT NULL);";

    m_createStmtsStrings[TABLE_LM_COMMAND] =
        "CREATE TABLE IF NOT EXISTS lmcommand ("
        "entryid        INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
        "lmname         TEXT                              NOT NULL, "
        "simulationtime INTEGER                           NOT NULL, "
        "cmdname        TEXT                              NOT NULL);";

    m_createStmtsStrings[TABLE_LTE_ENB] = "CREATE TABLE IF NOT EXISTS lteenb ("
                                          "nodeid INTEGER PRIMARY KEY NOT NULL, "
                                          "cellid INTEGER             NOT NULL, "
                                          "FOREIGN KEY(nodeid) REFERENCES node(nodeid));";

    m_createStmtsStrings[TABLE_LTE_UE] = "CREATE TABLE IF NOT EXISTS lteue ("
                                         "nodeid INTEGER PRIMARY KEY NOT NULL, "
                                         "imsi   INTEGER UNIQUE      NOT NULL, "
                                         "FOREIGN KEY(nodeid) REFERENCES node(nodeid));";

    m_createStmtsStrings[TABLE_LTE_UE_CELL] =
        "CREATE TABLE IF NOT EXISTS lteuecell ("
        "entryid        INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
        "nodeid         INTEGER                           NOT NULL, "
        "cellid         INTEGER                           NOT NULL, "
        "rnti           INTEGER                           NOT NULL, "
        "simulationtime INTEGER                           NOT NULL, "
        "FOREIGN KEY(cellid) REFERENCES lteenb(cellid), "
        "FOREIGN KEY(nodeid) REFERENCES lteue(nodeid));";

    m_createStmtsStrings[TABLE_LTE_UE_RSRP_RSRQ] =
        "CREATE TABLE IF NOT EXISTS lteuersrprsrq ("
        "entryid        INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
        "nodeid         INTEGER                           NOT NULL, "
        "simulationtime INTEGER                           NOT NULL, "
        "rnti           INTEGER                           NOT NULL, "
        "cellid         INTEGER                           NOT NULL, "
        "rsrp           REAL                              NOT NULL, "
        "rsrq           REAL                              NOT NULL, "
        "serving        BOOLEAN                           NOT NULL, "
        "ccid           INTEGER                           NOT NULL, "
        "FOREIGN KEY(cellid) REFERENCES lteenb(cellid), "
        "FOREIGN KEY(nodeid) REFERENCES lteue(nodeid));";



    // --- DDL: table + index ---
    m_createStmtsStrings[TABLE_LTE_ENB_PRB_UTIL] =
        "CREATE TABLE IF NOT EXISTS lteenbprbutilization ("
        "  entryid        INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
        "  nodeid         INTEGER                           NOT NULL, "
        "  cellid         INTEGER                           NOT NULL, "
        "  dlutil         REAL                              NOT NULL, "
        "  ulutil         REAL                              NOT NULL, "
        "  window_ms      INTEGER                           NOT NULL, "
        "  ttis_observed  INTEGER                           NOT NULL, "
        "  simulationtime INTEGER                           NOT NULL, "
        "  FOREIGN KEY(nodeid) REFERENCES lteenb(nodeid)"
        ");";

    m_createStmtsStrings[INDEX_LTE_ENB_PRB_UTIL_NODEID] =
        "CREATE INDEX IF NOT EXISTS idx_lteenbprbutil_nodeid "
        "ON lteenbprbutilization(nodeid);";

    // --- DML: insert ---
    m_queryStmtsStrings[INSERT_LTE_ENB_PRB_UTIL] =
        "INSERT INTO lteenbprbutilization "
        "(nodeid, cellid, dlutil, ulutil, window_ms, ttis_observed, simulationtime) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";
        
    // --- query average over a time window ---
    m_queryStmtsStrings[GET_AVG_LTE_ENB_PRB_UTIL] =
        "SELECT AVG(dlutil), AVG(ulutil) "
        "FROM lteenbprbutilization "
        "WHERE nodeid = ? AND simulationtime >= ? AND simulationtime <= ?;";



    m_createStmtsStrings[TABLE_LTE_UE_SINR] =
        "CREATE TABLE IF NOT EXISTS lteuesinr ("
        "  entryid        INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
        "  simulationtime INTEGER                           NOT NULL, "
        "  nodeid         INTEGER                           NOT NULL, "
        "  cellid         INTEGER                           NOT NULL, "
        "  rnti           INTEGER                           NOT NULL, "
        "  sinr_linear    REAL                              NOT NULL, "
        "  sinr_db        REAL                              NOT NULL, "
        "  rsrp_mw        REAL                              NOT NULL, "
        "  rsrp_dbm       REAL                              NOT NULL, "
        "  FOREIGN KEY(nodeid) REFERENCES lteue(nodeid)"
        ");";

    m_createStmtsStrings[INDEX_LTE_UE_SINR_NODEID] =
        "CREATE INDEX IF NOT EXISTS idx_lteuesinr_nodeid "
        "ON lteuesinr(nodeid);";



    m_createStmtsStrings[TABLE_LTE_ENB_SCHED_TP] =
        "CREATE TABLE IF NOT EXISTS lteenb_sched_tp ("
        "  entryid INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,"
        "  nodeid INTEGER NOT NULL,"
        "  cellid INTEGER NOT NULL,"
        "  dl_mbps REAL NOT NULL,"
        "  ul_mbps REAL NOT NULL,"
        "  window_ms INTEGER NOT NULL,"
        "  ttis_observed INTEGER NOT NULL,"
        "  simulationtime INTEGER NOT NULL,"
        "  FOREIGN KEY(nodeid) REFERENCES lteenb(nodeid)"
        ");";

    m_createStmtsStrings[INDEX_LTE_ENB_SCHED_TP_NODEID] =
        "CREATE INDEX IF NOT EXISTS idx_lteenb_sched_tp_nodeid "
        "ON lteenb_sched_tp(nodeid);";

    m_queryStmtsStrings[INSERT_LTE_ENB_SCHED_TP] =
        "INSERT INTO lteenb_sched_tp "
        "(nodeid, cellid, dl_mbps, ul_mbps, window_ms, ttis_observed, simulationtime) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";
    


    m_createStmtsStrings[TABLE_LTE_UE_PDCP_TP] =
    "CREATE TABLE IF NOT EXISTS lteue_pdcp_tp ("
    " entryid INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
    " ueid INTEGER NOT NULL, "
    " cellid INTEGER NOT NULL, "
    " dlmbps REAL NOT NULL, "
    " ulmbps REAL NOT NULL, "
    " simulationtime INTEGER NOT NULL, "
    " FOREIGN KEY(ueid) REFERENCES lteue(nodeid)"
    ");";

    m_createStmtsStrings[INDEX_LTE_UE_PDCP_TP_UEID] =
    "CREATE INDEX IF NOT EXISTS idx_lteue_pdcp_tp_ueid "
    "ON lteue_pdcp_tp(ueid);";

    m_queryStmtsStrings[INSERT_LTE_UE_PDCP_TP] =
    "INSERT INTO lteue_pdcp_tp (ueid, cellid, dlmbps, ulmbps, simulationtime) "
    "VALUES (?, ?, ?, ?, ?);";

            

    m_createStmtsStrings[TABLE_LTE_ENB_MCS] =
    "CREATE TABLE IF NOT EXISTS lteenb_mcs ("
    " entryid        INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
    " e2nodeid       INTEGER NOT NULL, "
    " cellid         INTEGER NOT NULL, "
    " dl_mean        REAL    NOT NULL, "
    " dl_p50         REAL    NOT NULL, "
    " dl_p95         REAL    NOT NULL, "
    " ul_mean        REAL    NOT NULL, "
    " ul_p50         REAL    NOT NULL, "
    " ul_p95         REAL    NOT NULL, "
    " simulationtime INTEGER NOT NULL "
    ");";

    m_createStmtsStrings[INDEX_LTE_ENB_MCS_CELL] =
    "CREATE INDEX IF NOT EXISTS idx_lteenb_mcs_cell "
    "ON lteenb_mcs(cellid);";

    m_createStmtsStrings[INDEX_LTE_ENB_MCS_E2] =
    "CREATE INDEX IF NOT EXISTS idx_lteenb_mcs_e2 "
    "ON lteenb_mcs(e2nodeid);";

    // query
    m_queryStmtsStrings[INSERT_LTE_ENB_MCS] =
    "INSERT INTO lteenb_mcs "
    "(e2nodeid, cellid, dl_mean, dl_p50, dl_p95, ul_mean, ul_p50, ul_p95, simulationtime) "
    "VALUES (?,?,?,?,?,?,?,?,?);";
    

        
    m_createStmtsStrings[TABLE_LTE_UE_PRB] =
    "CREATE TABLE IF NOT EXISTS lteue_prb ("
    " entryid INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
    " ueid INTEGER NOT NULL, "
    " cellid INTEGER NOT NULL, "
    " dl_prbs INTEGER NOT NULL, "
    " ul_prbs INTEGER NOT NULL, "
    " simulationtime INTEGER NOT NULL, "
    " FOREIGN KEY(ueid) REFERENCES lteue(nodeid)"
    ");";

    m_createStmtsStrings[INDEX_LTE_UE_PRB_UEID] =
    "CREATE INDEX IF NOT EXISTS idx_lteue_prb_ueid ON lteue_prb(ueid);";

    m_queryStmtsStrings[INSERT_LTE_UE_PRB] =
    "INSERT INTO lteue_prb (ueid, cellid, dl_prbs, ul_prbs, simulationtime) "
    "VALUES (?, ?, ?, ?, ?);";



    m_createStmtsStrings[TABLE_LTE_ENB_UL_INTERF] =
    "CREATE TABLE IF NOT EXISTS lteenb_ul_interf ("
    " entryid INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
    " e2nodeid INTEGER NOT NULL, "
    " cellid   INTEGER NOT NULL, "
    " p95mw    REAL NOT NULL, "
    " p95dbm   REAL NOT NULL, "
    " simulationtime INTEGER NOT NULL"
    ");";

    m_createStmtsStrings[INDEX_LTE_ENB_UL_INTERF_E2_CELL] =
    "CREATE INDEX IF NOT EXISTS idx_lteenb_ul_interf_e2_cell "
    "ON lteenb_ul_interf(e2nodeid, cellid);";

    m_queryStmtsStrings[INSERT_LTE_ENB_UL_INTERF] =
    "INSERT INTO lteenb_ul_interf (e2nodeid, cellid, p95mw, p95dbm, simulationtime) "
    "VALUES (?, ?, ?, ?, ?);";


        
    m_createStmtsStrings[TABLE_LTE_HO_EVENTS] =
    "CREATE TABLE IF NOT EXISTS lte_ho_events("
    " entryid INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,"
    " imsi INTEGER NOT NULL,"
    " ueid INTEGER NOT NULL,"
    " srcCell INTEGER NOT NULL,"
    " dstCell INTEGER NOT NULL,"
    " event TEXT NOT NULL,"
    " simulationtime INTEGER NOT NULL);";

    m_createStmtsStrings[INDEX_LTE_HO_EVENTS_UEID] =
    "CREATE INDEX IF NOT EXISTS idx_lte_ho_events_ueid "
    "ON lte_ho_events(ueid);";

    m_createStmtsStrings[INDEX_LTE_HO_EVENTS_TIME] =
    "CREATE INDEX IF NOT EXISTS idx_lte_ho_events_time "
    "ON lte_ho_events(simulationtime);";

    m_createStmtsStrings[TABLE_LTE_UE_DWELL] =
    "CREATE TABLE IF NOT EXISTS lteue_dwell("
    " entryid INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,"
    " ueid INTEGER NOT NULL,"
    " cellid INTEGER NOT NULL,"
    " dwell_s REAL NOT NULL,"
    " simulationtime INTEGER NOT NULL);";

    m_createStmtsStrings[INDEX_LTE_UE_DWELL_UEID] =
    "CREATE INDEX IF NOT EXISTS idx_lteue_dwell_ueid "
    "ON lteue_dwell(ueid);";

    m_queryStmtsStrings[INSERT_LTE_HO_EVENT] =
    "INSERT INTO lte_ho_events (imsi, ueid, srcCell, dstCell, event, simulationtime) "
    "VALUES (?, ?, ?, ?, ?, ?);";

    m_queryStmtsStrings[INSERT_LTE_UE_DWELL] =
    "INSERT INTO lteue_dwell (ueid, cellid, dwell_s, simulationtime) "
    "VALUES (?, ?, ?, ?);";



    m_createStmtsStrings[TABLE_LTE_ENB_UECOUNT] =
    "CREATE TABLE IF NOT EXISTS lteenb_uecount("
    " entryid INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,"
    " e2nodeid INTEGER NOT NULL,"
    " cellid   INTEGER NOT NULL,"
    " ue_count INTEGER NOT NULL,"
    " simulationtime INTEGER NOT NULL);";

    m_createStmtsStrings[INDEX_LTE_ENB_UECOUNT_TIME] =
    "CREATE INDEX IF NOT EXISTS idx_lteenb_uecount_time ON lteenb_uecount(simulationtime);";

    m_createStmtsStrings[INDEX_LTE_ENB_UECOUNT_CELL] =
    "CREATE INDEX IF NOT EXISTS idx_lteenb_uecount_cell ON lteenb_uecount(cellid);";

    m_queryStmtsStrings[INSERT_LTE_ENB_UECOUNT] =
    "INSERT INTO lteenb_uecount (e2nodeid, cellid, ue_count, simulationtime) "
    "VALUES (?, ?, ?, ?);";



    m_createStmtsStrings[TABLE_NODE] =
        "CREATE TABLE IF NOT EXISTS node ("
        "nodeid         INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
        "nodetype       INTEGER                           NOT NULL);";

    m_createStmtsStrings[TABLE_NODE_LOCATION] =
        "CREATE TABLE IF NOT EXISTS nodelocation ("
        "entryid        INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
        "nodeid         INTEGER                           NOT NULL, "
        "x              REAL                              NOT NULL, "
        "y              REAL                              NOT NULL, "
        "z              REAL                              NOT NULL, "
        "simulationtime INTEGER                           NOT NULL, "
        "FOREIGN KEY(nodeid) REFERENCES node(nodeid));";

    m_createStmtsStrings[TABLE_NODE_REGISTRATION] =
        "CREATE TABLE IF NOT EXISTS noderegistration ("
        "entryid        INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
        "nodeid         INTEGER                           NOT NULL, "
        "registered     BOOLEAN                           NOT NULL, "
        "simulationtime INTEGER                           NOT NULL, "
        "FOREIGN KEY(nodeid) REFERENCES node(nodeid));";

    m_createStmtsStrings[TABLE_TERMINATOR_COMMAND] =
        "CREATE TABLE IF NOT EXISTS terminatorcommand ("
        "entryid        INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
        "targetid       INTEGER                           NOT NULL, "
        "simulationtime INTEGER                           NOT NULL, "
        "cmdname        TEXT                              NOT NULL, "
        "FOREIGN KEY(targetid) REFERENCES node(nodeid));";

    m_createStmtsStrings[TABLE_APPLOSS_COMMAND] =
        "CREATE TABLE IF NOT EXISTS nodeapploss ("
        "entryid        INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
        "nodeid         INTEGER                           NOT NULL, "
        "loss           REAL                              NOT NULL, "
        "simulationtime INTEGER                           NOT NULL, "
        "FOREIGN KEY(nodeid) REFERENCES node(nodeid)              );";

    // Query Statements
    m_queryStmtsStrings[CHECK_NODE_REGISTERED] = "SELECT registered "
                                                 "FROM noderegistration "
                                                 "WHERE nodeid = ? "
                                                 "ORDER BY simulationtime DESC, entryid DESC "
                                                 "LIMIT 1;";

    m_queryStmtsStrings[GET_ALL_LAST_REGISTRATION_TIMES] = "SELECT nodeid, MAX(simulationtime) "
                                                           "FROM noderegistration "
                                                           "GROUP BY nodeid "
                                                           "HAVING registered = 1 "
                                                           "ORDER BY nodeid;";

    m_queryStmtsStrings[GET_LTE_ALL_ENB_E2NODEIDS] =
        "SELECT nr.nodeid, MAX(nr.simulationtime) "
        "FROM noderegistration AS nr "
        "INNER JOIN lteenb ON lteenb.nodeid = nr.nodeid "
        "GROUP BY nr.nodeid "
        "HAVING nr.registered = 1 "
        "ORDER BY nr.nodeid;";

    m_queryStmtsStrings[GET_LTE_ALL_UE_E2NODEIDS] = "SELECT nr.nodeid, MAX(nr.simulationtime) "
                                                    "FROM noderegistration AS nr "
                                                    "INNER JOIN lteue ON lteue.nodeid = nr.nodeid "
                                                    "GROUP BY nr.nodeid "
                                                    "HAVING nr.registered = 1 "
                                                    "ORDER BY nr.nodeid;";

    m_queryStmtsStrings[GET_LTE_CELLID_FROM_E2NODEID] = "SELECT cellid "
                                                        "FROM lteenb "
                                                        "WHERE nodeid = ?;";

    m_queryStmtsStrings[GET_LTE_UE_CELLINFO] = "SELECT cellid, rnti "
                                               "FROM lteuecell "
                                               "WHERE nodeid = ? "
                                               "ORDER BY simulationtime DESC, entryid DESC "
                                               "LIMIT 1;";

    m_queryStmtsStrings[GET_LTE_UE_E2NODEID_FROM_CELLINFO] = "SELECT nodeid "
                                                             "FROM lteuecell "
                                                             "WHERE cellid = ? AND rnti = ? "
                                                             "ORDER BY entryid DESC LIMIT 1;";

    m_queryStmtsStrings[GET_NODE_ALL_POSITIONS] =
        "SELECT simulationtime, x, y, z "
        "FROM nodelocation "
        "WHERE nodeid = ? AND simulationtime >= ? AND simulationtime <= ? "
        "ORDER BY simulationtime DESC, entryid DESC LIMIT ? ;";

    m_queryStmtsStrings[GET_LTE_UE_RSRP_RSRQ] = "SELECT rnti, cellid, rsrp, rsrq, serving, ccid "
                                                "FROM lteuersrprsrq "
                                                "WHERE nodeid = ? "
                                                "AND simulationtime IN ("
                                                "SELECT simulationtime "
                                                "FROM lteuersrprsrq "
                                                "WHERE nodeid = ? "
                                                "ORDER BY simulationtime DESC LIMIT 1"
                                                ");";

    m_queryStmtsStrings[INSERT_LTE_ENB_NODE] = "INSERT OR REPLACE INTO lteenb "
                                               "(nodeid, cellid) VALUES (?, ?);";

    m_queryStmtsStrings[INSERT_LTE_UE_CELL] =
        "INSERT INTO lteuecell "
        "(nodeid, cellid, rnti, simulationtime) VALUES (?, ?, ?, ?);";

    m_queryStmtsStrings[INSERT_LTE_UE_NODE] = "INSERT OR REPLACE INTO lteue "
                                              "(nodeid, imsi) VALUES (?, ?);";

    m_queryStmtsStrings[INSERT_NODE_ADD] = "INSERT INTO node "
                                           "(nodetype) VALUES (?);";

    m_queryStmtsStrings[INSERT_NODE_UPDATE] = "INSERT OR REPLACE INTO node "
                                              "(nodeid, nodetype) VALUES (?, ?);";

    m_queryStmtsStrings[INSERT_NODE_LOCATION] =
        "INSERT INTO nodelocation "
        "(nodeid, x, y, z, simulationtime) VALUES (?, ?, ?, ?, ?);";

    m_queryStmtsStrings[INSERT_NODE_REGISTRATION] =
        "INSERT INTO noderegistration "
        "(nodeid, registered, simulationtime) VALUES (?, ?, ?);";

    m_queryStmtsStrings[INSERT_LTE_UE_RSRP_RSRQ] =
        "INSERT INTO lteuersrprsrq "
        "(nodeid, simulationtime, rnti, cellid, rsrp, rsrq, serving, ccid) VALUES (?, ?, ?, ?, ?, "
        "?, ?, ?);";

    m_queryStmtsStrings[LOG_CMM_ACTION] =
        "INSERT INTO cmmaction "
        "(cmmname, simulationtime, description) VALUES (?, ?, ?);";

    m_queryStmtsStrings[LOG_E2TERMINATOR_COMMAND] =
        "INSERT INTO terminatorcommand "
        "(targetid, simulationtime, cmdname) VALUES (?, ?, ?);";

    m_queryStmtsStrings[LOG_LM_ACTION] = "INSERT INTO lmaction "
                                         "(lmname, simulationtime, description) VALUES (?, ?, ?);";

    m_queryStmtsStrings[LOG_LM_COMMAND] = "INSERT INTO lmcommand "
                                          "(lmname, simulationtime, cmdname) VALUES (?, ?, ?);";
}

void
OranDataRepositorySqlite::RunCreateStatement(std::string statement)
{
    NS_LOG_FUNCTION(this << statement);

    // Keep the return code in a separate variable to make it easier to debug
    // Otherwise, we could just run the sqlite3_step as the 2nd argument to the
    // CheckQueryReturnCode call
    int rc;
    sqlite3_stmt* stmt;

    sqlite3_prepare_v2(m_db, statement.c_str(), -1, &stmt, 0);
    rc = sqlite3_step(stmt);
    CheckQueryReturnCode(stmt, rc);
    sqlite3_finalize(stmt);
}

} // namespace ns3
