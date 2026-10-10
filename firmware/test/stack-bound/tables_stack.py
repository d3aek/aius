"""stack-bound tables for the optical images (4e link).

Scope (orchestrator decision, Phase 4e): stack-bound bounds the paths 4e
deepens, those through the card-wait hook (and, from Phase 4f, the status
render's chain to the render-append yield point): a declared caller chain from
loop() to each W2 SD root (every hop edge checked against the resolved
graph), then the resolved walk below the root restricted to callees that
reach the hook, then the nested pass (none under the unwindowed roots).
Paths that never reach the hook are unchanged by 4e and are backed by the
device floor stack_free_min_bytes. The exception chain stacks on top.

Declared frames (symbols with no .su) and the W3 leaves' inclusive depths
live in frames_optical.py, measured by gen_frames_optical.py.
"""
from aius_walk import YIELD_POINT, SiteRow
from stack_bound import Chain, Handler, StackTables

try:
    from frames_optical import DECLARED, LEAF_INCLUSIVE
except ImportError:  # before the first measurement
    DECLARED, LEAF_INCLUSIVE = {}, {}

MAIN = "optical_main.cpp.o"
RT = "aius_runtime_teensy.cpp.o"
SPDIF = "aius_spdif_sources.cpp.o"
DMA_CITE = ("cores/teensy4/DMAChannel.h", 514, "_VectorsRam[channel + IRQ_DMA_CH0 + 16] = isr;")

# loop > aius_runtime_teensy_epoch > aius_runtime_epoch, whose ops->service
# (aius_runtime.c:73) is runtime_service (aius_runtime_teensy.cpp:237).
EPOCH = [("loop", None, "call"), ("aius_runtime_teensy_epoch", RT, "call"), ("aius_runtime_epoch", None, "call"),
         ("_ZN12_GLOBAL__N_1L15runtime_serviceEPvPK17AiusRuntimeBudget", RT,
          "indirect: ops.service, aius_runtime_teensy.cpp:237")]
# runtime_service > runtime->platform.replay (platform_replay, optical_main.cpp:9645)
REPLAY = EPOCH + [("_ZL15platform_replayPvPK17AiusRuntimeBudgetm", MAIN,
                   "indirect: ops.replay, optical_main.cpp:9645")]
# runtime_service > aius_runtime_http_service > dispatch_route (Phase 4g: the
# fresh and the held DISPATCH share it) > aius_optical_app_http_dispatch > app op
HTTP = EPOCH + [("aius_runtime_http_service", None, "call"),
                ("_ZN12_GLOBAL__N_1L14dispatch_routeEP22AiusRuntimeNetworkHttpP24AiusOpticalAppDispatcherPK14AiusHttpAction",
                 "aius_runtime_network_http.cpp.o", "call"),
                ("aius_optical_app_http_dispatch", "aius_optical_app.cpp.o", "call")]
APP_OP = "indirect: AiusOpticalApp platform op registered by optical_main.cpp"
APP = "aius_optical_app.cpp.o"
WRITE_BODY_CHUNK = ("_ZN12_GLOBAL__N_1L16write_body_chunkEP22AiusRuntimeNetworkHttpPK14AiusHttpAction",
                    "aius_runtime_network_http.cpp.o", "call")
ADVANCE =("_ZN12_GLOBAL__N_1L7advanceEP18AiusRuntimeStoragemb", "aius_runtime_storage.cpp.o", "call")

CHAINS = [
    Chain("storage execute (replay root job)", REPLAY + [
        ("aius_runtime_teensy_start_storage_job", RT, "call"),
        ("aius_runtime_storage_start_root_job", None, "call"), ADVANCE]),
    Chain("storage execute (storage service)", EPOCH + [
        ("aius_runtime_storage_service", None, "call"), ADVANCE]),
    Chain("HTTP app_read_directory", HTTP + [("_ZL18app_read_directoryPvPKcP22AiusOpticalAppDirBatch", MAIN, APP_OP)]),
    Chain("HTTP app_write_replay_list", HTTP + [("_ZL21app_write_replay_listPvPhj", MAIN, APP_OP)]),
    Chain("HTTP app_replay_delete", HTTP + [("_ZL17app_replay_deletePvPKc", MAIN, APP_OP)]),
    # Phase 4f: the status render yields after every append (status_append
    # in optical_main.cpp, diag_append in aius_runtime_storage.cpp's sd_diag
    # section), so its deepest path runs to the yield point and a nested pass.
    Chain("HTTP app_write_status (render-append yields)",
          HTTP + [("_ZL16app_write_statusPvPhj", MAIN, APP_OP)], hook=YIELD_POINT),
    # Phase 4g: the fill walk's directory scan, from the SD lifecycle pass
    # (sd_lifecycle_service, inlined into platform_replay).
    Chain("fill walk app_read_directory", REPLAY + [
        ("_ZL19web_cache_fill_nextm", MAIN, "call"),
        ("_ZL18app_read_directoryPvPKcP22AiusOpticalAppDirBatch", MAIN, "call")]),
    # Phase 4g: the cache-served body's send loop, a yield point every 8
    # chunks under write_body_chunk's 512 B copy buffer.
    Chain("HTTP web cache send (body source yields)", EPOCH + [
        ("aius_runtime_http_service", None, "call"), WRITE_BODY_CHUNK,
        ("_ZL20web_cache_body_chunkPvmPhPm", APP, "indirect: response.body_chunk, set by dispatch_www_cached")],
        hook=YIELD_POINT),
    # Phase 4g: the pre-header verify's yield callback.
    Chain("HTTP web cache verify (yields)", HTTP + [
        ("_ZL21web_cache_seam_verifyPvh", MAIN, "indirect: AiusOpticalAppWebCache.verify (g_web_cache_seam)"),
        ("aius_web_cache_verify", None, "call"),
        ("_ZL22web_cache_verify_yieldPv", MAIN, "indirect: AiusWebCacheYield, web_cache_seam_verify")],
        hook=YIELD_POINT),
    Chain("unwindowed replay_job_advance", REPLAY + [("_ZL18replay_job_advancem", MAIN, "call")], unwindowed=True),
    Chain("unwindowed ota_copy_staged_to_psram",
          [("loop", None, "call"), ("_ZL24ota_copy_staged_to_psramPhm", MAIN, "call")], unwindowed=True),
]

EXTRA_SITES = [
    # advance's one site: service->publish.deliver (aius_runtime_storage.cpp:274),
    # set to runtime->platform.publish_config (aius_runtime_teensy.cpp:212),
    # which is platform_publish_config (optical_main.cpp:9649).
    SiteRow("_ZN12_GLOBAL__N_1L7advanceEP18AiusRuntimeStoragemb", "aius_runtime_storage.cpp.o", 1,
            ("names", [("_ZL23platform_publish_configPvPKhj", MAIN)])),
    # Phase 4g: the body source's seam calls (aius_optical_app.cpp
    # web_cache_body_chunk), registered by g_web_cache_seam (optical_main.cpp).
    SiteRow("_ZL20web_cache_body_chunkPvmPhPm", APP, 2,
            ("names", [("_ZL25web_cache_seam_copy_chunkPvhtPhPm", MAIN), ("_ZL21web_cache_seam_refillPv", MAIN)])),
    # Phase 4g: the fill primitive's hooks under the storage execute (the
    # root_output sink, optical_main.cpp): flush_delete is
    # web_cache_flush_delete (aius_web_cache_init in optical_main.cpp), and
    # chip_crc NULL selects aius_web_cache_crc32.
    SiteRow("aius_web_cache_fill_chunk", "aius_web_cache.c.o", 3,
            ("names", [("_ZL22web_cache_flush_deletePvm", MAIN), ("aius_web_cache_crc32", None)])),
    # Handler paths.
    SiteRow("_Z12software_isrv", "AudioStream.cpp.o", 1,  # every AudioStream's update()
            ("vtables", ["_ZTV16AudioInputSPDIF3", "_ZTV17AudioOutputSPDIF3", "_ZTV18AiusAudioProcessor"])),
    SiteRow("usb_isr", "usb.c.o", 2,  # usb_timer0/1 callbacks, usb_serial.c:305
            ("name_match", r"libFrameworkArduino\.a\(usb\w*\.c\.o\)", r"^(?!usb_isr$)")),
    SiteRow("run_callbacks", "usb.c.o", 1,  # transfer-completion callbacks
            ("name_match", r"libFrameworkArduino\.a\(usb\w*\.c\.o\)", r"^(?!usb_isr$)")),
    SiteRow("_reboot_Teensyduino_", "usb.c.o", 1, ("empty",), "boot ROM jump, usb.c:254; never returns"),
    SiteRow("_ZL7pit_isrv", "IntervalTimer.cpp.o", 4,  # funct_table[i]() inplace_function invokers
            ("name_match", r"IntervalTimer\.cpp\.o|AudioStream\.cpp\.o|NativeEthernet|aius_network_teensy",
             r"inplace_function")),
    # the plain-function-pointer invoker: NativeEthernet's IntervalTimer runs
    # fnet_poll (aius_network_teensy.cpp:245)
    SiteRow("_ZZN6teensy23inplace_function_detail6vtableIvJEEC4IPFvvEEENS0_7wrapperIT_EEENUlPvE_4_FUNES9_",
            None, 1, ("names", [("fnet_poll", None)])),
] + [
    # FNET under the PIT ISR (fnet_poll), each site's target set its registered
    # callbacks (framework-arduinoteensy/libraries/FNET/src and NativeEthernet/src):
    # the mutex API, NativeEthernet.cpp:107-111 and :239-243 (init_params.mutex_api).
    SiteRow(n, "fnet_stack.c.o", 1, ("names", [(t, "NativeEthernet.cpp.o")])) for n, t in [
        ("_fnet_mutex_lock", "_ZN13EthernetClass17teensy_mutex_lockEPPv"),
        ("_fnet_stack_mutex_lock", "_ZN13EthernetClass17teensy_mutex_lockEPPv"),
        ("_fnet_mutex_unlock", "_ZN13EthernetClass19teensy_mutex_unlockEPPv"),
        ("_fnet_stack_mutex_unlock", "_ZN13EthernetClass19teensy_mutex_unlockEPPv")]
] + [
    # timer API timer_get_ms (NativeEthernet.cpp:113-114, :245-246) and the
    # _fnet_timer_new handlers linked: fnet_arp.c:81, fnet_tcp.c:179, :187,
    # fnet_ip4.c:92 (fnet_netif.c:275's pmtu timer is not linked).
    SiteRow("_fnet_timer_poll", "fnet_timer.c.o", 3, ("names", [
        ("_ZN13EthernetClass12timer_get_msEv", "NativeEthernet.cpp.o"), ("_fnet_arp_timer", "fnet_arp.c.o"),
        ("_fnet_tcp_fasttimo", "fnet_tcp.c.o"), ("_fnet_tcp_slowtimo", "fnet_tcp.c.o"),
        ("_fnet_ip4_timer", "fnet_ip4.c.o")])),
    # pended ISR bottom halves: fnet_event_init handlers (fnet_arp.c:87,
    # fnet_ip4.c:104; fnet_socket.c:66's is not linked) and the FEC vector's
    # bottom half registered through _fnet_isr_vector_init (fnet_isr.c:114).
    SiteRow("fnet_isr_unlock", "fnet_isr.c.o", 1, ("names", [
        ("_fnet_arp_ip4_addr_conflict", "fnet_arp.c.o"), ("_fnet_ip4_input_low", "fnet_ip4.c.o"),
        ("fnet_fec_isr_handler_bottom", None)])),
    # registered service polls: fnet_service_register callers (FNET services,
    # e.g. fnet_dhcp_cln.c:1123, fnet_link.c:96, fnet_dns.c:287) and
    # EthernetServer::poll (NativeEthernetServer.cpp:37); only linked ones match.
    SiteRow("fnet_service_poll", "fnet_service.c.o", 1, ("name_match", r"libFNET\.a\(|libNativeEthernet\.a\(",
                                                          r"^(_fnet_\w+_poll|_ZN14EthernetServer4pollEPv)$")),
]

# PIT handler (pit_isr > inplace invoker > fnet_poll), orchestrator decision (b):
# charged a declared inclusive depth, not walked. 1,240 B is the walk's lower
# bound on the rebuilt 4e slot-a link with the sourced FNET rows above
# (pit_isr > fnet_poll > _fnet_timer_poll > _fnet_tcp_slowtimo > ... >
# _fnet_ip4_netif_output, where the unsourced netif/protocol/socket API sites
# begin), plus an explicit FNET allowance.
PIT_WALKED_LOWER_BOUND = 1240
FNET_ALLOWANCE = 2048
PIT_WHY = ("FNET under fnet_poll beyond the registered-callback rows is not walked; unchanged by 4e; "
           "device floor stack_free_min_bytes >= 2,048 in runs 1 and 2 backs it")

STACK = StackTables(
    declared=DECLARED,
    leaf_inclusive=LEAF_INCLUSIVE,
    pass_entry="aius_producer_yield_wait",
    main_roots=[("setup", None), ("loop", None)],
    chains=CHAINS,
    extra_sites=EXTRA_SITES,
    handlers=[
        # aius-installed (every install site under firmware/src, lib, vendor is a row)
        # aius_sites keyed by (file, enclosing function, call text), never by line
        Handler("_ZL17stall_witness_isrv", MAIN, 0, aius_sites=[
            ("firmware/src/optical_main.cpp", "stall_witness_arm",
             "attachInterruptVector(IRQ_GPT1, stall_witness_isr);"),
            ("firmware/src/optical_main.cpp", "stall_witness_arm", "NVIC_SET_PRIORITY(IRQ_GPT1, 0);")]),
        Handler("_ZL7pit_isrv", "IntervalTimer.cpp.o", 224, aius_sites=[  # AIUS_FNET_POLL_PRIORITY
            ("firmware/src/aius_network_teensy.cpp", "stack_start",
             "NVIC_SET_PRIORITY(IRQ_PIT, AIUS_FNET_POLL_PRIORITY);")],
                declared_depth=PIT_WALKED_LOWER_BOUND + FNET_ALLOWANCE,
                declared_why=PIT_WHY),
        Handler("_ZL5sdIrsv", "SdioTeensy.cpp.o", 6 * 16, aius_sites=[
            ("firmware/vendor/SdFat/src/SdCard/SdioTeensy.cpp", "initSDHC",
             "attachInterruptVector(IRQ_SDHC, sdIrs);"),
            ("firmware/vendor/SdFat/src/SdCard/SdioTeensy.cpp", "initSDHC", "NVIC_SET_PRIORITY(IRQ_SDHC, 6*16);")]),
        # framework-installed: cite the install line in the pinned framework.
        # Unset priorities are startup's 128 (cores/teensy4/startup.c:136).
        Handler("_Z12software_isrv", "AudioStream.cpp.o", 208,  # the audio update() ISR
                framework_cite=("cores/teensy4/AudioStream.cpp", 424, "NVIC_SET_PRIORITY(IRQ_SOFTWARE, 208)")),
        Handler("usb_isr", "usb.c.o", 128,
                framework_cite=("cores/teensy4/usb.c", 228, "attachInterruptVector(IRQ_USB1, &usb_isr)")),
        # DMA ISRs: aius uses the one-argument DMAChannel::attachInterrupt
        # (aius_spdif_sources.cpp:389, :454), which sets no priority.
        Handler("_ZN16AudioInputSPDIF33isrEv", SPDIF, 128, framework_cite=DMA_CITE),
        Handler("_ZN17AudioOutputSPDIF33isrEv", SPDIF, 128, framework_cite=DMA_CITE),
        Handler("_ZN12_GLOBAL__N_115AiusSpdifRxHook3isrEv", SPDIF, 128, framework_cite=DMA_CITE),
        Handler("_ZN12_GLOBAL__N_117AiusSpdifTx24Hook3isrEv", SPDIF, 128, framework_cite=DMA_CITE),
        # ENET: no row; the image links no ENET handler. An ENET vector FNET
        # installs at runtime (_fnet_isr_vector_init) is outside the scan;
        # the device floor backs it.
    ],
)

STACK_ZONE_SLOT_A_4E = 25632  # _estack - _ebss - 32 B MPU guard on the 4e teensy41-optical-slot-a link
