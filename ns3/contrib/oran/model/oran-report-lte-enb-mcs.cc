#include "oran-report-lte-enb-mcs.h"
#include <ns3/log.h>
#include <sstream>

namespace ns3 {
NS_LOG_COMPONENT_DEFINE("OranReportLteEnbMcs");
NS_OBJECT_ENSURE_REGISTERED(OranReportLteEnbMcs);

TypeId
OranReportLteEnbMcs::GetTypeId()
{
  static TypeId tid = TypeId("ns3::OranReportLteEnbMcs")
    .SetParent<OranReport>()
    .AddConstructor<OranReportLteEnbMcs>();
  return tid;
}
OranReportLteEnbMcs::OranReportLteEnbMcs() : OranReport() {}

std::string OranReportLteEnbMcs::ToJson() const
{
  std::ostringstream os;
  os << "{"
     << "\"e2nodeid\":" << GetReporterE2NodeId() << ","
     << "\"simTime\":"  << GetTime().GetSeconds() << ","
     << "\"cellid\":"   << m_cellId << ","
     << "\"dl_mean\":"  << m_dlMean << ","
     << "\"dl_p50\":"   << m_dlP50  << ","
     << "\"dl_p95\":"   << m_dlP95  << ","
     << "\"ul_mean\":"  << m_ulMean << ","
     << "\"ul_p50\":"   << m_ulP50  << ","
     << "\"ul_p95\":"   << m_ulP95
     << "}";
  return os.str();
}

} // namespace ns3
