#include "oran-report-lte-enb-prb-utilization.h"
#include <ns3/log.h>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("OranReportLteEnbPrbUtilization");
NS_OBJECT_ENSURE_REGISTERED (OranReportLteEnbPrbUtilization);

TypeId
OranReportLteEnbPrbUtilization::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::OranReportLteEnbPrbUtilization")
    .SetParent<OranReport> ()
    .AddConstructor<OranReportLteEnbPrbUtilization> ();
  return tid;
}

OranReportLteEnbPrbUtilization::OranReportLteEnbPrbUtilization ()
  : OranReport ()
{
  // no-op
}

OranReportLteEnbPrbUtilization::~OranReportLteEnbPrbUtilization ()
{
  // no-op
}

} // namespace ns3
