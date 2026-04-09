#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/lte-module.h"
#include "ns3/mobility-module.h"
#include "ns3/oran-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/oran-lm-command-bridge.h"
#include "ns3/lte-hex-grid-enb-topology-helper.h"
#include "ns3/cosine-antenna-model.h"

// custom reporters added under contrib/oran/model
#include "ns3/oran-reporter-lte-enb-prb-utilization.h"
#include "ns3/oran-reporter-lte-enb-scheduled-throughput.h"
#include "ns3/oran-reporter-lte-enb-mcs.h"
#include "ns3/oran-reporter-lte-enb-ul-interference.h"
#include "ns3/oran-reporter-lte-enb-ue-count.h"
#include "ns3/oran-reporter-lte-ue-pdcp-throughput.h"
#include "ns3/oran-reporter-lte-ue-prb.h"
#include "ns3/oran-reporter-lte-ho-and-dwell.h"
#include "ns3/oran-reporter-lte-ue-sinr.h"


#include "ns3/onoff-application.h"
#include "ns3/packet-sink.h"
#include "ns3/udp-client.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <functional>
#include <utility>
#include <vector>

using namespace ns3;

static std::string s_trafficTraceFile = "traffic-trace.tr";
static std::string s_positionTraceFile = "position-trace.tr";
static std::string s_handoverTraceFile = "handover-trace.tr";

// DB trace sink (optional)
static void
QueryRcSink(std::string query, std::string args, int rc)
{
  std::cout << Simulator::Now().GetSeconds()
            << " Query "
            << ((rc == SQLITE_OK || rc == SQLITE_DONE) ? "OK" : "ERROR")
            << "(" << rc << "): " << query;
  if (!args.empty())
  {
    std::cout << " (" << args << ")";
  }
  std::cout << std::endl;
}

// Function that will save the traces of RX'd packets
void
RxTrace(Ptr<const Packet> p, const Address& from, const Address& to)
{
    uint16_t ueId = (InetSocketAddress::ConvertFrom(to).GetPort() / 1000);

    std::ofstream rxOutFile(s_trafficTraceFile, std::ios_base::app);
    rxOutFile << Simulator::Now().GetSeconds() << " " << ueId << " RX " << p->GetSize()
              << std::endl;
}

// Function that will save the traces of TX'd packets
void
TxTrace(Ptr<const Packet> p, const Address& from, const Address& to)
{
    uint16_t ueId = (InetSocketAddress::ConvertFrom(to).GetPort() / 1000);

    std::ofstream rxOutFile(s_trafficTraceFile, std::ios_base::app);
    rxOutFile << Simulator::Now().GetSeconds() << " " << ueId << " TX " << p->GetSize()
              << std::endl;
}

// Trace each node's location
void
TracePositions(NodeContainer nodes)
{
    std::ofstream posOutFile(s_positionTraceFile, std::ios_base::app);

    posOutFile << Simulator::Now().GetSeconds();
    for (uint32_t i = 0; i < nodes.GetN(); i++)
    {
        Vector pos = nodes.Get(i)->GetObject<MobilityModel>()->GetPosition();
        posOutFile << " " << pos.x << " " << pos.y;
    }
    posOutFile << std::endl;

    Simulator::Schedule(Seconds(1), &TracePositions, nodes);
}

void
NotifyHandoverEndOkEnb(std::string context, uint64_t imsi, uint16_t cellid, uint16_t rnti)
{
    std::ofstream hoOutFile(s_handoverTraceFile, std::ios_base::app);
    hoOutFile << Simulator::Now().GetSeconds() << " " << imsi << " " << cellid << " " << rnti
              << std::endl;
}

// ---- Reversible motion for vehicular UEs ----
static void
ReverseVelocity(NodeContainer nodes, Time interval)
{
    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        auto cv = nodes.Get(i)->GetObject<ConstantVelocityMobilityModel>();
        if (!cv)
        {
            continue;
        }
        Vector v = cv->GetVelocity();
        cv->SetVelocity(Vector(-v.x, v.y, v.z));
    }
    Simulator::Schedule(interval, &ReverseVelocity, nodes, interval);
}

// ---- Best-two eNB indices by distance (for attach trick) ----
static std::pair<uint32_t, uint32_t>
BestTwoEnbIdx(const Vector& up, const NodeContainer& enbs)
{
    double bestD2 = std::numeric_limits<double>::max();
    double secondD2 = bestD2;
    uint32_t best = 0;
    uint32_t second = 0;
    for (uint32_t j = 0; j < enbs.GetN(); ++j)
    {
        auto mp = enbs.Get(j)->GetObject<ConstantPositionMobilityModel>();
        Vector ep = mp->GetPosition();
        double dx = up.x - ep.x;
        double dy = up.y - ep.y;
        double d2 = dx * dx + dy * dy;
        if (d2 < bestD2)
        {
            secondD2 = bestD2;
            second = best;
            bestD2 = d2;
            best = j;
        }
        else if (d2 < secondD2)
        {
            secondD2 = d2;
            second = j;
        }
    }
    return {best, second};
}

// ---- Random-variable string (exp/pareto) for OnOff bursts ----
static std::string
RvStr(const std::string& kind, double meanSec)
{
    std::ostringstream os;
    if (kind == "exp")
    {
        os << "ns3::ExponentialRandomVariable[Mean=" << meanSec << "]";
    }
    else
    {
        double a = 1.5;
        double scale = meanSec * (a - 1.0) / a;
        os << "ns3::ParetoRandomVariable[Shape=" << a << "|Scale=" << scale << "]";
    }
    return os.str();
}

// ---- Traffic builders (DL OnOff, UL UDP client, UL OnOff) ----
static void
AddDlOnOff(ApplicationContainer& ueAppsAll,
           ApplicationContainer& rhAppsAll,
           std::vector<std::vector<Ptr<Application>>>& ueAppsByUe,
           std::vector<std::vector<Ptr<Application>>>& rhAppsByUe,
           Ptr<Node> remoteHost,
           Ipv4Address dst,
           Ptr<Node> ueNode,
           uint16_t port,
           const std::string& rateStr,
           uint32_t pkt,
           bool burst,
           const std::string& onK,
           const std::string& offK,
           double onM,
           double offM,
           uint32_t ueIdx)
{
    PacketSinkHelper sink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
    auto sinkApps = sink.Install(ueNode);
    ueAppsAll.Add(sinkApps);
    auto sinkApp = sinkApps.Get(0);
    ueAppsByUe[ueIdx].push_back(sinkApp);
    sinkApp->TraceConnectWithoutContext("RxWithAddresses", MakeCallback(&RxTrace));

    Ptr<OnOffApplication> onoff = CreateObject<OnOffApplication>();
    onoff->SetAttribute("Remote", AddressValue(InetSocketAddress(dst, port)));
    onoff->SetAttribute("DataRate", DataRateValue(DataRate(rateStr)));
    onoff->SetAttribute("PacketSize", UintegerValue(pkt));
    onoff->SetAttribute("OnTime",
                        StringValue(burst ? RvStr(onK, onM)
                                          : "ns3::ConstantRandomVariable[Constant=1.0]"));
    onoff->SetAttribute("OffTime",
                        StringValue(burst ? RvStr(offK, offM)
                                          : "ns3::ConstantRandomVariable[Constant=0.0]"));
    remoteHost->AddApplication(onoff);
    rhAppsAll.Add(onoff);
    rhAppsByUe[ueIdx].push_back(onoff);
    onoff->TraceConnectWithoutContext("TxWithAddresses", MakeCallback(&TxTrace));
}

static void
AddUlUdpClient(ApplicationContainer& ueAppsAll,
               ApplicationContainer& rhAppsAll,
               std::vector<std::vector<Ptr<Application>>>& ueAppsByUe,
               std::vector<std::vector<Ptr<Application>>>& rhAppsByUe,
               Ptr<Node> ueNode,
               Ptr<Node> remoteHost,
               Ipv4Address dst,
               uint16_t port,
               uint32_t pkt,
               double periodMs,
               uint32_t ueIdx)
{
    PacketSinkHelper sink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
    auto rsink = sink.Install(remoteHost);
    rhAppsAll.Add(rsink);
    rhAppsByUe[ueIdx].push_back(rsink.Get(0));

    UdpClientHelper cli(dst, port);
    cli.SetAttribute("MaxPackets", UintegerValue(0));
    cli.SetAttribute("Interval", TimeValue(MilliSeconds(periodMs)));
    cli.SetAttribute("PacketSize", UintegerValue(pkt));
    auto clientApps = cli.Install(ueNode);
    ueAppsAll.Add(clientApps);
    ueAppsByUe[ueIdx].push_back(clientApps.Get(0));
}

static void
AddUlOnOff(ApplicationContainer& ueAppsAll,
           ApplicationContainer& rhAppsAll,
           std::vector<std::vector<Ptr<Application>>>& ueAppsByUe,
           std::vector<std::vector<Ptr<Application>>>& rhAppsByUe,
           Ptr<Node> ueNode,
           Ptr<Node> remoteHost,
           Ipv4Address dst,
           uint16_t port,
           const std::string& rateStr,
           uint32_t pkt,
           const std::string& onK,
           const std::string& offK,
           double onM,
           double offM,
           uint32_t ueIdx)
{
    PacketSinkHelper sink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
    auto rsink = sink.Install(remoteHost);
    rhAppsAll.Add(rsink);
    rhAppsByUe[ueIdx].push_back(rsink.Get(0));

    Ptr<OnOffApplication> onoff = CreateObject<OnOffApplication>();
    onoff->SetAttribute("Remote", AddressValue(InetSocketAddress(dst, port)));
    onoff->SetAttribute("DataRate", DataRateValue(DataRate(rateStr)));
    onoff->SetAttribute("PacketSize", UintegerValue(pkt));
    onoff->SetAttribute("OnTime", StringValue(RvStr(onK, onM)));
    onoff->SetAttribute("OffTime", StringValue(RvStr(offK, offM)));
    ueNode->AddApplication(onoff);
    ueAppsAll.Add(onoff);
    ueAppsByUe[ueIdx].push_back(onoff);
}


// ---- Emergency helpers (retune traffic without reinstalling apps) ----
static void
RetuneDlOnOffForUe(std::vector<std::vector<Ptr<Application>>>& rhAppsByUe,
                   uint32_t ueIdx,
                   const std::string& newRate,
                   double onMean,
                   double offMean,
                   bool burst = true)
{
    if (ueIdx >= rhAppsByUe.size())
    {
        return;
    }
    for (auto& app : rhAppsByUe[ueIdx])
    {
        Ptr<OnOffApplication> onoff = DynamicCast<OnOffApplication>(app);
        if (!onoff)
        {
            continue;
        }
        onoff->SetAttribute("DataRate", DataRateValue(DataRate(newRate)));
        if (burst)
        {
            onoff->SetAttribute("OnTime", StringValue(RvStr("pareto", onMean)));
            onoff->SetAttribute("OffTime", StringValue(RvStr("exp", offMean)));
        }
        else
        {
            onoff->SetAttribute("OnTime",
                                StringValue("ns3::ConstantRandomVariable[Constant=1.0]"));
            onoff->SetAttribute("OffTime",
                                StringValue("ns3::ConstantRandomVariable[Constant=0.0]"));
        }
        break;
    }
}

static void
RetuneUlUdpIntervalForUe(std::vector<std::vector<Ptr<Application>>>& ueAppsByUe,
                         uint32_t ueIdx,
                         double newPeriodMs)
{
    if (ueIdx >= ueAppsByUe.size())
    {
        return;
    }
    for (auto& app : ueAppsByUe[ueIdx])
    {
        Ptr<UdpClient> cli = DynamicCast<UdpClient>(app);
        if (!cli)
        {
            continue;
        }
        cli->SetAttribute("Interval", TimeValue(MilliSeconds(newPeriodMs)));
        break;
    }
}

static void
EnterEmergency(std::vector<std::vector<Ptr<Application>>>& rhAppsByUe,
               std::vector<std::vector<Ptr<Application>>>& ueAppsByUe,
               uint32_t numUes,
               uint32_t hotModulo,
               uint32_t hotOffset,
               const std::string& hotRate,
               double hotOn,
               double hotOff,
               bool boostUl)
{
    std::cout << Simulator::Now().GetSeconds() << "s ENTER-EMERGENCY" << std::endl;
    if (hotModulo == 0)
    {
        return;
    }
    for (uint32_t i = 0; i < numUes; ++i)
    {
        if (((i + hotOffset) % hotModulo) != 0)
        {
            continue;
        }
        RetuneDlOnOffForUe(rhAppsByUe, i, hotRate, hotOn, hotOff, true);
        if (boostUl)
        {
            RetuneUlUdpIntervalForUe(ueAppsByUe, i, 50.0); // tighter UL telemetry
        }
    }
}

static void
ExitEmergency(std::vector<std::vector<Ptr<Application>>>& rhAppsByUe,
              std::vector<std::vector<Ptr<Application>>>& ueAppsByUe,
              uint32_t numUes,
              const std::string& baseRate,
              double baseOn,
              double baseOff,
              bool boostUl)
{
    std::cout << Simulator::Now().GetSeconds() << "s EXIT-EMERGENCY" << std::endl;
    for (uint32_t i = 0; i < numUes; ++i)
    {
        RetuneDlOnOffForUe(rhAppsByUe, i, baseRate, baseOn, baseOff, true);
        if (boostUl)
        {
            RetuneUlUdpIntervalForUe(ueAppsByUe, i, 100.0); // restore baseline cadence
        }
    }
}

NS_LOG_COMPONENT_DEFINE("OranLte2LteMlHandoverExample");

int
main(int argc, char* argv[])
{

    // ---- KEEP-ALIVE handles (prevent refcount drop) ----
    Ptr<OranDataRepository> dataRepository;
    Ptr<OranCmm>            cmm;
    Ptr<OranNearRtRic>      nearRtRic;
    Ptr<OranNearRtRicE2Terminator> nearRtRicE2Terminator;
    Ptr<OranLm>             defaultLm;

    // optional: keep terminators alive explicitly (belt & suspenders)
    std::vector< Ptr<OranE2NodeTerminatorLteUe>  > ueTermsKeep;
    std::vector< Ptr<OranE2NodeTerminatorLteEnb> > enbTermsKeep;



    bool verbose = false;
    bool useOran = true;
    bool useOnnx = false;
    bool useTorch = false;
    bool useDistance = false;
    uint32_t startConfig = 1;
    double lmQueryInterval = 2;
    double txDelay = 0;
    // std::string handoverAlgorithm = "ns3::NoOpHandoverAlgorithm";
    std::string handoverAlgorithm = "ns3::A2A4RsrqHandoverAlgorithm";
    Time simTime = Seconds(300);
    std::string dbFileName = "oran-repository.db";

    bool useHex = true;              // turn on hex grid
    uint32_t numSites = 3;           // 3 sites => 9 cells
    double isd = 200.0;              // inter-site distance [m]
    uint32_t numUes = 20;            // number of UEs
    std::string embbBaseRate = "12Mbps";
    double embbBaseOnMean = 0.6;
    double embbBaseOffMean = 1.4;
    std::string embbHotRate = "28Mbps";
    double embbHotOnMean = 1.4;
    double embbHotOffMean = 0.25;
    uint32_t embbHotModulo = 3;
    uint32_t embbHotOffset = 0;
    double emergStart = 60.0;
    double emergDuration = 90.0;
    uint32_t emergHotModulo = 3;
    uint32_t emergHotOffset = 0;
    bool emergBoostUl = true;


    CommandLine cmd;
    cmd.AddValue("use-hex", "Use hex grid (3-sector macro sites)", useHex);
    cmd.AddValue("num-sites", "Number of macro sites (each 3 sectors)", numSites);
    cmd.AddValue("isd", "Inter-site distance (m)", isd);
    cmd.AddValue("num-ues", "Number of UEs", numUes);
    cmd.AddValue("embb-base-rate",
                 "Data rate for default eMBB DL bursts (e.g., 12Mbps)",
                 embbBaseRate);
    cmd.AddValue("embb-base-on",
                 "Mean ON-time (s) for default eMBB bursts",
                 embbBaseOnMean);
    cmd.AddValue("embb-base-off",
                 "Mean OFF-time (s) for default eMBB bursts",
                 embbBaseOffMean);
    cmd.AddValue("embb-hot-rate",
                 "Data rate for hotspot UEs (e.g., 22Mbps)",
                 embbHotRate);
    cmd.AddValue("embb-hot-on",
                 "Mean ON-time (s) for hotspot UEs",
                 embbHotOnMean);
    cmd.AddValue("embb-hot-off",
                 "Mean OFF-time (s) for hotspot UEs",
                 embbHotOffMean);
    cmd.AddValue("embb-hot-modulo",
                 "Every Nth UE (after offset) uses hotspot traffic (0 disables)",
                 embbHotModulo);
    cmd.AddValue("embb-hot-offset",
                 "Offset applied before modulo when selecting hotspot UEs",
                 embbHotOffset);
    cmd.AddValue("emerg-start", "Emergency start time (s)", emergStart);
    cmd.AddValue("emerg-dur", "Emergency duration (s)", emergDuration);
    cmd.AddValue("emerg-hot-modulo",
                 "Every Nth UE becomes hot during emergency (0 disables)",
                 emergHotModulo);
    cmd.AddValue("emerg-hot-offset",
                 "Offset before modulo when selecting emergency UEs",
                 emergHotOffset);
    cmd.AddValue("emerg-boost-ul",
                 "If true, tighten UL telemetry interval for emergency UEs",
                 emergBoostUl);
    cmd.AddValue("verbose", "Enable printing SQL queries results", verbose);
    cmd.AddValue("use-oran", "Indicates whether ORAN should be used or not", useOran);
    cmd.AddValue("use-onnx-lm", "Indicates whether the ONNX LM should be used or not", useOnnx);
    cmd.AddValue("use-torch-lm",
                 "Indicates whether the PyTorch LM should be used or not",
                 useTorch);
    cmd.AddValue("use-distance-lm",
                 "Indicates whether the distance LM should be used or not",
                 useDistance);
    cmd.AddValue("start-config", "The starting configuration", startConfig);
    cmd.AddValue("sim-time", "The duration for which traffic should flow", simTime);
    cmd.AddValue("lm-query-interval", "The LM query interval", lmQueryInterval);
    cmd.AddValue("tx-delay", "The E2 termiantor's transmission delay", txDelay);
    cmd.AddValue("handover-algorithm",
                 "Specify which handover algorithm to use",
                 handoverAlgorithm);
    cmd.AddValue("db-file", "Specify the DB file to create", dbFileName);
    cmd.AddValue("traffic-trace-file",
                 "Specify the traffic trace file to create",
                 s_trafficTraceFile);
    cmd.AddValue("position-trace-file",
                 "Specify the position trace file to create",
                 s_positionTraceFile);
    cmd.AddValue("handover-trace-file",
                 "Specify the handover trace file to create",
                 s_handoverTraceFile);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_IF(useOran == false && (useOnnx || useTorch || useDistance),
                    "Cannot use ML LM or distance LM without enabling O-RAN.");
    NS_ABORT_MSG_IF((useOnnx + useTorch + useDistance) > 1,
                    "Cannot use more than one LM simultaneously.");
    NS_ABORT_MSG_IF(handoverAlgorithm != "ns3::NoOpHandoverAlgorithm" &&
                        (useOnnx || useTorch || useDistance),
                    "Cannot use non-noop handover algorithm with ML LM or distance LM.");

    // Increase the buffer size to accomodate the application demand
    Config::SetDefault("ns3::LteRlcUm::MaxTxBufferSize", UintegerValue(1000 * 1024));
    // Disabled to prevent the automatic cell reselection when signal quality is bad.
    Config::SetDefault("ns3::LteUePhy::EnableRlfDetection", BooleanValue(false));
    // Give the macro eNBs more link budget so UEs can actually decode PDCP traffic.
    Config::SetDefault("ns3::LteEnbPhy::TxPower", DoubleValue(46.0));   // default 30 dBm
    Config::SetDefault("ns3::LteUePhy::TxPower", DoubleValue(23.0));

    // Configure the LTE parameters (pathloss, bandwidth, scheduler)
    Ptr<LteHelper> lteHelper = CreateObject<LteHelper>();
    lteHelper->SetAttribute("PathlossModel", StringValue("ns3::Cost231PropagationLossModel"));
    lteHelper->SetEnbDeviceAttribute("DlBandwidth", UintegerValue(25));
    lteHelper->SetEnbDeviceAttribute("UlBandwidth", UintegerValue(25));
    lteHelper->SetSchedulerType("ns3::PfFfMacScheduler");
    lteHelper->SetSchedulerAttribute("HarqEnabled", BooleanValue(true));
    lteHelper->SetHandoverAlgorithmType(handoverAlgorithm);

    // Set algorithm-specific attributes
    if (handoverAlgorithm == "ns3::A2A4RsrqHandoverAlgorithm")
    {
        lteHelper->SetHandoverAlgorithmAttribute("ServingCellThreshold", UintegerValue(28));
        lteHelper->SetHandoverAlgorithmAttribute("NeighbourCellOffset", UintegerValue(1));
    }
    else if (handoverAlgorithm == "ns3::A3RsrpHandoverAlgorithm")
    {
        // A3 uses Hysteresis and TimeToTrigger as main parameters
        lteHelper->SetHandoverAlgorithmAttribute("Hysteresis", DoubleValue(3.0));  // in dB, used to avoid ping-pong effect
        lteHelper->SetHandoverAlgorithmAttribute("TimeToTrigger", TimeValue(MilliSeconds(256)));
    }
    else if (handoverAlgorithm == "ns3::NoOpHandoverAlgorithm")
    {
        // No handover algorithm parameters are configured
    }

    // Deploy the EPC
    Ptr<PointToPointEpcHelper> epcHelper = CreateObject<PointToPointEpcHelper>();
    lteHelper->SetEpcHelper(epcHelper);

    Ptr<Node> pgw = epcHelper->GetPgwNode();

    // Create a single remote host
    NodeContainer remoteHostContainer;
    remoteHostContainer.Create(1);
    Ptr<Node> remoteHost = remoteHostContainer.Get(0);
    InternetStackHelper internet;
    internet.Install(remoteHostContainer);

    // IP configuration
    PointToPointHelper p2ph;
    p2ph.SetDeviceAttribute("DataRate", DataRateValue(DataRate("100Gb/s")));
    p2ph.SetDeviceAttribute("Mtu", UintegerValue(65000));
    p2ph.SetChannelAttribute("Delay", TimeValue(MilliSeconds(0)));
    NetDeviceContainer internetDevices = p2ph.Install(pgw, remoteHost);
    Ipv4AddressHelper ipv4h;
    ipv4h.SetBase("1.0.0.0", "255.0.0.0");
    Ipv4InterfaceContainer internetIpIfaces = ipv4h.Assign(internetDevices);

    Ipv4StaticRoutingHelper ipv4RoutingHelper;
    Ptr<Ipv4StaticRouting> remoteHostStaticRouting =
        ipv4RoutingHelper.GetStaticRouting(remoteHost->GetObject<Ipv4>());
    remoteHostStaticRouting->AddNetworkRouteTo(Ipv4Address("7.0.0.0"), Ipv4Mask("255.0.0.0"), 1);

    // Create eNB and UE
    NodeContainer ueNodes;
    NodeContainer enbNodes;

    NetDeviceContainer enbLteDevs;
    NetDeviceContainer ueLteDevs;

    Ipv4InterfaceContainer ueIpIface;  // shared across both branches

    if (useHex)
    {
        // 3-sector sites => 3 eNB nodes per site
        const uint32_t numEnb = numSites * 3;
        enbNodes.Create(numEnb);

        // Fixed eNBs
        MobilityHelper mobilityEnbs;
        mobilityEnbs.SetMobilityModel("ns3::ConstantPositionMobilityModel");
        mobilityEnbs.Install(enbNodes);

        // Lay out the sites as an even-row/odd-row hex grid
        Ptr<LteHexGridEnbTopologyHelper> hex = CreateObject<LteHexGridEnbTopologyHelper>();
        hex->SetLteHelper(lteHelper);
        hex->SetAttribute("InterSiteDistance", DoubleValue(isd));
        // For 7 sites, use 3 sites in even rows (odd rows get +1 -> 4) = 7 total
        hex->SetAttribute("GridWidth", UintegerValue(3));
        // Optional offsets to shift the grid origin if you want
        hex->SetAttribute("MinX", DoubleValue(0.0));
        hex->SetAttribute("MinY", DoubleValue(0.0));

        // give eNBs a directional antenna so Orientation works
        lteHelper->SetEnbAntennaModelType("ns3::CosineAntennaModel");

        // // NOTE: attribute name is "HorizontalBeamwidth" (not "Beamwidth")
        // lteHelper->SetEnbAntennaModelAttribute("HorizontalBeamwidth", DoubleValue(70.0));
        // // Optional (keeps EIRP sane; you can tune later)
        // lteHelper->SetEnbAntennaModelAttribute("MaxGain", DoubleValue(0.0));

        // now let the helper place the sectors; it will set Orientation per sector
        enbLteDevs = hex->SetPositionAndInstallEnbDevice(enbNodes);

        // UEs
        ueNodes.Create(numUes);

        // Let UEs roam a rectangle that comfortably covers the grid
        double gridWidth  = (3 + 0.5) * isd;                    // ~3.5*isd in X
        double gridHeight = 2.0 * std::sqrt(0.75) * isd * 2.0;  // ~3.46*isd in Y (two bi-rows)
        Rectangle bounds(-0.5*gridWidth, 1.5*gridWidth,
                        -0.5*gridHeight, 1.5*gridHeight);

        MobilityHelper mobilityUes;
        Ptr<RandomVariableStream> speedRvs =
            CreateObjectWithAttributes<UniformRandomVariable>("Min", DoubleValue(0.5),
                                                            "Max", DoubleValue(2.0));
        Ptr<RandomVariableStream> pauseRvs =
            CreateObjectWithAttributes<UniformRandomVariable>("Min", DoubleValue(0.5),
                                                            "Max", DoubleValue(2.0));
        mobilityUes.SetMobilityModel("ns3::RandomDirection2dMobilityModel",
                                    "Bounds", RectangleValue(bounds),
                                    "Speed",  PointerValue(speedRvs),
                                    "Pause",  PointerValue(pauseRvs));
        mobilityUes.Install(ueNodes);

        // Overlay a vehicular subset (every third UE) with reversible motion
        NodeContainer vehicularUes;
        MobilityHelper mobilityVehicular;
        mobilityVehicular.SetMobilityModel("ns3::ConstantVelocityMobilityModel");
        for (uint32_t i = 0; i < ueNodes.GetN(); ++i)
        {
            if (i % 3 != 0)
            {
                continue;
            }
            NodeContainer one;
            one.Add(ueNodes.Get(i));
            mobilityVehicular.Install(one);
            auto cv = ueNodes.Get(i)->GetObject<ConstantVelocityMobilityModel>();
            if (cv)
            {
                cv->SetVelocity(Vector(8.0, 0.0, 0.0));
            }
            vehicularUes.Add(ueNodes.Get(i));
        }
        if (vehicularUes.GetN() > 0)
        {
            Simulator::Schedule(Seconds(15), &ReverseVelocity, vehicularUes, Seconds(15));
        }

        // Devices
        ueLteDevs = lteHelper->InstallUeDevice(ueNodes);

        // IP stack & addresses for UEs
        // (reuse the 'internet' created above for the remote host)
        internet.Install(ueNodes);

        // assign into the shared ueIpIface we declared earlier
        ueIpIface = epcHelper->AssignUeIpv4Address(ueLteDevs);

        // reuse the ipv4RoutingHelper defined above (don’t re-declare)
        for (uint32_t u = 0; u < ueNodes.GetN(); ++u)
        {
            Ptr<Ipv4StaticRouting> ueStaticRouting =
                ipv4RoutingHelper.GetStaticRouting(ueNodes.Get(u)->GetObject<Ipv4>());
            ueStaticRouting->SetDefaultRoute(epcHelper->GetUeDefaultGatewayAddress(), 1);
        }

    }

    // Install and start applications on UEs and remote host (mixed traffic)
    const uint16_t basePort = 10000;
    const Ipv4Address remoteIp = internetIpIfaces.GetAddress(1);

    ApplicationContainer remoteAppsAll;
    ApplicationContainer ueAppsAll;
    std::vector<std::vector<Ptr<Application>>> remoteAppsByUe(ueNodes.GetN());
    std::vector<std::vector<Ptr<Application>>> ueAppsByUe(ueNodes.GetN());

    for (uint32_t i = 0; i < ueNodes.GetN(); ++i)
    {
        uint16_t port = basePort + i * 20;
        const bool isHotUe = (embbHotModulo > 0) &&
                             (((i + embbHotOffset) % embbHotModulo) == 0);
        const std::string ueDlRate = isHotUe ? embbHotRate : embbBaseRate;
        const double ueOnMean = isHotUe ? embbHotOnMean : embbBaseOnMean;
        const double ueOffMean = isHotUe ? embbHotOffMean : embbBaseOffMean;

        // eMBB DL bursts (baseline or hotspot profile)
        AddDlOnOff(ueAppsAll,
                   remoteAppsAll,
                   ueAppsByUe,
                   remoteAppsByUe,
                   remoteHost,
                   ueIpIface.GetAddress(i),
                   ueNodes.Get(i),
                   port++,
                   ueDlRate,
                   1400,
                   true,
                   "pareto",
                   "exp",
                   ueOnMean,
                   ueOffMean,
                   i);
        // if (i % 5 == 0)
        // {
        //     UdpClientHelper fb(remoteIp, port++);
        //     fb.SetAttribute("MaxPackets", UintegerValue(0));
        //     fb.SetAttribute("Interval", TimeValue(MilliSeconds(10)));  // 100 Hz
        //     fb.SetAttribute("PacketSize", UintegerValue(200));         // small packets
        //     auto fbApps = fb.Install(ueNodes.Get(i));
        //     ueAppsAll.Add(fbApps);
        //     ueAppsByUe[i].push_back(fbApps.Get(0));

        //     PacketSinkHelper fbSink("ns3::UdpSocketFactory", InetSocketAddress(remoteIp, port - 1));
        //     auto fbSinkApps = fbSink.Install(remoteHost);
        //     remoteAppsAll.Add(fbSinkApps);
        //     remoteAppsByUe[i].push_back(fbSinkApps.Get(0));
        // }
        // URLLC DL
        AddDlOnOff(ueAppsAll,
                   remoteAppsAll,
                   ueAppsByUe,
                   remoteAppsByUe,
                   remoteHost,
                   ueIpIface.GetAddress(i),
                   ueNodes.Get(i),
                   port++,
                   "2Mbps",
                   256,
                   true,
                   "exp",
                   "exp",
                   0.02,
                   0.02,
                   i);
        // V2X UL (UDP client)
        AddUlUdpClient(ueAppsAll,
                       remoteAppsAll,
                       ueAppsByUe,
                       remoteAppsByUe,
                       ueNodes.Get(i),
                       remoteHost,
                       remoteIp,
                       port++,
                       300,
                       100.0,
                       i);
        // mMTC UL bursts
        AddUlOnOff(ueAppsAll,
                   remoteAppsAll,
                   ueAppsByUe,
                   remoteAppsByUe,
                   ueNodes.Get(i),
                   remoteHost,
                   remoteIp,
                   port++,
                   "32kbps",
                   100,
                   "exp",
                   "exp",
                   0.1,
                   30.0,
                   i);
        if (i % 3 == 1)
        {
            AddUlOnOff(ueAppsAll,
                       remoteAppsAll,
                       ueAppsByUe,
                       remoteAppsByUe,
                       ueNodes.Get(i),
                       remoteHost,
                       remoteIp,
                       port++,
                       "4Mbps",
                       1200,
                       "pareto",
                       "exp",
                       0.6,
                       1.2,
                       i);
        }
    }

    // Stagger starts (sinks first, then sources)
    ueAppsAll.Start(Seconds(1.0));
    ueAppsAll.Stop(simTime + Seconds(15));
    remoteAppsAll.Start(Seconds(2.0));
    remoteAppsAll.Stop(simTime + Seconds(10));

    // Optional emergency window altering traffic demand
    if (emergHotModulo > 0 && emergDuration > 0.0)
    {
        Simulator::Schedule(Seconds(emergStart),
                            &EnterEmergency,
                            std::ref(remoteAppsByUe),
                            std::ref(ueAppsByUe),
                            numUes,
                            emergHotModulo,
                            emergHotOffset,
                            embbHotRate,
                            embbHotOnMean,
                            embbHotOffMean,
                            emergBoostUl);

        Simulator::Schedule(Seconds(emergStart + emergDuration),
                            &ExitEmergency,
                            std::ref(remoteAppsByUe),
                            std::ref(ueAppsByUe),
                            numUes,
                            embbBaseRate,
                            embbBaseOnMean,
                            embbBaseOffMean,
                            emergBoostUl);
    }


    // Initial UE attach: 2/3 nearest eNB, 1/3 second-nearest to create traffic skew
    for (uint32_t i = 0; i < ueNodes.GetN(); ++i)
    {
        Vector up = ueNodes.Get(i)->GetObject<MobilityModel>()->GetPosition();
        auto two = BestTwoEnbIdx(up, enbNodes);
        uint32_t best = two.first;
        uint32_t second = two.second;
        uint32_t choice = best;
        if (enbNodes.GetN() >= 2 && i % 3 == 0)
        {
            choice = second;
        }
        lteHelper->Attach(ueLteDevs.Get(i), enbLteDevs.Get(choice));
    }

    // ORAN BEGIN
    if (useOran == true)
    {
        if (!dbFileName.empty())
        {
            std::remove(dbFileName.c_str());
        }

        // --- RIC core objects ---
        dataRepository = CreateObject<OranDataRepositorySqlite>();
        cmm            = CreateObject<OranCmmHandover>();
        nearRtRic      = CreateObject<OranNearRtRic>();
        nearRtRicE2Terminator = CreateObject<OranNearRtRicE2Terminator>();

        // --- Use LM_CommandBridge instead of ONNX/Torch/Distance LMs ---
        defaultLm      = CreateObject<OranLmCommandBridge>();


        // Point the repo to the DB file we’re using in this run
        dataRepository->SetAttribute("DatabaseFile", StringValue(dbFileName));

        // Bridge settings
        defaultLm->SetAttribute("Verbose", BooleanValue(verbose));
        defaultLm->SetAttribute("NearRtRic", PointerValue(nearRtRic));
        // Make sure this path matches your Python orchestrator’s COMMANDS_PATH
        defaultLm->SetAttribute("CommandsPath",
            StringValue("commands.json"));
        // Apply CIO inline in the bridge; HO is emitted as OranCommand
        defaultLm->SetAttribute("ApplyCioInline", BooleanValue(true));
        // Ignore stale command files older than this simulated time
        defaultLm->SetAttribute("IgnoreOlderThan", TimeValue(Seconds(120)));

        // CMM & E2 terminator wiring
        cmm->SetAttribute("NearRtRic", PointerValue(nearRtRic));

        nearRtRicE2Terminator->SetAttribute("NearRtRic", PointerValue(nearRtRic));
        nearRtRicE2Terminator->SetAttribute("DataRepository", PointerValue(dataRepository));
        nearRtRicE2Terminator->SetAttribute(
            "TransmissionDelayRv",
            StringValue("ns3::ConstantRandomVariable[Constant=" + std::to_string(txDelay) + "]"));

        // Final RIC plumbing
        nearRtRic->SetAttribute("DefaultLogicModule", PointerValue(defaultLm));
        nearRtRic->SetAttribute("E2Terminator", PointerValue(nearRtRicE2Terminator));
        nearRtRic->SetAttribute("DataRepository", PointerValue(dataRepository));
        // How often the LM is polled (bridge reads commands.json on this cadence)
        nearRtRic->SetAttribute("LmQueryInterval", TimeValue(Seconds(lmQueryInterval)));
        nearRtRic->SetAttribute("ConflictMitigationModule", PointerValue(cmm));

        // DB logging to the terminal (toggle with --verbose=1)
        if (verbose)
        {
            nearRtRic->Data()->TraceConnectWithoutContext("QueryRc", MakeCallback(&QueryRcSink));
        }

        Simulator::ScheduleNow(&OranNearRtRic::Start, nearRtRic);

        for (uint32_t idx = 0; idx < ueNodes.GetN(); idx++)
        {
            Ptr<OranReporterLocation> locationReporter = CreateObject<OranReporterLocation>();
            Ptr<OranReporterLteUeCellInfo> lteUeCellInfoReporter = CreateObject<OranReporterLteUeCellInfo>();
            Ptr<OranReporterAppLoss> appLossReporter = CreateObject<OranReporterAppLoss>();
            Ptr<OranE2NodeTerminatorLteUe> lteUeTerminator = CreateObject<OranE2NodeTerminatorLteUe>();
            ueTermsKeep.push_back(lteUeTerminator);
            // RSRP/RSRQ reporter
            Ptr<OranReporterLteUeRsrpRsrq> rsrpRsrqReporter = CreateObject<OranReporterLteUeRsrpRsrq>();

            // Hook reporters to the same UE terminator
            locationReporter->SetAttribute("Terminator", PointerValue(lteUeTerminator));
            lteUeCellInfoReporter->SetAttribute("Terminator", PointerValue(lteUeTerminator));
            appLossReporter->SetAttribute("Terminator", PointerValue(lteUeTerminator));
            rsrpRsrqReporter->SetAttribute("Terminator", PointerValue(lteUeTerminator)); // NEW

            for (auto& app : remoteAppsByUe[idx])
            {
                if (!app)
                {
                    continue;
                }
                if (DynamicCast<OnOffApplication>(app) || DynamicCast<UdpClient>(app))
                {
                    app->TraceConnectWithoutContext("TxWithAddresses",
                        MakeCallback(&ns3::OranReporterAppLoss::AddTxWithAddresses, appLossReporter));
                }
                else if (DynamicCast<PacketSink>(app))
                {
                    app->TraceConnectWithoutContext("RxWithAddresses",
                        MakeCallback(&ns3::OranReporterAppLoss::AddRxWithAddresses, appLossReporter));
                }
            }
            for (auto& app : ueAppsByUe[idx])
            {
                if (!app)
                {
                    continue;
                }
                if (DynamicCast<OnOffApplication>(app) || DynamicCast<UdpClient>(app))
                {
                    app->TraceConnectWithoutContext("TxWithAddresses",
                        MakeCallback(&ns3::OranReporterAppLoss::AddTxWithAddresses, appLossReporter));
                }
                else if (DynamicCast<PacketSink>(app))
                {
                    app->TraceConnectWithoutContext("RxWithAddresses",
                        MakeCallback(&ns3::OranReporterAppLoss::AddRxWithAddresses, appLossReporter));
                }
            }

            // connect UE PHY measurement trace to the RSRP/RSRQ reporter
            for (uint32_t netDevIdx = 0; netDevIdx < ueNodes.Get(idx)->GetNDevices(); netDevIdx++)
            {
                Ptr<LteUeNetDevice> lteUeDevice =
                    ueNodes.Get(idx)->GetDevice(netDevIdx)->GetObject<LteUeNetDevice>();
                if (lteUeDevice)
                {
                    Ptr<LteUePhy> uePhy = lteUeDevice->GetPhy();
                    // This callback makes the reporter write into lteuersrprsrq (serving & neighbor rows)
                    uePhy->TraceConnectWithoutContext(
                        "ReportUeMeasurements",
                        MakeCallback(&ns3::OranReporterLteUeRsrpRsrq::ReportRsrpRsrq,
                                    rsrpRsrqReporter));
                }
            }

            // RIC / Terminator plumbing (existing)
            lteUeTerminator->SetAttribute("NearRtRic", PointerValue(nearRtRic));
            lteUeTerminator->SetAttribute("RegistrationIntervalRv",
                                          StringValue("ns3::ConstantRandomVariable[Constant=1]"));
            lteUeTerminator->SetAttribute("SendIntervalRv",
                                          StringValue("ns3::ConstantRandomVariable[Constant=2]")); // used to be 1

            lteUeTerminator->AddReporter(locationReporter);
            lteUeTerminator->AddReporter(lteUeCellInfoReporter);
            lteUeTerminator->AddReporter(appLossReporter);
            lteUeTerminator->AddReporter(rsrpRsrqReporter); // NEW

            lteUeTerminator->SetAttribute("TransmissionDelayRv",
                                          StringValue("ns3::ConstantRandomVariable[Constant=" +
                                                      std::to_string(txDelay) + "]"));

            lteUeTerminator->Attach(ueNodes.Get(idx));


            // ---- UE SINR reporter ----
            {
                TypeId tid;
                if (TypeId::LookupByNameFailSafe("ns3::OranReporterLteUeSinr", &tid))
                {
                    Ptr<OranReporterLteUeSinr> sinrRpt = CreateObject<OranReporterLteUeSinr>();
                    Ptr<OranReportTriggerPeriodic> sinrTrig = CreateObject<OranReportTriggerPeriodic>();
                    sinrTrig->SetAttribute("IntervalRv",
                        StringValue("ns3::ConstantRandomVariable[Constant=1.0]"));

                    sinrRpt->SetAttribute("Terminator", PointerValue(lteUeTerminator));
                    sinrRpt->SetAttribute("Trigger",    PointerValue(sinrTrig));
                    lteUeTerminator->AddReporter(sinrRpt);
                }
                else
                {
                    NS_LOG_WARN("OranReporterLteUeSinr not found—skipping UE SINR reporter");
                }
            }
            // ---- end UE SINR ----

            Simulator::ScheduleNow(&OranE2NodeTerminatorLteUe::Activate, lteUeTerminator);
        }

        Ptr<OranE2NodeTerminator> globalHostForUeRpt = nullptr;

        for (uint32_t idx = 0; idx < enbNodes.GetN(); idx++)
        {
            Ptr<OranReporterLocation> locationReporter = CreateObject<OranReporterLocation>();
            Ptr<OranE2NodeTerminatorLteEnb> lteEnbTerminator =
                CreateObject<OranE2NodeTerminatorLteEnb>();
            enbTermsKeep.push_back(lteEnbTerminator);

            locationReporter->SetAttribute("Terminator", PointerValue(lteEnbTerminator));

            lteEnbTerminator->SetAttribute("NearRtRic", PointerValue(nearRtRic));
            lteEnbTerminator->SetAttribute("RegistrationIntervalRv",
                                           StringValue("ns3::ConstantRandomVariable[Constant=1]"));
            lteEnbTerminator->SetAttribute("SendIntervalRv",
                                           StringValue("ns3::ConstantRandomVariable[Constant=2]")); // used to be 1

            lteEnbTerminator->AddReporter(locationReporter);

            // Keep the first eNB terminator to host global UE reporters later
            if (!globalHostForUeRpt) { globalHostForUeRpt = lteEnbTerminator; }

            // ---- eNB PRB / throughput / MCS / UL interference / UE count (guarded) ----
            {
                auto MakePeriodic = []() {
                    Ptr<OranReportTriggerPeriodic> t = CreateObject<OranReportTriggerPeriodic>();
                    t->SetAttribute("IntervalRv",
                        StringValue("ns3::ConstantRandomVariable[Constant=1.0]"));
                    return t;
                };
                TypeId tid;

                // PRB utilization
                if (TypeId::LookupByNameFailSafe("ns3::OranReporterLteEnbPrbUtilization", &tid))
                {
                    Ptr<OranReporterLteEnbPrbUtilization> prb = CreateObject<OranReporterLteEnbPrbUtilization>();
                    prb->SetAttribute("Terminator", PointerValue(lteEnbTerminator));
                    prb->SetAttribute("Trigger",    PointerValue(MakePeriodic()));
                    lteEnbTerminator->AddReporter(prb);
                }

                // Scheduled throughput
                if (TypeId::LookupByNameFailSafe("ns3::OranReporterLteEnbScheduledThroughput", &tid))
                {
                    Ptr<OranReporterLteEnbScheduledThroughput> tp = CreateObject<OranReporterLteEnbScheduledThroughput>();
                    tp->SetAttribute("Terminator", PointerValue(lteEnbTerminator));
                    tp->SetAttribute("Trigger",    PointerValue(MakePeriodic()));
                    lteEnbTerminator->AddReporter(tp);
                }

                // MCS
                if (TypeId::LookupByNameFailSafe("ns3::OranReporterLteEnbMcs", &tid))
                {
                    Ptr<OranReporterLteEnbMcs> mcs = CreateObject<OranReporterLteEnbMcs>();
                    mcs->SetAttribute("Terminator", PointerValue(lteEnbTerminator));
                    mcs->SetAttribute("Trigger",    PointerValue(MakePeriodic()));
                    mcs->SetAttribute("NearRtRic",  PointerValue(nearRtRic));  // <-- important
                    lteEnbTerminator->AddReporter(mcs);
                }

                // UL wideband interference
                if (TypeId::LookupByNameFailSafe("ns3::OranReporterLteEnbUlInterference", &tid))
                {
                    Ptr<OranReporterLteEnbUlInterference> ulinf = CreateObject<OranReporterLteEnbUlInterference>();
                    ulinf->SetAttribute("Terminator", PointerValue(lteEnbTerminator));
                    ulinf->SetAttribute("Trigger",    PointerValue(MakePeriodic()));
                    ulinf->SetAttribute("NearRtRic", PointerValue(nearRtRic));  // <-- add
                    lteEnbTerminator->AddReporter(ulinf);
                }

                // UE count
                if (TypeId::LookupByNameFailSafe("ns3::OranReporterLteEnbUeCount", &tid))
                {
                    Ptr<OranReporterLteEnbUeCount> uec = CreateObject<OranReporterLteEnbUeCount>();
                    uec->SetAttribute("Terminator", PointerValue(lteEnbTerminator));
                    uec->SetAttribute("Trigger",    PointerValue(MakePeriodic()));
                    uec->SetAttribute("NearRtRic", PointerValue(nearRtRic));  // <-- add
                    lteEnbTerminator->AddReporter(uec);
                }
            }
            // ---- end eNB reporters ----


            lteEnbTerminator->Attach(enbNodes.Get(idx));
            lteEnbTerminator->SetAttribute("TransmissionDelayRv",
                                           StringValue("ns3::ConstantRandomVariable[Constant=" +
                                                       std::to_string(txDelay) + "]"));
            Simulator::ScheduleNow(&OranE2NodeTerminatorLteEnb::Activate,
                                lteEnbTerminator);
        }

        // ---- Global UE reporters (hosted on one eNB terminator), guarded ----
        if (globalHostForUeRpt)
        {
            auto MakePeriodic = []() {
                Ptr<OranReportTriggerPeriodic> t = CreateObject<OranReportTriggerPeriodic>();
                t->SetAttribute("IntervalRv",
                    StringValue("ns3::ConstantRandomVariable[Constant=1.0]"));
                return t;
            };
            TypeId tid;

            // UE PDCP throughput
            if (TypeId::LookupByNameFailSafe("ns3::OranReporterLteUePdcpThroughput", &tid))
            {
                Ptr<OranReporterLteUePdcpThroughput> pdcp = CreateObject<OranReporterLteUePdcpThroughput>();
                pdcp->SetAttribute("Terminator", PointerValue(globalHostForUeRpt));
                pdcp->SetAttribute("Trigger",    PointerValue(MakePeriodic()));
                pdcp->SetAttribute("NearRtRic", PointerValue(nearRtRic));   // <<< REQUIRED
                globalHostForUeRpt->AddReporter(pdcp);
            }

            // UE PRB
            if (TypeId::LookupByNameFailSafe("ns3::OranReporterLteUePrb", &tid))
            {
                Ptr<OranReporterLteUePrb> uePrb = CreateObject<OranReporterLteUePrb>();
                uePrb->SetAttribute("Terminator", PointerValue(globalHostForUeRpt));
                uePrb->SetAttribute("Trigger",    PointerValue(MakePeriodic()));
                uePrb->SetAttribute("NearRtRic", PointerValue(nearRtRic));   // <<< REQUIRED
                globalHostForUeRpt->AddReporter(uePrb);
            }

            // HO events & dwell time
            if (TypeId::LookupByNameFailSafe("ns3::OranReporterLteHoAndDwell", &tid))
            {
                Ptr<OranReporterLteHoAndDwell> ho = CreateObject<OranReporterLteHoAndDwell>();
                ho->SetAttribute("Terminator", PointerValue(globalHostForUeRpt));
                ho->SetAttribute("Trigger",    PointerValue(MakePeriodic()));
                ho->SetAttribute("NearRtRic", PointerValue(nearRtRic));  // <-- add
                globalHostForUeRpt->AddReporter(ho);
            }
        }
        // ---- end global UE reporters ----
    }
    // ORAN END

    // X2 between all eNBs
    lteHelper->AddX2Interface(enbNodes);

    // Erase the trace files if they exist
    std::ofstream trafficOutFile(s_trafficTraceFile, std::ios_base::trunc);
    trafficOutFile.close();
    std::ofstream posOutFile(s_positionTraceFile, std::ios_base::trunc);
    posOutFile.close();
    std::ofstream hoOutFile(s_handoverTraceFile, std::ios_base::trunc);
    hoOutFile.close();

    // Start tracing node locations
    Simulator::Schedule(Seconds(1), &TracePositions, ueNodes);

    // Connect to handover trace so we know when a handover is successfully performed
    Config::Connect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverEndOk",
                    MakeCallback(&NotifyHandoverEndOkEnb));

    // Tell the simulator how long to run
    Simulator::Stop(simTime + Seconds(20));
    // Run the simulation
    Simulator::Run();
    // Clean up used resources
    Simulator::Destroy();

    return 0;
}
