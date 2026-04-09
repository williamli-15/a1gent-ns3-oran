// oran-report-lte-ho-event.h
#pragma once
#include "oran-report.h"

namespace ns3 {
class OranReportLteHoEvent : public OranReport
{
public:
  static TypeId GetTypeId();
  OranReportLteHoEvent() = default;

  void     SetImsi(uint64_t v)    { m_imsi = v; }
  void     SetUeId(uint64_t v)    { m_ueid = v; }
  void     SetSrcCell(uint16_t v) { m_srcCell = v; }
  void     SetDstCell(uint16_t v) { m_dstCell = v; }
  void     SetEvent(std::string v){ m_event = std::move(v); }

  uint64_t GetImsi()   const { return m_imsi; }
  uint64_t GetUeId()   const { return m_ueid; }
  uint16_t GetSrcCell()const { return m_srcCell; }
  uint16_t GetDstCell()const { return m_dstCell; }
  const std::string& GetEvent() const { return m_event; }

private:
  uint64_t    m_imsi   = 0;
  uint64_t    m_ueid   = 0;
  uint16_t    m_srcCell= 0;
  uint16_t    m_dstCell= 0;
  std::string m_event;
};
} // ns3
