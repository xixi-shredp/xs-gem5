from m5.objects.ClockedObject import ClockedObject
from m5.params import NULL, Param, RequestPort, ResponsePort
from m5.proxy import Parent


class IdealDCacheDirectedTester(ClockedObject):
    type = "IdealDCacheDirectedTester"
    cxx_header = (
        "cpu/testers/ideal_dcache/ideal_dcache_directed_tester.hh"
    )
    cxx_class = "gem5::IdealDCacheDirectedTester"

    system = Param.System(Parent.any, "System under test")
    mode = Param.String("Directed test mode")
    test_addr = Param.Addr(0x1000, "Cache-line-aligned test address")
    mshr_addr = Param.Addr(
        0x1000, "Address used to hold a same-line normal L1 MSHR target"
    )
    generic_addr = Param.Addr(
        0x1000000,
        "Address in a non-oracle memory used for functional reconciliation",
    )
    device_memory = Param.AbstractMemory(
        NULL, "Requestor-specific memory overlapping physical RAM"
    )
    action_delay = Param.Latency(
        "1us", "Delay after request admission before the interfering action"
    )
    verify_delay = Param.Latency(
        "20us", "Delay after carrier admission before the final raw read"
    )
    timeout = Param.Latency(
        "100us", "Maximum time allowed for a directed test"
    )

    external_port = RequestPort("Direct system-interconnect request port")
    normal_port = RequestPort("Request port connected through a normal L1D")
    ideal_port = RequestPort("Request port connected through the ideal L1D")
    visitor_probe_cpu_side_port = ResponsePort(
        "Probe response port connected to the normal L1D memory side"
    )
    visitor_probe_mem_side_port = RequestPort(
        "Probe request port forwarding normal L1D traffic downstream"
    )
