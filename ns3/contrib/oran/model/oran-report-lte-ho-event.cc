// oran-report-lte-ho-event.cc
#include "oran-report-lte-ho-event.h"
#include <ns3/type-id.h>

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(OranReportLteHoEvent);
TypeId OranReportLteHoEvent::GetTypeId()
{
  static TypeId tid = TypeId("ns3::OranReportLteHoEvent")
    .SetParent<OranReport>()
    .AddConstructor<OranReportLteHoEvent>();
  return tid;
}
} // ns3
