#include "oran-report-lte-ue-pdcp-throughput.h"
#include <ns3/type-id.h>
#include <ns3/log.h>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("OranReportLteUePdcpThroughput");
NS_OBJECT_ENSURE_REGISTERED (OranReportLteUePdcpThroughput);

TypeId
OranReportLteUePdcpThroughput::GetTypeId ()
{
  static TypeId tid = TypeId ("ns3::OranReportLteUePdcpThroughput")
    .SetParent<OranReport> ()
    .AddConstructor<OranReportLteUePdcpThroughput> ();
  return tid;
}

OranReportLteUePdcpThroughput::OranReportLteUePdcpThroughput ()
  : OranReport ()
{
  NS_LOG_FUNCTION (this);
}

} // namespace ns3
