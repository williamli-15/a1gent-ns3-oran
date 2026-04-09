// oran-report-lte-ue-sinr.cc
#include "oran-report-lte-ue-sinr.h"
#include "ns3/log.h"

namespace ns3 {
NS_LOG_COMPONENT_DEFINE ("OranReportLteUeSinr");
NS_OBJECT_ENSURE_REGISTERED (OranReportLteUeSinr);

TypeId
OranReportLteUeSinr::GetTypeId ()
{
  static TypeId tid = TypeId ("ns3::OranReportLteUeSinr")
    .SetParent<OranReport>()
    .AddConstructor<OranReportLteUeSinr>();   // <-- add this
  return tid;
}
OranReportLteUeSinr::OranReportLteUeSinr () = default;
OranReportLteUeSinr::~OranReportLteUeSinr () = default;
} // namespace ns3
