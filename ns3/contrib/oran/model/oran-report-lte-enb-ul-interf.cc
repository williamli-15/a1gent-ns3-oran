#include "oran-report-lte-enb-ul-interf.h"
#include <ns3/uinteger.h>
#include <ns3/double.h>

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED(OranReportLteEnbUlInterf);

TypeId
OranReportLteEnbUlInterf::GetTypeId()
{
  static TypeId tid =
    TypeId("ns3::OranReportLteEnbUlInterf")
      .SetParent<OranReport>()
      .AddConstructor<OranReportLteEnbUlInterf>();
  return tid;
}

} // namespace ns3
