import m5.objects
from m5.objects import *
from common import ObjectList
from common import HMC
from MetaCache import MetaCache

def create_mem_intf(intf, r, i, intlv_bits, intlv_size,
                    xor_low_bit):
    """
    Helper function for creating a single memoy controller from the given
    options.  This function is invoked multiple times in config_mem function
    to create an array of controllers.
    """

    import math
    intlv_low_bit = int(math.log(intlv_size, 2))

    # Use basic hashing for the channel selection, and preferably use
    # the lower tag bits from the last level cache. As we do not know
    # the details of the caches here, make an educated guess. 4 MByte
    # 4-way associative with 64 byte cache lines is 6 offset bits and
    # 14 index bits.
    if (xor_low_bit):
        xor_high_bit = xor_low_bit + intlv_bits - 1
    else:
        xor_high_bit = 0

    # Create an instance so we can figure out the address
    # mapping and row-buffer size
    interface = intf()

    # Only do this for DRAMs
    if issubclass(intf, m5.objects.DRAMInterface):
        # If the channel bits are appearing after the column
        # bits, we need to add the appropriate number of bits
        # for the row buffer size
        if interface.addr_mapping.value == 'RoRaBaChCo':
            # This computation only really needs to happen
            # once, but as we rely on having an instance we
            # end up having to repeat it for each and every
            # one
            rowbuffer_size = interface.device_rowbuffer_size.value * \
                interface.devices_per_rank.value

            intlv_low_bit = int(math.log(rowbuffer_size, 2))

    # Also adjust interleaving bits for NVM attached as memory
    # Will have separate range defined with unique interleaving
    if issubclass(intf, m5.objects.NVMInterface):
        # If the channel bits are appearing after the low order
        # address bits (buffer bits), we need to add the appropriate
        # number of bits for the buffer size
        if interface.addr_mapping.value == 'RoRaBaChCo':
            # This computation only really needs to happen
            # once, but as we rely on having an instance we
            # end up having to repeat it for each and every
            # one
            buffer_size = interface.per_bank_buffer_size.value

            intlv_low_bit = int(math.log(buffer_size, 2))

    # We got all we need to configure the appropriate address
    # range
    interface.range = m5.objects.AddrRange(r.start, size = r.size(),
                                      intlvHighBit = \
                                          intlv_low_bit + intlv_bits - 1,
                                      xorHighBit = xor_high_bit,
                                      intlvBits = intlv_bits,
                                      intlvMatch = i)
    return interface

def config_mem(options, system):
    """
    Create the memory controllers based on the options and attach them.

    If requested, we make a multi-channel configuration of the
    selected memory controller class by creating multiple instances of
    the specific class. The individual controllers have their
    parameters set such that the address range is interleaved between
    them.
    """

    # Mandatory options
    opt_mem_channels = options.mem_channels

    # Semi-optional options
    # Must have either mem_type or nvm_type or both
    opt_mem_type = getattr(options, "mem_type", None)
    opt_nvm_type = getattr(options, "nvm_type", None)
    if not opt_mem_type and not opt_nvm_type:
        fatal("Must have option for either mem-type or nvm-type, or both")

    # Optional options
    opt_tlm_memory = getattr(options, "tlm_memory", None)
    opt_external_memory_system = getattr(options, "external_memory_system",
                                         None)
    opt_elastic_trace_en = getattr(options, "elastic_trace_en", False)
    opt_mem_ranks = getattr(options, "mem_ranks", None)
    opt_nvm_ranks = getattr(options, "nvm_ranks", None)
    opt_hybrid_channel = getattr(options, "hybrid_channel", False)
    opt_dram_powerdown = getattr(options, "enable_dram_powerdown", None)
    opt_mem_channels_intlv = getattr(options, "mem_channels_intlv", 128)
    opt_xor_low_bit = getattr(options, "xor_low_bit", 0)

    if opt_mem_type == "HMC_2500_1x32":
        HMChost = HMC.config_hmc_host_ctrl(options, system)
        HMC.config_hmc_dev(options, system, HMChost.hmc_host)
        subsystem = system.hmc_dev
        xbar = system.hmc_dev.xbar
    else:
        subsystem = system
        xbar = system.membus

    if opt_tlm_memory:
        system.external_memory = m5.objects.ExternalSlave(
            port_type="tlm_slave",
            port_data=opt_tlm_memory,
            port=system.membus.mem_side_ports,
            addr_ranges=system.mem_ranges)
        system.workload.addr_check = False
        return

    if opt_external_memory_system:
        subsystem.external_memory = m5.objects.ExternalSlave(
            port_type=opt_external_memory_system,
            port_data="init_mem0", port=xbar.master,
            addr_ranges=system.mem_ranges)
        subsystem.workload.addr_check = False
        return

    nbr_mem_ctrls = opt_mem_channels

    import math
    from m5.util import fatal
    intlv_bits = int(math.log(nbr_mem_ctrls, 2))
    if 2 ** intlv_bits != nbr_mem_ctrls:
        fatal("Number of memory channels must be a power of 2")

    if opt_mem_type:
        intf = ObjectList.mem_list.get(opt_mem_type)
    if opt_nvm_type:
        n_intf = ObjectList.mem_list.get(opt_nvm_type)

    if opt_elastic_trace_en and not issubclass(intf, m5.objects.SimpleMemory):
        fatal("When elastic trace is enabled, configure mem-type as "
                "simple-mem.")

    # The default behaviour is to interleave memory channels on 128
    # byte granularity, or cache line granularity if larger than 128
    # byte. This value is based on the locality seen across a large
    # range of workloads.
    intlv_size = max(opt_mem_channels_intlv, system.cache_line_size.value)

    nvm_intf = create_mem_intf(n_intf, system.mem_ranges[0], 0,
        intlv_bits, intlv_size, opt_xor_low_bit)

    # Set the number of ranks based on the command-line
    # options if it was explicitly set
    if issubclass(n_intf, m5.objects.NVMInterface) and \
       opt_nvm_ranks:
        nvm_intf.ranks_per_channel = opt_nvm_ranks

    mem_ctrl = m5.objects.MemCtrl()
    mem_ctrl.nvm = nvm_intf

    # Insert SecCtrl between xbar and mem ctrl
    subsystem.sec_bus = SystemXBar()

    subsystem.sec_ctrl = SecCtrl()

    subsystem.meta_cache = MetaCache()

    subsystem.sec_ctrl.cpu_side_port = xbar.mem_side_ports

    subsystem.sec_ctrl.meta_port = subsystem.meta_cache.cpu_side

    subsystem.sec_bus.cpu_side_ports = [
            subsystem.meta_cache.mem_side,
            subsystem.sec_ctrl.mem_port
            ]

    mem_ctrl.port = subsystem.sec_bus.mem_side_ports

    subsystem.mem_ctrls = [mem_ctrl]
