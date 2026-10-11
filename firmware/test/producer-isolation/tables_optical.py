"""W2, W4, W5 (2) and W6 tables for the optical images.

Seeded from the census of the Oct 6 teensy41-optical ELF (no link map, no
relocations, no hook yet): the site counts below were re-measured by
aius_walk on that ELF and match §Walk specification W7. Object-file columns
are filename suffixes matched against the link map; parent-wiring re-runs
the census on the 4e link (map, relocations, hook) and fixes every row the
real link moves, in its own commit (CR4-S4).
"""
from aius_walk import RecursionRow, SiteRow, Tables

MAIN = "optical_main.cpp.o"
FFMPEG = r"ffmpeg-eac3"

SERVICE_CALLBACKS = ("names", [("_ZL14audio_clock_usPv", MAIN), ("_ZL26audio_enter_reset_criticalPv", MAIN),
                               ("_ZL25audio_exit_reset_criticalPv", MAIN)])
PCM_SINK = ("names", [("_ZN18AiusAudioProcessor13consume_blockEPvPK12AiusPcmBlock", MAIN)])
FFMPEG_ROW = ("taken_by", FFMPEG)

SDFAT = r"libSdFat\.a\(|libSD\.a\("
# SD stack (vendored SdFat and SD, built into libSdFat.a and libSD.a, whose
# provenance the link half proves): one row per site function, one target
# set for all of them (W4):
# - block-device and card virtuals: _ZTV8SdioCard, _ZTV16DedicatedSpiCard
#   (SharedSpiCard's own vtable is not linked, so no other implementer exists);
# - the date-time callback: FsDateTime's dateTimeMs10 adaptor and the SD
#   wrapper's SDClass::dateTime it forwards to;
# - SdioTeensy's busy and completion functions (m_busyFcn), address-taken
#   only by SdioTeensy.cpp.o;
# - the sector callbacks readSectorsCallback is passed: FatPartition's
#   freeClusterCount callbacks, taken by FatPartition.cpp.o.
# Not a target: Print (SDClass::format and the formatters write progress to
# a Print). format is reachable only through SDClass's FS vtable, which no
# row targets, and writeSectorsCallback's callback comes only from the
# formatters, so neither enters an SD root's closure.
SD_LIBRARY_ROW = ("union", [
    ("vtables", ["_ZTV8SdioCard", "_ZTV16DedicatedSpiCard"]),
    ("names", [("_ZL12dateTimeMs10PtS_Ph", "FsDateTime.cpp.o"), ("_ZN7SDClass8dateTimeEPtS0_", "SD.cpp.o")]),
    ("taken_by", r"libSdFat\.a\(SdioTeensy\.cpp\.o\)"),
    ("taken_by", r"libSdFat\.a\(FatPartition\.cpp\.o\)"),
])
# FS.h File's FileImpl calls (File::read, File::openNextFile, FS.h:167, :216,
# and the rest of the inline File API) inlined into the SD roots resolve to
# _ZTV6SDFile, the only FileImpl vtable linked (CR3-S2).
SDFILE_ROW = ("vtables", ["_ZTV6SDFile"])
SD_ROWS = [SiteRow(n, o, c, SD_LIBRARY_ROW) for n, o, c in [
    ("_ZN9ExFatFile4readEPvj", "ExFatFile.cpp.o", 2),
    ("_ZN9ExFatFile11openPrivateEPS_P8ExName_ti", "ExFatFile.cpp.o", 1),
    ("_ZN9ExFatFile4syncEv", "ExFatFileWrite.cpp.o", 1),
    ("_ZN9ExFatFile7syncDirEv", "ExFatFileWrite.cpp.o", 2),
    ("_ZN9ExFatFile6removeEv", "ExFatFileWrite.cpp.o", 1),
    ("_ZN9ExFatFile5writeEPKvj", "ExFatFileWrite.cpp.o", 2),
    ("_ZN7FsCache7prepareEmh", "FsCache.cpp.o", 1),
    ("_ZN7FsCache4syncEv", "FsCache.cpp.o", 2),
    ("_ZN7FatFile4syncEv", "FatFile.cpp.o", 2),
    ("_ZN7FatFile4readEPvj", "FatFile.cpp.o", 2),
    ("_ZN7FatFile5writeEPKvj", "FatFile.cpp.o", 2),
    ("_ZN7FatFile5mkdirEPS_P8FatLfn_t", "FatFile.cpp.o", 1),
    ("_ZN7FatFile6renameEPS_PKc", "FatFile.cpp.o", 1),
    ("_ZN7FatFile6removeEv", "FatFileLFN.cpp.o", 2),
    ("_ZN7FatFile4openEPS_P8FatLfn_ti", "FatFileLFN.cpp.o", 1),
    ("_ZN12FatPartition16freeClusterCountEv", "FatPartition.cpp.o", 4),
    ("_ZN13SharedSpiCard5beginE11SdSpiConfig", "SdSpiCard.cpp.o", 2),
    ("_ZN13SharedSpiCard11cardCommandEhm", "SdSpiCard.cpp.o", 2),
    ("_ZN13SharedSpiCard10writeStartEm", "SdSpiCard.cpp.o", 1),
    ("_ZN13SharedSpiCard9readStartEm", "SdSpiCard.cpp.o", 1),
    ("_ZN13SharedSpiCard5eraseEmm", "SdSpiCard.cpp.o", 1),
    ("_ZN13SharedSpiCard11writeSectorEmPKh", "SdSpiCard.cpp.o", 1),
    ("_ZN7SDClass11sdErrorCodeEv", "SD.cpp.o", 1),
    ("_ZN7SDClass12mediaPresentEv", "SD.cpp.o", 2),
    ("_ZL12dateTimeMs10PtS_Ph", "FsDateTime.cpp.o", 1),
    ("_ZN13SharedSpiCard10syncDeviceEv", "SdSpiCard.cpp.o", 1),
    ("_ZN13SharedSpiCard11sectorCountEv", "SdSpiCard.cpp.o", 1),
    ("_ZN16DedicatedSpiCard10readSectorEmPh", "SdSpiCard.cpp.o", 1),
    ("_ZN16DedicatedSpiCard11writeSectorEmPKh", "SdSpiCard.cpp.o", 1),
    ("_ZN16DedicatedSpiCard12writeSectorsEmPKhj", "SdSpiCard.cpp.o", 3),
    ("_ZN16DedicatedSpiCard15setDedicatedSpiEb", "SdSpiCard.cpp.o", 2),
    ("_ZN16DedicatedSpiCard19readSectorsCallbackEmPhjPFvmS0_PvES1_", "SdSpiCard.cpp.o", 1),
    ("_ZN16DedicatedSpiCard20writeSectorsCallbackEmjPFPKhmPvES2_", "SdSpiCard.cpp.o", 4),
    ("_ZN22FsBlockDeviceInterface19readSectorsCallbackEmPhjPFvmS0_PvES1_", "FatPartition.cpp.o", 2),
    ("_ZN22FsBlockDeviceInterface20writeSectorsCallbackEmjPFPKhmPvES2_", "FatFormatter.cpp.o", 2),
    ("_ZN7FatFile9timestampEhthhhhh", "FatFile.cpp.o", 1),
    ("_ZN9ExFatFile9timestampEhthhhhh", "ExFatFileWrite.cpp.o", 1),
    ("_ZN8SdioCard10readSectorEmPh", "SdioTeensy.cpp.o", 1),
    ("_ZN8SdioCard11readSectorsEmPhj", "SdioTeensy.cpp.o", 2),
    ("_ZN8SdioCard11writeSectorEmPKh", "SdioTeensy.cpp.o", 3),
    ("_ZN8SdioCard12writeSectorsEmPKhj", "SdioTeensy.cpp.o", 2),
    ("_ZN8SdioCard6isBusyEv", "SdioTeensy.cpp.o", 1),
]] + [
    # Under the hook: aius_producer_yield_wait > nested_pass > ledger record >
    # the boot-ledger commit, which calls its storage primitives. Registration
    # row: the primitives are filled at firmware/src/aius_boot_teensy.cpp:516
    # (hardware_*), the only functions that object takes the address of.
    SiteRow("aius_boot_storage_teensy_commit", "aius_boot_teensy.cpp.o", 10,
            ("taken_by", r"aius_boot_teensy\.cpp\.o")),
    # 25 since Phase 4g round 5 (SD.clearError and direct_card_settle(SD.sdErrorCode())).
    SiteRow("_ZL18app_read_directoryPvPKcP22AiusOpticalAppDirBatch", MAIN, 25, SDFILE_ROW),
    SiteRow("_ZL21app_write_replay_listPvPhj", MAIN, 35, SDFILE_ROW),
    SiteRow("_ZL24ota_copy_staged_to_psramPhm", MAIN, 14, SDFILE_ROW),
    SiteRow("_Z22aius_sd_teensy_executeP12AiusSdTeensyPK12AiusSdAction", "aius_sd_teensy.cpp.o", 17,
            ("vtables", ["_ZTV6SDFile", "_ZTV6FsFile", "_ZTV10StreamFileI10FsBaseFileyE"])),
    # yield, reached from the SD stack's yieldTimeout and never from a producer
    # root: serial-event and EventResponder dispatch, whose targets are the
    # functions the core's yield, EventResponder and HardwareSerial objects
    # take the address of.
    SiteRow("yield", "yield.cpp.o", 3,
            ("taken_by", r"libFrameworkArduino\.a\((yield|EventResponder|HardwareSerial\w*)\.cpp\.o\)")),
]

SITES = [
    # registration rows
    SiteRow("aius_audio_service_run", None, 7, SERVICE_CALLBACKS, "aius_audio_service.h:42-45"),
    SiteRow("aius_pipeline_service_one", None, 1, PCM_SINK, "aius_codec.h:125"),
    SiteRow("aius_a52_begin_access_unit", None, 5, PCM_SINK),
    SiteRow("aius_a52_decode_next_block", None, 3, PCM_SINK),
    SiteRow("aius_dts_decode_access_unit", None, 2, PCM_SINK),
    SiteRow("aius_eac3_decode_access_unit", None, 2, PCM_SINK),
    SiteRow("ff_get_buffer", None, 1, ("names", [("provide_planes", None)]), "aius_eac3.c:103, :154"),
    # library rows
    SiteRow("a52_imdct_256", None, 2, ("names", [("ifft128_c", None), ("ifft64_c", None)]), "imdct.c:77-78"),
    SiteRow("a52_imdct_512", None, 1, ("names", [("ifft128_c", None), ("ifft64_c", None)])),
    SiteRow("core_filter", None, 3, ("taken_by", r"dcadec-core"), "interpolator.h:27-30"),
    SiteRow("do_imdct", None, 5, FFMPEG_ROW),
    SiteRow("decode_audio_block", None, 2, FFMPEG_ROW),
    SiteRow("aius_eac3_core_frame", None, 1, FFMPEG_ROW),
    SiteRow("ff_ac3dsp_downmix", None, 1, FFMPEG_ROW),
    SiteRow("reset_ctx", None, 1, FFMPEG_ROW),
    SiteRow("int32_to_float_fmul_array8_c", None, 1, FFMPEG_ROW),
    SiteRow("ff_tx_init_subtx", None, 1, FFMPEG_ROW),
    SiteRow("ff_tx_init_tabs_float", None, 2, FFMPEG_ROW),
] + [SiteRow(n, None, c, FFMPEG_ROW) for n, c in [
    ("ff_tx_dctIII_float_c", 2), ("ff_tx_dctII_float_c", 2), ("ff_tx_dctI_float_c", 1),
    ("ff_tx_dstI_float_c", 1), ("ff_tx_fft_float_c", 1), ("ff_tx_fft_inplace_float_c", 1),
    ("ff_tx_fft_pfa_float_c", 2), ("ff_tx_fft_pfa_ns_float_c", 2), ("ff_tx_mdct_fwd_float_c", 1),
    ("ff_tx_mdct_inv_float_c", 1), ("ff_tx_mdct_inv_full_float_c", 1),
    ("ff_tx_mdct_pfa_15xM_fwd_float_c", 1), ("ff_tx_mdct_pfa_15xM_inv_float_c", 1),
    ("ff_tx_mdct_pfa_3xM_fwd_float_c", 1), ("ff_tx_mdct_pfa_3xM_inv_float_c", 1),
    ("ff_tx_mdct_pfa_5xM_fwd_float_c", 1), ("ff_tx_mdct_pfa_5xM_inv_float_c", 1),
    ("ff_tx_mdct_pfa_7xM_fwd_float_c", 1), ("ff_tx_mdct_pfa_7xM_inv_float_c", 1),
    ("ff_tx_mdct_pfa_9xM_fwd_float_c", 1), ("ff_tx_mdct_pfa_9xM_inv_float_c", 1),
    ("ff_tx_rdft_c2r_float_c", 1), ("ff_tx_rdft_r2c_float_c", 1), ("ff_tx_rdft_r2i_float_c", 1),
    ("ff_tx_rdft_r2i_mod2_float_c", 1), ("ff_tx_rdft_r2r_float_c", 1), ("ff_tx_rdft_r2r_mod2_float_c", 1)]
] + [
    # empty rows: setters absent from the image
    SiteRow("a52_block", None, 1, ("empty",), "parse.c:636-637; a52_dynrng absent"),
    SiteRow("ta_free", None, 1, ("empty",), "ta.c:52; ta_set_destructor absent"),
] + SD_ROWS  # stack-only rows live in firmware/test/stack-bound/tables_stack.py

VTABLES = {
    "_ZTV6SDFile": "FS.h FileImpl calls (W4 vtable row)",
    "_ZTV8SdioCard": "SdFat block-device and card calls (W4 vtable row)",
    "_ZTV16DedicatedSpiCard": "SdFat block-device and card calls (W4 vtable row)",
    "_ZTV4File": "Print slot: reached only behind the pinned libc row's _write site",
    "_ZTV6Stream": "Print slot: libc row",
    "_ZTV16usb_serial_class": "Print slot: libc row",
    "_ZTV10StreamFileI10FsBaseFileyE": "SdFat Stream adaptor: no producer or SD-root site",
    "_ZTV6FsFile": "SdFat Stream adaptor: no producer or SD-root site",
    "_ZTV7SDClass": "SD wrapper: no virtual call site",
    "_ZTV11AudioStream": "audio library update() dispatch (ISR, stack-bound handler table)",
    "_ZTV16AudioInputSPDIF3": "audio library update() dispatch (ISR)",
    "_ZTV17AudioOutputSPDIF3": "audio library update() dispatch (ISR)",
    "_ZTV18AiusAudioProcessor": "audio library update() dispatch (ISR)",
    "_ZTV11EthernetUDP": "network stack: no producer site",
    "_ZTV14EthernetClient": "network stack: no producer site",
    "_ZTV14EthernetServer": "network stack: no producer site",
    "_ZTV9IPAddress": "Printable: no producer site",
}

# 4e link: _vfprintf_r left the image with E14 (the log bodies' formatting is
# gone), so the row is 11 sites in 6 functions.
LIBC_ROW = {"__sflush_r": 3, "__sfvwrite_r": 3, "_vfiprintf_r": 1,
            "_wcrtomb_r": 2, "_wcsnrtombs_l": 1, "_write": 1}

RECURSION = [RecursionRow(anchor=("ff_tx_init_subtx", None), bound=2,
                          chain=[("ff_tx_init_subtx", None), ("ff_tx_mdct_init_float_c", None)],
                          size=43, guard="av_tx_init calls in vendor/ffmpeg-eac3 request 128 and 256 point inverse MDCTs only"),
             # Core: yield polls usb_serial_available, which calls yield; yield's
             # reentrancy flag (cores/teensy4/yield.cpp:45-47) returns at once on
             # the nested call, so the cycle nests once.
             RecursionRow(anchor=("yield", "yield.cpp.o"), bound=1,
                          chain=[("yield", "yield.cpp.o"), ("usb_serial_available", "usb_serial.c.o")],
                          size=2, guard="cores/teensy4/yield.cpp:45-47 static running flag"),
             # SdFat card layer (stack-bound only): an artefact of one target set
             # shared by each function's card-virtual sites. SdFat's file layer
             # calls the block device, which never calls back into the file
             # layer, so no real path re-enters; charged every member frame once
             # (an upper bound on any simple path) plus the deepest exit.
             RecursionRow(anchor=("_ZN8SdioCard10readSectorEmPh", "SdioTeensy.cpp.o"), bound=1, chain=[],
                          size=27, charge="sum",
                          guard="no recursion: shared card-vtable target set; device floor stack_free_min_bytes")]

TABLES = Tables(
    # Phase 4j (S1, S-new1): the idle DSP apply, called from platform_replay
    # (which yields), must reach no yield point between its flag read and
    # clear, apply_config included; noinline so this row cannot inline away.
    producer_roots=[("aius_audio_service_run", "aius_audio_service.c.o"),
                    ("_ZN18AiusAudioProcessor13consume_blockEPvPK12AiusPcmBlock", MAIN),
                    ("_ZN18AiusAudioProcessor22dsp_idle_apply_serviceEm", MAIN)],
    # Phase 4f: the status render's appends (status_append in optical_main.cpp,
    # diag_append in aius_runtime_storage.cpp's sd_diag section, both noinline)
    # are the render-append yield roots; every status section reaches the
    # yield through one of them.
    # Phase 4g: the web cache's send loop (the chunk-sourced body's source,
    # a yield every 8 chunks) and the pre-header verify's yield callback.
    # Phase 4h accept: the one-shot aius_web_cache_put (the settings routes,
    # the H10 restore and the Q2 re-put; 12 KB of dsp.json is 24 chunks
    # copied and chip-CRC-checked in one call) yields after every chunk
    # through aius_web_cache_set_put_yield, bound to web_cache_verify_yield,
    # so its yield-root row is that row below; no new direct caller.
    # Phase 4h flush and STORE sink: no yield point. The flush input
    # (aius_runtime_storage_root_input > aius_web_cache_copy_chunk) and the
    # sink's aius_web_cache_put_chunk each move one 512 B chunk per storage
    # advance, so the epoch loop between chunks already runs the producer.
    # W5 (3) fails a row naming a function that does not call the yield point.
    yield_roots=[("_ZL19platform_checkpointPvt", MAIN), ("_ZL13status_appendPcjPjPKcz", MAIN),
                 ("_ZN12_GLOBAL__N_1L11diag_appendEPcjPjPKcz", "aius_runtime_storage.cpp.o"),
                 ("_ZL20web_cache_body_chunkPvmPhPm", "aius_optical_app.cpp.o"),
                 ("_ZL22web_cache_verify_yieldPv", MAIN)],
    sd_windowed=[("_ZN12_GLOBAL__N_1L7advanceEP18AiusRuntimeStoragemb", "aius_runtime_storage.cpp.o"),
                 ("_ZL18app_read_directoryPvPKcP22AiusOpticalAppDirBatch", MAIN),
                 ("_ZL21app_write_replay_listPvPhj", MAIN), ("_ZL17app_replay_deletePvPKc", MAIN)],
    sd_unwindowed=[("_ZL18replay_job_advancem", MAIN), ("_ZL24ota_copy_staged_to_psramPhm", MAIN)],
    sd_stack_obj=r"libSdFat\.a\(|libSD\.a\(|aius_sd_teensy\.cpp\.o",
    sites=SITES,
    libc_row=LIBC_ROW,
    vtables=VTABLES,
    recursion=RECURSION,
    empty_bodies=["dca_format_log", "av_log", "avpriv_request_sample"],
)
