#include "csh/sec_ctrl.hh"

#include "base/trace.hh"
#include "debug/SecCtrl.hh"
#include "sim/system.hh"

namespace gem5
{

SecCtrl::SecCtrl(const SecCtrlParams &p) :
    SimObject(p),
    readVerFinished([this]{ processReadVerFinished(); }, name()),
    sendMacWrite([this]{ processSendMacWrite(); }, name()),
    sendNextMtWrite([this]{ processSendNextMtWrite(); }, name()),
    writeVerFinished([this]{ processWriteVerFinished(); }, name()),
    cpuSidePort(name() + ".cpu_side_port", this),
    memPort(name() + ".mem_port", this),
    metaPort(name() + ".meta_port", this),
    state(Idle),
    chargeTime(0),
    verifiedPktAddr(0),
    verifiedCntOffs(0),
    flags(0), requestorId(0),
    needsResponse(true),
    cntBorder(0), macBorder(0), mtBorders{0},
    responsePkt(nullptr), counterPkt(nullptr), macPkt(nullptr),
    mtPkts{nullptr}
{
    DPRINTF(SecCtrl, "Constructing\n");

    // Calculate each space border
    cntBorder = DATA_SPACE;

    macBorder = cntBorder + DATA_SPACE / 64;

    for (uint8_t i=0; i<MT_LEVEL; i++) {
        mtBorders[i] = macBorder + DATA_SPACE / 4;
    }

    Addr node_space = NODE_SPACE;
    for (uint8_t i=0; i<MT_LEVEL-1; i++) {
        node_space *= 8;

        for (uint8_t j=0; j<=i; j++) {
            mtBorders[MT_LEVEL-1-j] += node_space;
        }
    }

}

bool
SecCtrl::CPUSidePort::sendPacket(PacketPtr pkt)
{
    panic_if(blockedPacket != nullptr, "Should never try to send if blocked!");

    // If we can't send the packet across the port, store it for later.
    if (sendTimingResp(pkt)) {
        DPRINTF(SecCtrl, "Sent the packet %s\n", pkt->print());

        return true;

    } else {
        DPRINTF(SecCtrl, "Failed to send the packet %s\n", pkt->print());
        blockedPacket = pkt;

        return false;

    }
}

void
SecCtrl::CPUSidePort::trySendRetryReq()
{
    if (needRetry && blockedPacket == nullptr) {
        DPRINTF(SecCtrl, "Sending retry req for %d\n", id);

        // Only send a retry if the port is now completely free
        needRetry = false;
        sendRetryReq();
    }
}

AddrRangeList
SecCtrl::CPUSidePort::getAddrRanges() const
{
    return ctrl->getAddrRanges();
}

void
SecCtrl::CPUSidePort::recvFunctional(PacketPtr pkt)
{
    ctrl->handleFunctional(pkt);
}

bool
SecCtrl::CPUSidePort::recvTimingReq(PacketPtr pkt)
{
    if (ctrl->state == Idle) {
        DPRINTF(SecCtrl, "Got request %s\n", pkt->print());

        ctrl->handleRequest(pkt);
        return true;
    } else {
        DPRINTF(SecCtrl, "Rejected request %s\n", pkt->print());

        needRetry = true;
        return false;
    }
}

void
SecCtrl::CPUSidePort::recvRespRetry()
{
    DPRINTF(SecCtrl, "Received response retry\n");

    // We should have a blocked packet if this function is called.
    assert(blockedPacket != nullptr);

    // Grab the blocked packet.
    PacketPtr pkt = blockedPacket;
    blockedPacket = nullptr;

    // Try to resend it. It's possible that it fails again.
    ctrl->handleResponse(pkt);
}

bool
SecCtrl::MemSidePort::sendPacket(PacketPtr pkt)
{
    panic_if(blockedPacket != nullptr, "Should never try to send if blocked!");

    // If we can't send the packet across the port, store it for later.
    if (sendTimingReq(pkt)) {
        DPRINTF(SecCtrl, "Sent the packet %s\n", pkt->print());

        return true;

    } else {
        DPRINTF(SecCtrl, "Failed to send the packet %s\n", pkt->print());
        blockedPacket = pkt;

        return false;

    }
}

bool
SecCtrl::MemSidePort::recvTimingResp(PacketPtr pkt)
{
    if (pkt->req->getAccessDepth() == 0) {
        DPRINTF(SecCtrl, "Cache hit, got response %s\n", pkt->print());
    } else {
        DPRINTF(SecCtrl, "Cache miss, got response %s\n", pkt->print());
    }

    ctrl->handleResponse(pkt);

    return true;
}

void
SecCtrl::MemSidePort::recvReqRetry()
{
    DPRINTF(SecCtrl, "Received request retry\n");

    // We should have a blocked packet if this function is called.
    assert(blockedPacket != nullptr);

    // Grab the blocked packet.
    PacketPtr pkt = blockedPacket;
    blockedPacket = nullptr;

    // Try to resend it. It's possible that it fails again.
    ctrl->handleRequest(pkt);
}

void
SecCtrl::MemSidePort::recvRangeChange()
{
    ctrl->handleRangeChange();
}

void
SecCtrl::processReadVerFinished()
{
    DPRINTF(SecCtrl, "Read verification is finished\n");

    if (cpuSidePort.sendPacket(responsePkt)) {
        state = Idle;
        responsePkt = nullptr;
        counterPkt = nullptr;
        macPkt = nullptr;
        for (uint8_t i=0; i<MT_LEVEL-1; i++) mtPkts[i] = nullptr;
        cpuSidePort.trySendRetryReq();
    } else {
        // Just wait cpuSidePort::recvReqRetry
        // for recalling this function
        ;
    }

}

void
SecCtrl::processSendMacWrite()
{
    sendMacPkt(false);
}

void
SecCtrl::processSendNextMtWrite()
{
    for (uint8_t i=0; i<MT_LEVEL-1; i++) {
        if (mtPkts[i] == nullptr) {
            sendMtPkt(i, false);

            return;
        }
    }

    assert(false);
}

void
SecCtrl::processWriteVerFinished()
{
    DPRINTF(SecCtrl, "Write verification is finished\n");

    if (needsResponse) {
        if (cpuSidePort.sendPacket(responsePkt)) {
            state = Idle;
            responsePkt = nullptr;
            counterPkt = nullptr;
            macPkt = nullptr;
            for (uint8_t i=0; i<MT_LEVEL-1; i++)
                mtPkts[i] = nullptr;
            cpuSidePort.trySendRetryReq();
        } else {
            // Just wait cpuSidePort::recvReqRetry
            // for recalling this function
            ;
        }
    } else {
        state = Idle;
        responsePkt = nullptr;
        counterPkt = nullptr;
        macPkt = nullptr;
        for (uint8_t i=0; i<MT_LEVEL-1; i++) mtPkts[i] = nullptr;
        cpuSidePort.trySendRetryReq();
    }

}

void
SecCtrl::updateChargeTime(Tick newChargeTime)
{
    if (chargeTime < newChargeTime) {
        chargeTime = newChargeTime;
    }
}

PacketPtr
SecCtrl::createMetaPkt(Addr addr, unsigned size, bool isRead)
{
    RequestPtr req(new Request(addr, size, flags, requestorId));

    // Select packet command
    MemCmd cmd = isRead ? MemCmd::ReadReq : MemCmd::WriteReq;

    PacketPtr retPkt = new Packet(req, cmd);

    uint8_t *reqData = new uint8_t[size]; // just empty here
    retPkt->dataDynamic(reqData);

    return retPkt;
}

bool
SecCtrl::sendCntPkt(bool isRead)
{
    // Approximation
    // Assume every block has a 8 bit counter

    Addr addr =
        cntBorder + (verifiedPktAddr >> 6);

    PacketPtr cntPkt = createMetaPkt(
            addr,
            1,
            isRead);

    return metaPort.sendPacket(cntPkt);
}

bool
SecCtrl::sendMacPkt(bool isRead)
{
    Addr addr =
        macBorder + (verifiedPktAddr >> 2);

    PacketPtr macPkt = createMetaPkt(
            addr >> 4 << 4,
            16,
            isRead);

    return metaPort.sendPacket(macPkt);
}

bool
SecCtrl::sendMtPkt(uint8_t nth, bool isRead)
{
    Addr addr =
        mtBorders[nth] + (verifiedCntOffs >> (nth+1)*3);

    PacketPtr mtPkt = createMetaPkt(
            isRead ? addr >> 6 << 6 : addr >> 3 << 3, // Alignment
            isRead ? 64 : 8,
            isRead);

    return metaPort.sendPacket(mtPkt);
}

void
SecCtrl::handleRequest(PacketPtr pkt)
{
    switch (state) {
        case Idle:
            // Store the information of the packet

            // Verified Counter Offset (BMT)
            verifiedPktAddr = pkt->getAddr();
            verifiedCntOffs = verifiedPktAddr >> 6;
            // Params of the packet
            flags = pkt->req->getFlags();
            requestorId = pkt->req->requestorId();
            // Whether the pkt needs response or not
            needsResponse = pkt->needsResponse();

            if (pkt->isRead()) {
                state = Read;

                // Coverable all failures
                // because they are different ports
                memPort.sendPacket(pkt);
                sendCntPkt(true);
                sendMacPkt(true);
                sendMtPkt(0, true);

                return;

            } else {
                state = Write;

                // Coverable of both failures
                // because they are different ports
                memPort.sendPacket(pkt);
                sendCntPkt(true);

                return;
            }

        /**
         * MemSidePort::recvReqRetry case
         */
        default:
            // Resend the packet

            // Check if the address is valid
            if (pkt->getAddr() == verifiedPktAddr) {
                memPort.sendPacket(pkt);

                return;

            } else if (pkt->getAddr() == cntBorder + verifiedCntOffs) {
                metaPort.sendPacket(pkt);

                return;

            } else if (pkt->getAddr() == macBorder + (verifiedCntOffs << 4)) {
                metaPort.sendPacket(pkt);

                return;
            }

            for (uint8_t i=0; i<MT_LEVEL; i++) {
                Addr validAddr =
                    mtBorders[i] + (verifiedCntOffs >> (i+1)*3);
                if (state == Read) {
                    validAddr = validAddr >> 6 << 6; // Alignment

                } else {
                    assert(state == Write);

                    validAddr = validAddr >> 3 << 3; // Alignment
                }

                if (pkt->getAddr() == validAddr) {
                    metaPort.sendPacket(pkt);

                    return;
                }
            }

            panic("Invalid blockedPkt in MemSidePort");

    }
}

void
SecCtrl::handleResponse(PacketPtr pkt)
{
    // Communicate packets
    switch (state) {
        case Idle:
            panic("Invalid state");

        case Read:
            if (pkt->getAddr() == verifiedPktAddr) {
                responsePkt = pkt;

            } else if (pkt->getAddr() == cntBorder + verifiedCntOffs) {
                counterPkt = pkt;

                updateChargeTime(curTick() + HASH_CYCLE * 1000);

            } else if (pkt->getAddr() == macBorder + (verifiedCntOffs << 4)) {
                macPkt = pkt;

            } else {
                for (uint8_t i=0; i<MT_LEVEL-2; i++) {
                    Addr validAddr =
                        mtBorders[i] + (verifiedCntOffs >> ((i+1)*3));
                    validAddr = validAddr >> 6 << 6; // Alignment

                    if (pkt->getAddr() == validAddr) {
                        mtPkts[i] = pkt;

                        updateChargeTime(curTick() + HASH_CYCLE * 1000);

                        if (pkt->req->getAccessDepth() == 0) {
                            // No need more nodes
                            break;

                        } else {
                            sendMtPkt(i+1, true);

                            break;
                        }
                    }
                }

                Addr validAddr =
                    mtBorders[MT_LEVEL-2] +
                    (verifiedCntOffs >> ((MT_LEVEL-1)*3));
                validAddr = validAddr >> 6 << 6; // Alignment

                if (pkt->getAddr() == validAddr) {
                    mtPkts[MT_LEVEL-2] = pkt;

                }
            }

            if (responsePkt != nullptr &&
                counterPkt != nullptr &&
                macPkt != nullptr) {

                updateChargeTime(curTick() + MAC_CYCLE * 1000);
            }

            break;

        case Write:
            if (pkt->getAddr() == verifiedPktAddr) {
                assert(needsResponse);

                responsePkt = pkt;

                updateChargeTime(curTick());

            } else if (pkt->getAddr() == cntBorder + verifiedCntOffs) {
                counterPkt = pkt;

                schedule(sendMacWrite, curTick() + MAC_CYCLE * 1000);
                schedule(sendNextMtWrite, curTick() + HASH_CYCLE * 1000);

            } else if (pkt->getAddr() == macBorder + (verifiedCntOffs << 4)) {
                macPkt = pkt;

                updateChargeTime(curTick());

            } else {
                if (pkt->isRead()) {
                    for (uint8_t i=0; i<MT_LEVEL-2; i++) {
                        Addr validAddr =
                            mtBorders[i] + (verifiedCntOffs >> ((i+1)*3));
                        validAddr = validAddr >> 6 << 6; // Alignment

                        if (pkt->getAddr() == validAddr) {
                            // Write should be done
                            assert(mtPkts[i] != nullptr);

                            schedule(sendNextMtWrite,
                                     curTick() + HASH_CYCLE * 1000);

                            return;
                        }

                    }

                    Addr validAddr =
                        mtBorders[MT_LEVEL-2] +
                        (verifiedCntOffs >> ((MT_LEVEL-1)*3));
                    validAddr = validAddr >> 6 << 6; // Alignment

                    updateChargeTime(curTick() + HASH_CYCLE * 1000);

                    panic_if(pkt->getAddr() != validAddr, "Invalid addr");

                } else {
                    for (uint8_t i=0; i<MT_LEVEL-1; i++) {
                        Addr validAddr =
                            mtBorders[i] + (verifiedCntOffs >> ((i+1)*3));
                        validAddr = validAddr >> 3 << 3; // Alignment

                        if (pkt->getAddr() == validAddr) {
                            mtPkts[i] = pkt;

                            if (pkt->req->getAccessDepth() == 0) {
                                // No need more nodes
                                updateChargeTime(curTick() + HASH_CYCLE * 1000);

                                break;

                            } else {
                                sendMtPkt(i, true);

                                return;
                            }
                        }
                    }
                }
            }

            break;

    }

    // Check Verification
    switch (state) {
        case Idle:
            panic("Invalid state");

        case Read:
            assert(needsResponse);

            if (responsePkt == nullptr ||
                counterPkt == nullptr ||
                macPkt == nullptr) {

                // Verification is not finished
                return;
            }

            for (uint8_t i=0; i<MT_LEVEL-1; i++) {
                if (mtPkts[i] == nullptr) {
                    // Verification is not finished
                    return;

                } else {
                    if (mtPkts[i]->req->getAccessDepth() == 0) {
                        // Verification is finished
                        break;
                    }
                }
            }

            // Sanity Check
            for (uint8_t i=0; i<MT_LEVEL-1; i++) {
                Addr validAddr =
                    mtBorders[i] + (verifiedCntOffs >> ((i+1)*3));
                validAddr = validAddr >> 6 << 6; // Alignment

                if (mtPkts[i]->getAddr() != validAddr) {
                    panic("mtNode's addr is not valid");
                }

                if (mtPkts[i]->req->getAccessDepth() == 0) {
                    break;
                }
            }

            // Verification is finished
            schedule(readVerFinished, chargeTime);

            return;

        case Write:
            if (needsResponse && responsePkt == nullptr) {
                // Verification is not finished
                return;
            }

            if (counterPkt == nullptr || macPkt == nullptr) {
                // Verification is not finished
                return;
            }

            for (uint8_t i=0; i<MT_LEVEL-1; i++) {
                if (mtPkts[i] == nullptr) {
                    // Verification is not finished
                    return;
                } else {
                    if (mtPkts[i]->req->getAccessDepth() == 0) {
                        // Verification is finished
                        break;
                    }
                }
            }

            // Sanity Check
            for (uint8_t i=0; i<MT_LEVEL-1; i++) {
                Addr validAddr =
                    mtBorders[i] + (verifiedCntOffs >> ((i+1)*3));
                validAddr = validAddr >> 3 << 3; // Alignment

                if (mtPkts[i]->getAddr() != validAddr) {
                    panic("mtNode's addr is not valid");
                }

                if (mtPkts[i]->req->getAccessDepth() == 0) {
                    break;
                }
            }

            // Verification is finished
            schedule(writeVerFinished, chargeTime);

            return;

    }

}

void
SecCtrl::handleFunctional(PacketPtr pkt)
{
    memPort.sendFunctional(pkt);
}

AddrRangeList
SecCtrl::getAddrRanges() const
{
    // Divide physical memory space for meta data
    DPRINTF(SecCtrl, "Sending new ranges\n");

    AddrRangeList addrRanges = memPort.getAddrRanges();
    panic_if(addrRanges.size() != 1, "Multiple addresses");

    AddrRange addrRange = addrRanges.front();
    panic_if(addrRange.interleaved(), "This address is interleaved");

    panic_if(addrRange.start() != 0, "Bad memory space");
    panic_if(addrRange.end() != mtBorders[MT_LEVEL-1],
            "Bad memory space");

    AddrRange dataAddrRange = AddrRange(
            0,
            cntBorder);

    DPRINTF(SecCtrl,
            "Original range is %s. New range is %s\n",
                addrRange.to_string(),
                dataAddrRange.to_string());

    return { dataAddrRange };
}

void
SecCtrl::handleRangeChange()
{
    cpuSidePort.sendRangeChange();
}

Port &
SecCtrl::getPort(const std::string &if_name, PortID idx)
{
    panic_if(idx != InvalidPortID, "This object doesn't support vector ports");

    // This is the name from the Python SimObject declaration (SecCtrl.py)
    if (if_name == "cpu_side_port") {
        return cpuSidePort;
    } else if (if_name == "mem_port") {
        return memPort;
    } else if (if_name == "meta_port") {
        return metaPort;
    } else {
        // pass it along to our super class
        return SimObject::getPort(if_name, idx);
    }
}

} // namespace gem5
