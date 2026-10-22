"""Generated from config/addrmap.json; do not edit."""
MAP_ADDR_BITS = 64
TICKS_PER_SECOND = 1000000000000000
LEVELS = {'post_llc': 0, 'pre_cache': 1, 'axi_master': 2, 'interconnect': 3}
LEVEL_NAMES = {v:k for k,v in LEVELS.items()}
SOURCES = {'host': (0, 'post_llc', 2000, 500000), 'vortex': (1, 'post_llc', 1000, 1000000)}
SRC_NAME_BY_ID = {s[0]:n for n,s in SOURCES.items()}
REGIONS = {'vortex_cp': (536870912, 512, 'mmio', ('host',)), 'host_heap': (2147483648, 268435456, 'dram', ('host',)), 'vortex_bar': (4294967296, 4294967296, 'bar', ('host', 'vortex'))}
DRAM_WINDOW = (2147483648, 1073741824)
TRACE_WINDOWS = (('vortex_bar', 4294967296, 4294967296),)
HANDOFF_REGIONS = ('vortex_bar',)
def region_of(addr):
    return next((n for n,(b,s,_,_) in REGIONS.items() if b<=addr<b+s), None)
def is_dram(addr):
    return DRAM_WINDOW[0]<=addr<sum(DRAM_WINDOW)
def is_traced(addr):
    return any(b<=addr<b+s for _,b,s in TRACE_WINDOWS)
def may_access(src_name,addr):
    name=region_of(addr)
    return name is not None and src_name in REGIONS[name][3]
