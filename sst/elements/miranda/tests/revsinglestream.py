import sst

# Define SST core options
sst.setProgramOption("timebase", "1ns")
sst.setProgramOption("stopAtCycle", "0 ns")

# Define the simulation components
comp_cpu = sst.Component("cpu", "miranda.BaseCPU")
comp_cpu.addParams({
	"verbose" : 0,
	"clock" : "2GHz",
	"generator" : "miranda.ReverseSingleStreamGenerator",
	"generatorParams.verbose" : 1,
        "generatorParams.start_at" : 4096,
        "generatorParams.stop_at" : 0,
	"printStats" : 1,
})

comp_l1cache = sst.Component("l1cache", "memHierarchy.Cache")
comp_l1cache.addParams({
      "access_latency_cycles" : "2",
      "cache_frequency" : "2 Ghz",
      "replacement_policy" : "lru",
      "coherence_protocol" : "MESI",
      "associativity" : "4",
      "cache_line_size" : "64",
      "prefetcher" : "cassini.StridePrefetcher",
      "debug" : "1",
      "low_network_links" : "1",
      "statistics" : "1",
      "L1" : "1",
      "cache_size" : "2KB"
})

comp_memory = sst.Component("memory", "memHierarchy.MemController")
comp_memory.addParams({
      "coherence_protocol" : "MESI",
      "backend.access_time" : "10 ns",
      "mem_size" : "512",
      "clock" : "1GHz"
})


# Define the simulation links
link_cpu_cache_link = sst.Link("link_cpu_cache_link")
link_cpu_cache_link.connect( (comp_cpu, "cache_link", "50ps"), (comp_l1cache, "high_network_0", "50ps") )
link_mem_bus_link = sst.Link("link_mem_bus_link")
link_mem_bus_link.connect( (comp_l1cache, "low_network_0", "50ps"), (comp_memory, "direct_link", "50ps") )
