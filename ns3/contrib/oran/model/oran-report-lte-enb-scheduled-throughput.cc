#include "oran-report-lte-enb-scheduled-throughput.h"
#include <ns3/log.h>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("OranReportLteEnbScheduledThroughput");
NS_OBJECT_ENSURE_REGISTERED (OranReportLteEnbScheduledThroughput);

TypeId
OranReportLteEnbScheduledThroughput::GetTypeId (void)
{
  static TypeId tid =
    TypeId ("ns3::OranReportLteEnbScheduledThroughput")
      .SetParent<OranReport> ()
      .AddConstructor<OranReportLteEnbScheduledThroughput> ();
  return tid;
}

OranReportLteEnbScheduledThroughput::OranReportLteEnbScheduledThroughput ()
  : OranReport ()
{
  NS_LOG_FUNCTION (this);
}

} // namespace ns3
