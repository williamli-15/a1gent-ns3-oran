// oran-report-lte-enb-ue-count.cc
#include "oran-report-lte-enb-ue-count.h"
#include <ns3/type-id.h>

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(OranReportLteEnbUeCount);
TypeId OranReportLteEnbUeCount::GetTypeId()
{
  static TypeId tid = TypeId("ns3::OranReportLteEnbUeCount")
    .SetParent<OranReport>()
    .AddConstructor<OranReportLteEnbUeCount>();
  return tid;
}
}
