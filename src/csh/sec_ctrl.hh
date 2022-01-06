#ifndef __CSH_SEC_CTRL_HH__
#define __CSH_SEC_CTRL_HH__

#include "mem/port.hh"
#include "mem/request.hh"
#include "params/SecCtrl.hh"
#include "sim/sim_object.hh"

#define DATA_SPACE 0x200000000
#define NODE_SPACE 0x40
#define MT_LEVEL 7 // Actually, Level 8
#define MAC_CYCLE 80
#define HASH_CYCLE 80

namespace gem5
{

class SecCtrl : public SimObject
{
  private:

    enum State
    {
        Idle,
        Read,
        Write
    };

    /**
     * Port on the CPU-side that receives requests.
     */
    class CPUSidePort : public ResponsePort
    {
      private:
        /// The ctrl that owns this object (SecCtrl)
        SecCtrl *ctrl;

        /// True if the port needs to send a retry req.
        bool needRetry;

        /// If we tried to send a packet and it was blocked, store it here
        PacketPtr blockedPacket;

      public:
        /**
         * Constructor. Just calls the superclass constructor.
         */
        CPUSidePort(const std::string& name, SecCtrl *_ctrl) :
            ResponsePort(name, _ctrl),
            ctrl(_ctrl),
            needRetry(false),
            blockedPacket(nullptr)
        {}

        /**
         * Send a packet across this port. This is called by the owner and
         * all of the flow control is hanled in this function.
         *
         * @param packet to send.
         */
        bool sendPacket(PacketPtr pkt);

        /**
         * Send a retry to the peer port only if it is needed. This is called
         * from the SimpleMemobj whenever it is unblocked.
         */
        void trySendRetryReq();

        /**
         * Get a list of the non-overlapping address ranges the owner is
         * responsible for. All response ports must override this function
         * and return a populated list with at least one item.
         *
         * @return a list of ranges responded to
         */
        AddrRangeList getAddrRanges() const override;

      protected:
        /**
         * Receive an atomic request packet from the request port.
         * No need to implement in this simple memobj.
         */
        Tick recvAtomic(PacketPtr pkt) override
        { panic("recvAtomic unimpl."); }

        /**
         * Receive a functional request packet from the request port.
         * Performs a "debug" access updating/reading the data in place.
         *
         * @param packet the requestor sent.
         */
        void recvFunctional(PacketPtr pkt) override;

        /**
         * Receive a timing request from the request port.
         *
         * @param the packet that the requestor sent
         * @return whether this object can consume the packet. If false, we
         *         will call sendRetry() when we can try to receive this
         *         request again.
         */
        bool recvTimingReq(PacketPtr pkt) override;

        /**
         * Called by the request port if sendTimingResp was called on this
         * response port (causing recvTimingResp to be called on the request
         * port) and was unsuccesful.
         */
        void recvRespRetry() override;
    };

    /**
     * Port on the memory-side that receives responses.
     */
    class MemSidePort : public RequestPort
    {
      private:
        /// The ctrl that owns this object (SecCtrl)
        SecCtrl *ctrl;

        /// If we tried to send a packet and it was blocked, store it here
        PacketPtr blockedPacket;

      public:
        /**
         * Constructor. Just calls the superclass constructor.
         */
        MemSidePort(const std::string& name, SecCtrl *_ctrl) :
            RequestPort(name, _ctrl),
            ctrl(_ctrl),
            blockedPacket(nullptr)
        {}

        /**
         * Send a packet across this port. This is called by the owner and
         * all of the flow control is hanled in this function.
         *
         * @param packet to send.
         */
        bool sendPacket(PacketPtr pkt);

      protected:
        /**
         * Receive a timing response from the response port.
         */
        bool recvTimingResp(PacketPtr pkt) override;

        /**
         * Called by the response port if sendTimingReq was called on this
         * request port (causing recvTimingReq to be called on the responder
         * port) and was unsuccesful.
         */
        void recvReqRetry() override;

        /**
         * Called to receive an address range change from the peer responder
         * port. The default implementation ignores the change and does
         * nothing. Override this function in a derived class if the owner
         * needs to be aware of the address ranges, e.g. in an
         * interconnect component like a bus.
         */
        void recvRangeChange() override;
    };

    /**
     * Utility
     */
    void updateChargeTime(Tick newChargeTime);

    PacketPtr createMetaPkt(
            Addr addr,
            unsigned size,
            bool isRead);

    bool sendCntPkt(bool isRead);
    bool sendMacPkt(bool isRead);
    bool sendMtPkt(uint8_t nth, bool isRead);

    /**
     * Handle the request from the CPU side
     *
     * @param requesting packet
     * @return true if we can handle the request this cycle, false if the
     *         requestor needs to retry later
     */
    void handleRequest(PacketPtr pkt);

    /**
     * Handle the respone from the memory side
     *
     * @param responding packet
     * @return true if we can handle the response this cycle, false if the
     *         responder needs to retry later
     */
    void handleResponse(PacketPtr pkt);

    /**
     * Handle a packet functionally. Update the data on a write and get the
     * data on a read.
     *
     * @param packet to functionally handle
     */
    void handleFunctional(PacketPtr pkt);

    /**
     * Return the address ranges this secctrl is responsible for.
     */
    AddrRangeList getAddrRanges() const;

    /**
     * Tell the CPU side to ask for our memory ranges.
     */
    void handleRangeChange();

    void processReadVerFinished();
    EventFunctionWrapper readVerFinished;

    void processSendMacWrite();
    EventFunctionWrapper sendMacWrite;

    void processSendNextMtWrite();
    EventFunctionWrapper sendNextMtWrite;

    void processWriteVerFinished();
    EventFunctionWrapper writeVerFinished;


    CPUSidePort cpuSidePort;
    MemSidePort memPort;
    MemSidePort metaPort;

    State state;

    Tick chargeTime;

    /**
     * Information of the packet being verified
     */
    Addr verifiedPktAddr;
    Addr verifiedCntOffs;
    uint32_t flags;
    uint16_t requestorId;
    bool needsResponse;

    mutable Addr cntBorder;
    mutable Addr macBorder;
    mutable Addr mtBorders[MT_LEVEL];

    PacketPtr responsePkt;

    PacketPtr counterPkt;
    PacketPtr macPkt;

    // Merkle Tree nodes without root
    PacketPtr mtPkts[MT_LEVEL-1];

  public:

    /**
     * Constructor
     */
    SecCtrl(const SecCtrlParams &p);

    /**
     * Get a port with a given name and index. This is used at
     * binding time and returns a reference to a protocol-agnostic
     * port.
     *
     * @param if_name Port name
     * @param idx Index in the case of a VectorPort
     *
     * @return A reference to the given port
     */
    Port &getPort(const std::string &if_name,
                  PortID idx=InvalidPortID) override;
};

} // namespace gem5

#endif // __CSH_SEC_CTRL_HH__
