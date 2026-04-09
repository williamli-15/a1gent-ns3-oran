// oran-report-lte-ue-dwell.cc
#include "oran-report-lte-ue-dwell.h"
#include <ns3/type-id.h>
namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(OranReportLteUeDwell);
TypeId OranReportLteUeDwell::GetTypeId()
{
  static TypeId tid = TypeId("ns3::OranReportLteUeDwell")
    .SetParent<OranReport>()
    .AddConstructor<OranReportLteUeDwell>();
  return tid;
}
} // ns3
