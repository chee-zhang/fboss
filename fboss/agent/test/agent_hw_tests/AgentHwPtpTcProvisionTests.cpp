/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/AsicUtils.h"
#include "fboss/agent/packet/PTPHeader.h"
#include "fboss/agent/packet/PktFactory.h"
#include "fboss/agent/test/AgentHwTest.h"
#include "fboss/agent/test/EcmpSetupHelper.h"
#include "fboss/agent/test/utils/ConfigUtils.h"
#include "fboss/agent/test/utils/PacketSnooper.h"
#include "fboss/agent/test/utils/PacketTestUtils.h"
#include "fboss/agent/test/utils/TrapPacketUtils.h"

#include <folly/IPAddress.h>

namespace {} // namespace

namespace facebook::fboss {

/*
 *  Test case to validate PTP TC on all ports.
 *  1. Select two ports, one ingress and one egress.
 *  2. Add Vlans and put ports in loopback mode (MAC), config egress port as
 * next hop.
 *  3. Inject PTP packet packet out of ingress port, packet received back on the
 * same port, RX timestamp applied. The packet is now forwarded out of egress
 * port, TX timestamp applied.
 *  4. Packet is received back on egress port, now forward the same to CPU,
 * capture the packet and check for CF field in the packet.
 *  5. Run for each port pairs.
 */
class AgentHwPtpTcProvisionTests
    : public AgentHwTest,
      public testing::WithParamInterface<cfg::PortSpeed> {
 protected:
  const std::string kdstIpPrefix = "2192::101:";
  const std::string knexthopMacPrefix = "aa:bb:cc:dd:ee:";
  const folly::IPAddressV6 kSrcIp = folly::IPAddressV6("2025::1");
  const folly::MacAddress kSrcMac = folly::MacAddress("aa:bb:cc:00:00:01");

  const cfg::PortSpeed portSpeed = GetParam();

  void SetUp() override {
    AgentHwTest::SetUp();
  }

  std::vector<ProductionFeature> getProductionFeaturesVerified()
      const override {
    return {
        ProductionFeature::PTP_TC,
        ProductionFeature::PTP_TC_PROVISIONING_TIME_HW_VALIDATION};
  }

  cfg::SwitchConfig initialConfig(
      const AgentEnsemble& ensemble) const override {
    auto l3Asics = ensemble.getSw()->getHwAsicTable()->getL3Asics();
    auto asic = checkSameAndGetAsic(l3Asics);
    auto config = utility::onePortPerInterfaceConfig(
        ensemble.getSw(),
        ensemble.masterLogicalPortIds(),
        true /*interfaceHasSubnet*/);
    config.switchSettings()->ptpTcEnable() = true;
    auto ports = ensemble.masterLogicalInterfacePortIds();
    // Port disabled if not support this speed
    utility::configurePortGroup(
        ensemble.getPlatformMapping(),
        ensemble.getSw()->getPlatformSupportsAddRemovePort(),
        config,
        portSpeed,
        ports);
    // Todo: We should use dst mac to avoid same packet trapped twice
    // on inject and dst ports. But Leaba 1.42.8 SDK does not support it yet.
    std::set<folly::CIDRNetwork> prefixs;
    for (int idx = 0; idx < ports.size(); idx++) {
      prefixs.emplace(getDstIp(idx), 128);
    }
    utility::addTrapPacketAcl(asic, &config, prefixs);
    return config;
  }

  int getPortIndexFromDstIp(folly::IPAddressV6 ip) {
    auto bytes = ip.toByteArray();
    return ((bytes[14] << 8) | bytes[15]) - 10;
  }

  folly::IPAddressV6 getDstIp(int portIndex) const {
    std::stringstream ss;
    auto offset = 10;
    ss << std::hex << portIndex + offset;
    return folly::IPAddressV6(kdstIpPrefix + ss.str());
  }

  int getPortIndexFromNexthopMac(folly::MacAddress mac) {
    auto lastByte = mac.bytes()[5];
    return lastByte - 10;
  }

  folly::MacAddress getNexthopMac(int portIndex) {
    std::stringstream ss;
    auto offset = 10;
    ss << std::hex << portIndex + offset;
    return folly::MacAddress(knexthopMacPrefix + ss.str());
  }

  std::string getPortName(const PortID& portId) const {
    for (const auto& portMap :
         std::as_const(*getSw()->getState()->getPorts())) {
      for (const auto& port : std::as_const(*portMap.second)) {
        if (port.second->getID() == portId) {
          return port.second->getName();
        }
      }
    }
    throw FbossError("No port with ID: ", portId);
  }

  void setupEcmpTraffic(
      const PortID& portID,
      const folly::MacAddress& nexthopMac,
      const folly::IPAddressV6& dstIp) {
    utility::EcmpSetupTargetedPorts6 ecmpHelper6{
        getProgrammedState(), getSw()->needL2EntryForNeighbor(), nexthopMac};
    auto dstPort = PortDescriptor(portID);
    applyNewState([&](const std::shared_ptr<SwitchState>& in) {
      auto newState = ecmpHelper6.resolveNextHops(in, {dstPort});
      return newState;
    });
    auto wrapper = this->getSw()->getRouteUpdater();
    ecmpHelper6.programRoutes(
        &wrapper, {dstPort}, {RoutePrefix<folly::IPAddressV6>{dstIp, 128}});
  }

  void sendPtpPkts(
      PTPMessageType ptpType,
      const PortID& injectPortID,
      const folly::IPAddressV6& dstIp,
      const PortID& dstPort) {
    XLOGF(
        DBG2,
        "Sending PTP packet from port {} to port {}",
        getPortName(injectPortID),
        getPortName(dstPort));
    auto portDescriptor = PortDescriptor(injectPortID);
    auto intfID = getSw()->getState()->getInterfaceIDForPort(portDescriptor);
    auto vlanID =
        getSw()->getState()->getInterfaces()->getNodeIf(intfID)->getVlanID();
    auto matcher =
        getSw()->getScopeResolver()->scope(getSw()->getState(), portDescriptor);
    auto scope = getSw()->getScopeResolver()->scope(
        getSw()->getState()->getVlans()->getNode(vlanID));
    auto dstMac = utility::getInterfaceMac(getSw()->getState(), intfID);
    // Send out PTP packet
    auto ptpPkt = makePtpPkt(getVlanIDForTx().value(), dstIp, dstMac, ptpType);
    getAgentEnsemble()->sendPacketAsync(
        std::move(ptpPkt), portDescriptor, std::nullopt);
  }

  void verifyPtpPkts(
      PTPMessageType ptpType,
      utility::SwSwitchPacketSnooper& snooper,
      const PortID& injectPortID,
      const PortID& dstPortID,
      const std::vector<PortID>& ports) {
    // expect two PTP packet, one trapped from inject port and one from dst port
    for (int i = 0; i < 2; i++) {
      auto pktBufOpt = snooper.waitForPacket(5);
      EXPECT_TRUE(pktBufOpt.has_value());
      auto pktBuf = *pktBufOpt.value().get();
      folly::io::Cursor pktCursor(&pktBuf);
      EXPECT_TRUE(utility::isPtpEventPacket(pktCursor));
      PTPHeader ptpHdr(&pktCursor);

      pktCursor.reset(&pktBuf);
      EthHdr ethHdr(pktCursor);
      auto srcMac = ethHdr.getSrcMac();
      auto dstMac = ethHdr.getDstMac();
      if (srcMac == kSrcMac) {
        // Continue if the packet trapped from inject port
        continue;
      }
      IPv6Hdr ipHdr(pktCursor);
      auto dstIp = ipHdr.dstAddr;
      // use dstMac and dstIP to find the dst port and inject port
      auto dstPortIdx = getPortIndexFromNexthopMac(dstMac);
      auto injectPortIdx = getPortIndexFromDstIp(dstIp);
      // Verify packet is received on the correct port
      EXPECT_EQ(dstPortIdx, (injectPortIdx + 1) % ports.size());
      EXPECT_EQ(ports[injectPortIdx], injectPortID);
      EXPECT_EQ(ports[dstPortIdx], dstPortID);
      // Verify PTP fields
      auto correctionField = ptpHdr.getCorrectionField();
      uint64_t cfInNsecs = (correctionField >> 16) & 0x0000ffffffffffff;
      XLOG(DBG2) << "PTP packet with CorrectionField (CF) set to " << cfInNsecs
                 << " for packet sent from "
                 << getPortName(ports[injectPortIdx])
                 << " and received on port " << getPortName(ports[dstPortIdx]);
      EXPECT_EQ(ptpType, ptpHdr.getPtpType());
      EXPECT_EQ(PTPVersion::PTP_V2, ptpHdr.getPtpVersion());
      EXPECT_EQ(PTP_DELAY_REQUEST_MSG_SIZE, ptpHdr.getPtpMessageLength());
      // Verify PTP correction field
      EXPECT_GT(cfInNsecs, 0);
      // Ideally we should have a upper bound threshold per ASIC.
      // For now, a generic value will be used as minor latency differences are
      // not a concern.
      EXPECT_LT(cfInNsecs, 8000);
    }
  }

  folly::MacAddress getIntfMac() const {
    return utility::getMacForFirstInterfaceWithPorts(getProgrammedState());
  }

 private:
  std::unique_ptr<facebook::fboss::TxPacket> makePtpPkt(
      const VlanID& vlanID,
      const folly::IPAddressV6& dstIp,
      const folly::MacAddress& dstMac,
      PTPMessageType ptpType) {
    return utility::makePTPTxPacket(
        getSw(),
        vlanID,
        kSrcMac,
        dstMac,
        kSrcIp,
        dstIp,
        0 /* dscp */,
        5, /* Avoid ttl 1/0 punt*/
        ptpType);
  }
};

// Each port will be selected as Ingress port once and Egress port once.
TEST_P(AgentHwPtpTcProvisionTests, VerifyPtpTcDelayRequest) {
  std::vector<PortID> ports;
  std::vector<folly::MacAddress> nexthopMacs; // binding to ingress port
  std::vector<folly::IPAddressV6> dstIps; // binding to egress port

  auto cfg = getAgentEnsemble()->getCurrentConfig();
  // Test all enabled ports
  for (auto& port : *cfg.ports()) {
    if (port.state() == cfg::PortState::ENABLED &&
        port.portType() == cfg::PortType::INTERFACE_PORT) {
      ports.emplace_back(*port.logicalID());
    }
  }
  for (int idx = 0; idx < ports.size(); idx++) {
    nexthopMacs.emplace_back(getNexthopMac(idx));
    dstIps.emplace_back(getDstIp(idx));
  }

  XLOG(DBG0) << "Test " << ports.size() << " Ports with speed set to "
             << static_cast<int>(portSpeed) << " kbps for PTP TC test";

  auto setup = [=, this]() {
    for (int idx = 0; idx < ports.size(); idx++) {
      auto dstIdx = (idx + 1) % ports.size();
      // For inject port (idx) IP, routed to dest port (dstIdx)
      setupEcmpTraffic(ports[dstIdx], nexthopMacs[dstIdx], dstIps[idx]);
    }
  };

  // Send packet using dstIps[ingressPortIdx]
  // The Eth header will be modified later to use nexthopMacs[egressPortIdx]
  // as dstMac once trapped from egress port
  auto verify = [=, this]() {
    utility::SwSwitchPacketSnooper snooper(getSw(), "snooper-ptp");

    for (int idx = 0; idx < ports.size(); idx++) {
      auto dstIdx = (idx + 1) % ports.size();
      sendPtpPkts(
          PTPMessageType::PTP_DELAY_REQUEST,
          ports[idx],
          dstIps[idx],
          ports[dstIdx]);

      verifyPtpPkts(
          PTPMessageType::PTP_DELAY_REQUEST,
          snooper,
          ports[idx],
          ports[dstIdx],
          ports);
    }
  };
  verifyAcrossWarmBoots(setup, verify);
}

INSTANTIATE_TEST_SUITE_P(
    test, // simple test_suite prefix to reduce name length
    AgentHwPtpTcProvisionTests,
    ::testing::Values(
        cfg::PortSpeed::TWENTYFIVEG,
        cfg::PortSpeed::FIFTYG,
        cfg::PortSpeed::HUNDREDG,
        cfg::PortSpeed::TWOHUNDREDG,
        cfg::PortSpeed::FOURHUNDREDG),
    [](const ::testing::TestParamInfo<cfg::PortSpeed>& info) {
      return apache::thrift::util::enumNameSafe(info.param);
    });

} // namespace facebook::fboss
