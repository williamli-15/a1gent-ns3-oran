// oran-report-lte-ue-prb.cc
#include "oran-report-lte-ue-prb.h"
#include "ns3/type-id.h"

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(OranReportLteUePrb);

TypeId OranReportLteUePrb::GetTypeId()
{
  static TypeId tid = TypeId("ns3::OranReportLteUePrb")
      .SetParent<OranReport>()
      .AddConstructor<OranReportLteUePrb>();
  return tid;
}

} // ns3
