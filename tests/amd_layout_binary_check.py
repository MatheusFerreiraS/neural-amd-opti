"""Check AmdLayout.h and the bootstrap contracts against the danielblnc runtime binaries.

Every value in a layout is a raw write into, or a call into, a closed-source DLL, and a wrong one
does not fail politely. This reads the tables out of the headers and checks each one against the
binary whose size and SHA-256 it names:

  * code entries (init, Record, Notify, shutdown, the wait helper) are .pdata function starts in
    .text, and graphicsWaitEnd is the helper's .pdata end;
  * the four wait-dispatch return addresses are, in order, the helper's only `call [rax+0x70]`s;
  * every data field (and recordLock+0x4c) lies in .data, and d3dCompileIat is the IAT slot of
    D3DCompile, or 0 when the runtime imports no D3DCompile at all;
  * Record's first three tests read enabled, nativeFailure and initDone, in that order, and Notify
    calls through the trampoline slot near its start;
  * every option with a [DlssNrOnAmd] key is the store that follows that key in the INI reader;
  * every other data field is reached by the pinned instruction sites in ANCHORS (opcode bytes,
    containing function and, where it matters, the call that follows);
  * the bootstrap entry's call bytes, thread start, prologue and CreateThread import match.

Header-level checks make sure every table except 0.2.17 (no binary at hand) gives each non-zero
data field one of those per-field checks, so a new field or table cannot go in unpinned.

No runtime ships with this repository. Pass the files or folders that hold them:

    python tests/amd_layout_binary_check.py <version.dll | folder> ...

With no argument it checks the headers alone. Standard library only.
"""

import hashlib
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LAYOUT_H = ROOT / "OptiScaler/dlssnr/amd/AmdLayout.h"
HOSTLOAD_H = ROOT / "OptiScaler/dlssnr/amd/RuntimeHostLoad.h"

# Members that are .pdata function starts, and fields that are not data slots.
CODE = ("init", "record", "notify", "shutdown", "graphicsWaitBegin")
DISPATCH = ("waitDispatchInit", "waitDispatchFallback", "waitDispatchSlices", "waitDispatchFinish")
NOT_DATA = set(CODE) | set(DISPATCH) | {"name", "size", "sha256", "d3dCompileIat", "graphicsWaitEnd"}
# [DlssNrOnAmd] key -> layout member, for the options the runtime's INI reader stores directly.
INI_KEYS = {
    "Enabled": "enabled", "Temporal": "temporal", "UseFsrInputs": "fsrInputs", "UseDepth": "depthPresent",
    "Tonemap": "tonemap", "LocalTone": "tone", "LocalStructure": "structure", "SkinStructure": "skin",
    "UseAutoMask": "charMask", "ToneChannels": "toneChannels", "Scale": "scale", "SpinDraw": "spinDraw",
    "Async": "configuredInline", "Interop": "interop", "Style": "style", "ToneCurve": "toneCurve",
    "ToneLift": "toneLift", "UseGameExposure": "useGameExposure", "Quality": "quality",
}
# Fields the checks in check_layout pin by what the code does with them (besides INI_KEYS).
BEHAVIOUR_CHECKED = {"enabled", "nativeFailure", "initDone", "trampoline"}
SEED_OFFSETS = {"0.4.1": (0x3c, 0x104), "0.4.2": (0x3c, 0x104),
                "0.4.3": (0x3c, 0x104), "0.5.0": (0x4c, 0x114)}

# Pinned reference sites for every other data field: (field, site RVA, opcode bytes before the
# rip disp32, bytes after it, layout function the site must lie in or None, layout function the
# next call goes to or None[, offset the site adds to the field]). The instruction at the site must
# have exactly these bytes around its disp32, and the disp32 must reach the field. The 0.4.0 sites
# are the ones the verified maps cite. The 0.4.1 rows are the same instructions, found through the
# aligned instructions of each matched function (analysis-opti/implement/anchors/derive_041.py), and the
# 0.4.2 and 0.4.3 rows are the 0.4.1 ones mapped the same way (derive_042_043.py), and the 0.5.0 rows
# the 0.4.3 ones (derive_050.py). The 0.5.1 rows are the 0.5.0 ones through derive_051.py, at the same
# addresses, and the 0.6.0 rows the 0.5.1 ones through derive_060.py, but for Record's depthInverted read,
# which moved to another register (0x19a72, found by its field).
# The 0.3.1 and 0.3.0 rows are the same instructions in the same order, found by shape (daniel-runtime/analysis-opti/implement/anchors/derive_anchors.py and
# anchors.txt, which prints each site with its neighbours). 0.3.0 has no Notify-side jobDone wait
# and no mapped wait helper, so it lacks those rows.
ANCHORS = {
    "0.5.0": [
        ('device', 0xa032, '48 83 3d', '00', 'notify', None),
        ('queue', 0xa028, '48 83 3d', '00', 'notify', None),
        ('queue', 0xa091, '48 89 35', '', 'notify', None),
        ('engine', 0xc159, '48 8d 0d', '', None, 'init'),
        ('engine', 0x17cd5, '48 8d 0d', '', 'record', None),
        ('historyView', 0x1d506, '4c 8b 05', '', None, None),
        ('historyValid', 0x17ceb, 'c6 05', '00', 'record', None),
        ('jobDone', 0x159db, '39 05', '', 'record', None),
        ('jobDone', 0xa268, '39 2d', '', 'notify', None),
        ('timeoutCount', 0x1968f, 'c7 05', '00 00 00 00', 'shutdown', None),
        ('timeoutCount', 0x1e9d9, '3b 05', '', None, None),
        ('watchdog', 0x188dd, '44 8b 05', '', 'record', 'graphicsWaitBegin'),
        ('pendingList', 0xa0e5, 'f0 48 0f b1 0d', '', 'notify', None),
        ('pendingList', 0x18e62, '48 87 05', '', 'record', None),
        ('jobId', 0x159d5, '8b 05', '', 'record', None),
        ('jobId', 0x18e3c, '44 89 25', '', 'record', None),
        ('depthInverted', 0x17a3d, '44 8b 15', '', 'record', None),
        ('depthInverted', 0x14280, '44 89 0d', '', None, None),
        ('explicitDepth', 0x1426e, 'c6 05', '01', None, None),
        ('hipOrdinal', 0x156d4, '8b 0d', '', 'record', None),
        ('recreate', 0x159a9, '80 3d', '01', 'record', None),
        ('recreate', 0x15ce1, 'c6 05', '00', 'record', None),
        ('recordLock', 0x15cb8, '48 8d 0d', '', 'record', None),
        ('recordLock', 0x15ccc, '81 3d', 'ff ff ff 7f', 'record', 'shutdown', 0x4c),
        ('gate4c', 0x159b6, '83 3d', '00', 'record', None),
        ('gate68', 0x159bf, '83 3d', '00', 'record', None),
        ('gate68', 0xa0f9, '87 2d', '', 'notify', None),
        ('counter78', 0x1832f, 'ff 05', '', 'record', None),
        ('graphicsPso', 0x19edf, '48 83 3d', '00', 'graphicsWaitBegin', None),
        ('graphicsPso', 0x19f00, '48 8b 15', '', 'graphicsWaitBegin', None),
        ('predicateReady', 0x19be9, '80 3d', '01', 'graphicsWaitBegin', None),
    ],
    "0.5.1": [
        ('device', 0xa032, '48 83 3d', '00', 'notify', None),
        ('queue', 0xa028, '48 83 3d', '00', 'notify', None),
        ('queue', 0xa091, '48 89 35', '', 'notify', None),
        ('engine', 0xc159, '48 8d 0d', '', None, 'init'),
        ('engine', 0x17cd5, '48 8d 0d', '', 'record', None),
        ('historyView', 0x1d506, '4c 8b 05', '', None, None),
        ('historyValid', 0x17ceb, 'c6 05', '00', 'record', None),
        ('jobDone', 0x159db, '39 05', '', 'record', None),
        ('jobDone', 0xa268, '39 2d', '', 'notify', None),
        ('timeoutCount', 0x1968f, 'c7 05', '00 00 00 00', 'shutdown', None),
        ('timeoutCount', 0x1e9d9, '3b 05', '', None, None),
        ('watchdog', 0x188dd, '44 8b 05', '', 'record', 'graphicsWaitBegin'),
        ('pendingList', 0xa0e5, 'f0 48 0f b1 0d', '', 'notify', None),
        ('pendingList', 0x18e62, '48 87 05', '', 'record', None),
        ('jobId', 0x159d5, '8b 05', '', 'record', None),
        ('jobId', 0x18e3c, '44 89 25', '', 'record', None),
        ('depthInverted', 0x17a3d, '44 8b 15', '', 'record', None),
        ('depthInverted', 0x14280, '44 89 0d', '', None, None),
        ('explicitDepth', 0x1426e, 'c6 05', '01', None, None),
        ('hipOrdinal', 0x156d4, '8b 0d', '', 'record', None),
        ('recreate', 0x159a9, '80 3d', '01', 'record', None),
        ('recreate', 0x15ce1, 'c6 05', '00', 'record', None),
        ('recordLock', 0x15cb8, '48 8d 0d', '', 'record', None),
        ('recordLock', 0x15ccc, '81 3d', 'ff ff ff 7f', 'record', 'shutdown', 0x4c),
        ('gate4c', 0x159b6, '83 3d', '00', 'record', None),
        ('gate68', 0x159bf, '83 3d', '00', 'record', None),
        ('gate68', 0xa0f9, '87 2d', '', 'notify', None),
        ('counter78', 0x1832f, 'ff 05', '', 'record', None),
        ('graphicsPso', 0x19edf, '48 83 3d', '00', 'graphicsWaitBegin', None),
        ('graphicsPso', 0x19f00, '48 8b 15', '', 'graphicsWaitBegin', None),
        ('predicateReady', 0x19be9, '80 3d', '01', 'graphicsWaitBegin', None),
    ],
    "0.6.0": [
        ('device', 0xb4d2, '48 83 3d', '00', 'notify', None),
        ('queue', 0xb4c8, '48 83 3d', '00', 'notify', None),
        ('queue', 0xb531, '48 89 35', '', 'notify', None),
        ('engine', 0xda0f, '48 8d 0d', '', None, 'init'),
        ('engine', 0x19d2b, '48 8d 0d', '', 'record', None),
        ('historyView', 0x1fcae, '4c 8b 05', '', None, None),
        ('historyValid', 0x19d41, 'c6 05', '00', 'record', None),
        ('jobDone', 0x17a1b, '39 05', '', 'record', None),
        ('jobDone', 0xb708, '39 2d', '', 'notify', None),
        ('timeoutCount', 0x1b76f, 'c7 05', '00 00 00 00', 'shutdown', None),
        ('timeoutCount', 0x2129e, '3b 05', '', None, None),
        ('watchdog', 0x1a98a, '44 8b 05', '', 'record', 'graphicsWaitBegin'),
        ('pendingList', 0xb585, 'f0 48 0f b1 0d', '', 'notify', None),
        ('pendingList', 0x1af38, '48 87 05', '', 'record', None),
        ('jobId', 0x17a15, '8b 05', '', 'record', None),
        ('jobId', 0x1aee9, '44 89 25', '', 'record', None),
        ('depthInverted', 0x16260, '44 89 0d', '', None, None),
        ('depthInverted', 0x19a72, '8b 15', '', 'record', None),
        ('explicitDepth', 0x1624e, 'c6 05', '01', None, None),
        ('hipOrdinal', 0x17714, '8b 0d', '', 'record', None),
        ('recreate', 0x179e9, '80 3d', '01', 'record', None),
        ('recreate', 0x17d21, 'c6 05', '00', 'record', None),
        ('recordLock', 0x17cf8, '48 8d 0d', '', 'record', None),
        ('recordLock', 0x17d0c, '81 3d', 'ff ff ff 7f', 'record', 'shutdown', 0x4c),
        ('gate4c', 0x179f6, '83 3d', '00', 'record', None),
        ('gate68', 0x179ff, '83 3d', '00', 'record', None),
        ('gate68', 0xb599, '87 2d', '', 'notify', None),
        ('counter78', 0x1a3b2, 'ff 05', '', 'record', None),
        ('graphicsPso', 0x1c14f, '48 83 3d', '00', 'graphicsWaitBegin', None),
        ('graphicsPso', 0x1c170, '48 8b 15', '', 'graphicsWaitBegin', None),
        ('predicateReady', 0x1be59, '80 3d', '01', 'graphicsWaitBegin', None),
    ],
    "0.4.3": [
        ("device", 0xa232, "48 83 3d", "00", "notify", None),
        ("queue", 0xa228, "48 83 3d", "00", "notify", None),
        ("queue", 0xa291, "48 89 35", "", "notify", None),
        ("engine", 0xc32b, "48 8d 0d", "", None, "init"),
        ("engine", 0x17e85, "48 8d 0d", "", "record", None),
        ("historyView", 0x1d666, "4c 8b 05", "", None, None),
        ("historyValid", 0x17e9b, "c6 05", "00", "record", None),
        ("jobDone", 0x15b8b, "39 05", "", "record", None),
        ("jobDone", 0xa468, "39 2d", "", "notify", None),
        ("timeoutCount", 0x197ef, "c7 05", "00 00 00 00", "shutdown", None),
        ("timeoutCount", 0x1eb39, "3b 05", "", None, None),
        ("watchdog", 0x18a3c, "44 8b 05", "", "record", "graphicsWaitBegin"),
        ("pendingList", 0xa2e5, "f0 48 0f b1 0d", "", "notify", None),
        ("pendingList", 0x18fc1, "48 87 05", "", "record", None),
        ("jobId", 0x15b85, "8b 05", "", "record", None),
        ("jobId", 0x18f9b, "44 89 25", "", "record", None),
        ("depthInverted", 0x17bed, "44 8b 15", "", "record", None),
        ("depthInverted", 0x14430, "44 89 0d", "", None, None),
        ("explicitDepth", 0x1441e, "c6 05", "01", None, None),
        ("hipOrdinal", 0x15884, "8b 0d", "", "record", None),
        ("recreate", 0x15b59, "80 3d", "01", "record", None),
        ("recreate", 0x15e91, "c6 05", "00", "record", None),
        ("recordLock", 0x15e68, "48 8d 0d", "", "record", None),
        ("recordLock", 0x15e7c, "81 3d", "ff ff ff 7f", "record", "shutdown", 0x4c),
        ("gate4c", 0x15b66, "83 3d", "00", "record", None),
        ("gate68", 0x15b6f, "83 3d", "00", "record", None),
        ("gate68", 0xa2f9, "87 2d", "", "notify", None),
        ("counter78", 0x1848e, "ff 05", "", "record", None),
        ("graphicsPso", 0x1a03f, "48 83 3d", "00", "graphicsWaitBegin", None),
        ("graphicsPso", 0x1a060, "48 8b 15", "", "graphicsWaitBegin", None),
        ("predicateReady", 0x19d49, "80 3d", "01", "graphicsWaitBegin", None),
    ],
    "0.4.2": [
        ("device", 0x9de2, "48 83 3d", "00", "notify", None),
        ("queue", 0x9dd8, "48 83 3d", "00", "notify", None),
        ("queue", 0x9e41, "48 89 35", "", "notify", None),
        ("engine", 0xbedb, "48 8d 0d", "", None, "init"),
        ("engine", 0x176d5, "48 8d 0d", "", "record", None),
        ("historyView", 0x1cec6, "4c 8b 05", "", None, None),
        ("historyValid", 0x176eb, "c6 05", "00", "record", None),
        ("jobDone", 0x153db, "39 05", "", "record", None),
        ("jobDone", 0xa018, "39 2d", "", "notify", None),
        ("timeoutCount", 0x1903f, "c7 05", "00 00 00 00", "shutdown", None),
        ("timeoutCount", 0x1e3b7, "3b 05", "", None, None),
        ("watchdog", 0x1828c, "44 8b 05", "", "record", "graphicsWaitBegin"),
        ("pendingList", 0x9e95, "f0 48 0f b1 0d", "", "notify", None),
        ("pendingList", 0x18811, "48 87 05", "", "record", None),
        ("jobId", 0x153d5, "8b 05", "", "record", None),
        ("jobId", 0x187eb, "44 89 25", "", "record", None),
        ("depthInverted", 0x1743d, "44 8b 15", "", "record", None),
        ("depthInverted", 0x13c80, "44 89 0d", "", None, None),
        ("explicitDepth", 0x13c6e, "c6 05", "01", None, None),
        ("hipOrdinal", 0x150d4, "8b 0d", "", "record", None),
        ("recreate", 0x153a9, "80 3d", "01", "record", None),
        ("recreate", 0x156e1, "c6 05", "00", "record", None),
        ("recordLock", 0x156b8, "48 8d 0d", "", "record", None),
        ("recordLock", 0x156cc, "81 3d", "ff ff ff 7f", "record", "shutdown", 0x4c),
        ("gate4c", 0x153b6, "83 3d", "00", "record", None),
        ("gate68", 0x153bf, "83 3d", "00", "record", None),
        ("gate68", 0x9ea9, "87 2d", "", "notify", None),
        ("counter78", 0x17cde, "ff 05", "", "record", None),
        ("graphicsPso", 0x1988f, "48 83 3d", "00", "graphicsWaitBegin", None),
        ("graphicsPso", 0x198b0, "48 8b 15", "", "graphicsWaitBegin", None),
        ("predicateReady", 0x19599, "80 3d", "01", "graphicsWaitBegin", None),
    ],
    "0.4.1": [
        ("device", 0x9db2, "48 83 3d", "00", "notify", None),
        ("queue", 0x9da8, "48 83 3d", "00", "notify", None),
        ("queue", 0x9e11, "48 89 35", "", "notify", None),
        ("engine", 0xbeab, "48 8d 0d", "", None, "init"),
        ("engine", 0x172ba, "48 8d 0d", "", "record", None),
        ("historyView", 0x1ca56, "4c 8b 05", "", None, None),
        ("historyValid", 0x172d0, "c6 05", "00", "record", None),
        ("jobDone", 0x14fdb, "39 05", "", "record", None),
        ("jobDone", 0x9fe8, "39 2d", "", "notify", None),
        ("timeoutCount", 0x18c1f, "c7 05", "00 00 00 00", "shutdown", None),
        ("timeoutCount", 0x1df37, "3b 05", "", None, None),
        ("watchdog", 0x17e71, "44 8b 05", "", "record", "graphicsWaitBegin"),
        ("pendingList", 0x9e65, "f0 48 0f b1 0d", "", "notify", None),
        ("pendingList", 0x183f6, "48 87 05", "", "record", None),
        ("jobId", 0x14fd5, "8b 05", "", "record", None),
        ("jobId", 0x183d0, "44 89 25", "", "record", None),
        ("depthInverted", 0x1703d, "44 8b 15", "", "record", None),
        ("depthInverted", 0x13880, "44 89 0d", "", None, None),
        ("explicitDepth", 0x1386e, "c6 05", "01", None, None),
        ("hipOrdinal", 0x14cd4, "8b 0d", "", "record", None),
        ("recreate", 0x14fa9, "80 3d", "01", "record", None),
        ("recreate", 0x152e1, "c6 05", "00", "record", None),
        ("recordLock", 0x152b8, "48 8d 0d", "", "record", None),
        ("recordLock", 0x152cc, "81 3d", "ff ff ff 7f", "record", "shutdown", 0x4c),
        ("gate4c", 0x14fb6, "83 3d", "00", "record", None),
        ("gate68", 0x14fbf, "83 3d", "00", "record", None),
        ("gate68", 0x9e79, "87 2d", "", "notify", None),
        ("counter78", 0x178c3, "ff 05", "", "record", None),
        ("graphicsPso", 0x1946f, "48 83 3d", "00", "graphicsWaitBegin", None),
        ("graphicsPso", 0x19490, "48 8b 15", "", "graphicsWaitBegin", None),
        ("predicateReady", 0x19179, "80 3d", "01", "graphicsWaitBegin", None),
    ],
    "0.4.0": [
        ("device", 0x9e42, "48 83 3d", "00", "notify", None),  # cmp qword [device],0 after call [trampoline]
        ("queue", 0x9e38, "48 83 3d", "00", "notify", None),  # cmp qword [queue],0 after call [trampoline]
        ("queue", 0x9ea1, "48 89 35", "", "notify", None),  # mov [queue],rsi; 'present queue %p'
        ("engine", 0xbf3b, "48 8d 0d", "", None, "init"),  # lea rcx,[engine]; lea rdx,weights; call init
        ("engine", 0x172bd, "48 8d 0d", "", "record", None),  # lea rcx,[engine]; call history reset
        ("historyView", 0x1ca66, "4c 8b 05", "", None, None),  # worker: mov r8,[historyView]; 'dlssnr_hist'
        ("historyValid", 0x172d3, "c6 05", "00", "record", None),  # mov byte [historyValid],0 after the reset
        ("jobDone", 0x1506b, "39 05", "", "record", None),  # mov eax,[jobId]; cmp [jobDone],eax
        ("jobDone", 0xa078, "39 2d", "", "notify", None),  # inline wait: cmp [jobDone],ebp
        ("timeoutCount", 0x18c2f, "c7 05", "00 00 00 00", "shutdown", None),  # timeoutCount = 0
        ("timeoutCount", 0x1df47, "3b 05", "", None, None),  # worker: cmp eax,[timeoutCount]; jne store
        ("watchdog", 0x17e74, "44 8b 05", "", "record", "graphicsWaitBegin"),  # mov r8d,[watchdog]; call wait
        ("pendingList", 0x9ef5, "f0 48 0f b1 0d", "", "notify", None),  # lock cmpxchg [pendingList],rcx
        ("pendingList", 0x183f9, "48 87 05", "", "record", None),  # publish: xchg [pendingList],rax
        ("jobId", 0x15065, "8b 05", "", "record", None),  # mov eax,[jobId]; cmp [jobDone],eax
        ("jobId", 0x183d3, "44 89 25", "", "record", None),  # mov [jobId],r12d before the publish
        ("depthInverted", 0x170cd, "44 8b 15", "", "record", None),  # mov r10d,[depthInverted]
        ("depthInverted", 0x13910, "44 89 0d", "", None, None),  # FSR flag hook: mov [depthInverted],r9d
        ("explicitDepth", 0x138fe, "c6 05", "01", None, None),  # FSR flag hook: mov byte [explicitDepth],1
        ("hipOrdinal", 0x14d64, "8b 0d", "", "record", None),  # mov ecx,[hipOrdinal] for hipSetDevice
        ("recreate", 0x15039, "80 3d", "01", "record", None),  # cmp byte [recreate],1
        ("recreate", 0x15371, "c6 05", "00", "record", None),  # mov byte [recreate],0 after the join/reset
        ("recordLock", 0x15348, "48 8d 0d", "", "record", None),  # lea rcx,[recordLock]; call lock
        ("recordLock", 0x1535c, "81 3d", "ff ff ff 7f", "record", "shutdown", 0x4c),  # cmp [lock+0x4c],0x7fffffff
        ("gate4c", 0x15046, "83 3d", "00", "record", None),  # cmp dword [gate4c],0; jns
        ("gate68", 0x1504f, "83 3d", "00", "record", None),  # cmp dword [gate68],0; jns
        ("gate68", 0x9f09, "87 2d", "", "notify", None),  # xchg [gate68],ebp after the pendingList cmpxchg
        ("counter78", 0x178c6, "ff 05", "", "record", None),  # inc dword [counter78]
        ("graphicsPso", 0x1947f, "48 83 3d", "00", "graphicsWaitBegin", None),  # cmp qword [graphicsPso],0
        ("graphicsPso", 0x194a0, "48 8b 15", "", "graphicsWaitBegin", None),  # mov rdx,[graphicsPso] for SetPSO
        ("predicateReady", 0x19189, "80 3d", "01", "graphicsWaitBegin", None),  # cmp byte [predicateReady],1
    ],
    "0.3.1": [
        ("device", 0x9752, "48 83 3d", "00", "notify", None),
        ("queue", 0x9748, "48 83 3d", "00", "notify", None),
        ("queue", 0x97b1, "48 89 35", "", "notify", None),
        ("engine", 0xb84b, "48 8d 0d", "", None, "init"),
        ("engine", 0x15b2d, "48 8d 0d", "", "record", None),
        ("historyView", 0x1aeff, "4c 8b 05", "", None, None),
        ("historyValid", 0x15b43, "c6 05", "00", "record", None),
        ("jobDone", 0x138db, "39 05", "", "record", None),
        ("jobDone", 0x9988, "39 2d", "", "notify", None),
        ("timeoutCount", 0x1749f, "c7 05", "00 00 00 00", "shutdown", None),
        ("timeoutCount", 0x1bdec, "3b 05", "", None, None),
        ("watchdog", 0x166e4, "44 8b 05", "", "record", "graphicsWaitBegin"),
        ("pendingList", 0x9805, "f0 48 0f b1 0d", "", "notify", None),
        ("pendingList", 0x16c69, "48 87 05", "", "record", None),
        ("jobId", 0x138d5, "8b 05", "", "record", None),
        ("jobId", 0x16c43, "44 89 25", "", "record", None),
        ("depthInverted", 0x1593d, "44 8b 15", "", "record", None),
        ("depthInverted", 0x12180, "44 89 0d", "", None, None),
        ("explicitDepth", 0x1216e, "c6 05", "01", None, None),
        ("hipOrdinal", 0x135d4, "8b 0d", "", "record", None),
        ("recreate", 0x138a9, "80 3d", "01", "record", None),
        ("recreate", 0x13be1, "c6 05", "00", "record", None),
        ("recordLock", 0x13bb8, "48 8d 0d", "", "record", None),
        ("recordLock", 0x13bcc, "81 3d", "ff ff ff 7f", "record", "shutdown", 0x4c),
        ("gate4c", 0x138b6, "83 3d", "00", "record", None),
        ("gate68", 0x138bf, "83 3d", "00", "record", None),
        ("gate68", 0x9819, "87 2d", "", "notify", None),
        ("counter78", 0x16136, "ff 05", "", "record", None),
        ("graphicsPso", 0x17ccd, "48 83 3d", "00", "graphicsWaitBegin", None),
        ("graphicsPso", 0x17cee, "48 8b 15", "", "graphicsWaitBegin", None),
        ("predicateReady", 0x179d9, "80 3d", "01", "graphicsWaitBegin", None),
    ],
    "0.3.0": [
        ("device", 0x9483, "48 83 3d", "00", "notify", None),
        ("queue", 0x9479, "48 83 3d", "00", "notify", None),
        ("queue", 0x94e2, "48 89 35", "", "notify", None),
        ("engine", 0xb04a, "48 8d 0d", "", None, "init"),
        ("engine", 0x14d12, "48 8d 0d", "", "record", None),
        ("historyView", 0x19899, "4c 8b 05", "", None, None),
        ("historyValid", 0x14d28, "c6 05", "00", "record", None),
        ("jobDone", 0x129db, "39 05", "", "record", None),
        ("timeoutCount", 0x164c0, "c7 05", "00 00 00 00", "shutdown", None),
        ("timeoutCount", 0x1a6a0, "3b 05", "", None, None),
        ("watchdog", 0x158cc, "44 8b 05", "", "record", None),  # its wait helper 0x169a0 is not in the table
        ("pendingList", 0x9535, "f0 48 0f b1 0d", "", "notify", None),
        ("pendingList", 0x15d02, "48 87 05", "", "record", None),
        ("jobId", 0x129d5, "8b 05", "", "record", None),
        ("jobId", 0x15cdc, "44 89 25", "", "record", None),
        ("depthInverted", 0x14b22, "44 8b 15", "", "record", None),
        ("depthInverted", 0x11280, "44 89 0d", "", None, None),
        ("explicitDepth", 0x1126e, "c6 05", "01", None, None),
        ("hipOrdinal", 0x126d4, "8b 0d", "", "record", None),
        ("recreate", 0x129a9, "80 3d", "01", "record", None),
        ("recreate", 0x12ca1, "c6 05", "00", "record", None),
        ("recordLock", 0x12c78, "48 8d 0d", "", "record", None),
        ("recordLock", 0x12c8c, "81 3d", "ff ff ff 7f", "record", "shutdown", 0x4c),
        ("gate4c", 0x129b6, "83 3d", "00", "record", None),
        ("gate68", 0x129bf, "83 3d", "00", "record", None),
        ("gate68", 0x9549, "87 3d", "", "notify", None),  # xchg [gate68],edi
        ("counter78", 0x15315, "ff 05", "", "record", None),
    ],
}
# No binary of these is at hand, so their data fields have only the header-level checks.
NO_BINARY = {"0.2.17"}


def strip_comments(text):
    return re.sub(r"//[^\n]*", "", text)


def parse_layouts():
    text = strip_comments(LAYOUT_H.read_text(encoding="utf-8"))
    body = re.search(r"struct AmdLayout\s*\{(.*?)\};", text, re.S).group(1)
    members = []
    for decl in re.findall(r"^\s*(?:const char\*|std::size_t|Sha256|std::uint32_t)\s+([^;]+);", body, re.M):
        members += [re.match(r"\s*(\w+)", part).group(1) for part in decl.split(",")]
    layouts = {}
    for var, init in re.findall(r"inline constexpr AmdLayout (\w+)\s*\{(.*?)\};", text, re.S):
        sha = re.search(r'Sha256FromHex\("([0-9a-fA-F]{64})"\)', init).group(1).lower()
        init = re.sub(r'Sha256FromHex\("[0-9a-fA-F]+"\)', "SHA", init)
        values = {}
        if re.search(r"^\s*\.", init):
            for key, value in re.findall(r"\.(\w+)\s*=\s*([^,]+)", init):
                values[key] = value.strip()
            order = [m for m in members if m in values]
            if order != list(values):
                raise ValueError(f"{var}: designated initializers out of declaration order")
        else:
            for key, value in zip(members, [v.strip() for v in init.split(",") if v.strip()]):
                values[key] = value
        name = values.pop("name").strip('"')
        size = int(values.pop("size"))
        values.pop("sha256")
        fields = {m: int(values.get(m, "0"), 0) for m in members if m not in ("name", "size", "sha256")}
        layouts[var] = {"name": name, "size": size, "sha": sha, "fields": fields}
    listed = re.search(r"kAmdLayouts\[\]\s*=\s*\{([^}]*)\}", text).group(1)
    listed = re.findall(r"&(\w+)", listed)
    return members, layouts, listed


def parse_bootstraps():
    text = strip_comments(HOSTLOAD_H.read_text(encoding="utf-8"))
    table = re.search(r"Bootstrap bootstraps\[\]\s*\{(.*?)\n\};", text, re.S).group(1)
    out = {}
    for var, start, ret, raw in re.findall(r"\{\s*&(\w+),\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*\{([^}]*)\}\s*\}",
                                           table):
        out[var] = (int(start, 16), int(ret, 16), bytes(int(b, 0) for b in raw.replace("\n", " ").split(",")))
    start_bytes = re.search(r"startBytes\s*\{([^}]*)\}", text).group(1)
    return out, bytes(int(b, 0) for b in start_bytes.split(","))


class Image:
    """A PE mapped the way the loader would, plus its sections, .pdata and import slots."""

    def __init__(self, data):
        e = struct.unpack_from("<I", data, 0x3C)[0]
        if data[e:e + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        count, opt_size = struct.unpack_from("<H", data, e + 6)[0], struct.unpack_from("<H", data, e + 20)[0]
        opt = e + 24
        self.base = struct.unpack_from("<Q", data, opt + 24)[0]
        size_of_image, size_of_headers = struct.unpack_from("<II", data, opt + 56)
        dirs = [struct.unpack_from("<II", data, opt + 112 + 8 * i) for i in range(16)]
        self.img = bytearray(size_of_image)
        self.img[:size_of_headers] = data[:size_of_headers]
        self.sections = {}
        for i in range(count):
            off = opt + opt_size + 40 * i
            name = data[off:off + 8].rstrip(b"\0").decode("latin1")
            vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, off + 8)
            self.img[va:va + min(rawsize, vsize or rawsize)] = data[raw:raw + min(rawsize, vsize or rawsize)]
            self.sections[name] = (va, max(vsize, rawsize))
        self.funcs = {}
        pva, psize = dirs[3]
        for off in range(pva, pva + psize, 12):
            begin, end, _ = struct.unpack_from("<III", self.img, off)
            if begin:
                self.funcs[begin] = end
        self.imports = {}
        for directory, delay in ((dirs[1], False), (dirs[13], True)):
            va, _ = directory
            while va:
                if delay:
                    attrs, dll, _, iat, names = struct.unpack_from("<IIIII", self.img, va)[:5]
                    step = 32
                else:
                    names, _, _, dll, iat = struct.unpack_from("<IIIII", self.img, va)
                    step = 20
                if not dll:
                    break
                dll_name = self.cstr(dll).lower()
                names = names or iat
                i = 0
                while True:
                    thunk = struct.unpack_from("<Q", self.img, names + 8 * i)[0]
                    if not thunk:
                        break
                    if not thunk >> 63:
                        self.imports[iat + 8 * i] = dll_name + "!" + self.cstr((thunk & 0x7FFFFFFF) + 2)
                    i += 1
                va += step

    def cstr(self, rva):
        end = self.img.index(0, rva)
        return self.img[rva:end].decode("latin1")

    def section(self, rva):
        for name, (va, size) in self.sections.items():
            if va <= rva < va + size:
                return name
        return None

    def rip(self, at, length):
        """Target of the rip-relative disp32 that ends `length` bytes into the instruction at `at`."""
        return at + length + struct.unpack_from("<i", self.img, at + length - 4)[0]

    def find_all(self, needle, start=0, end=None):
        end = len(self.img) if end is None else end
        at = self.img.find(needle, start, end)
        while at >= 0:
            yield at
            at = self.img.find(needle, at + 1, end)


def check_layout(image, layout, bootstrap, start_bytes, check):
    f = layout["fields"]
    text_va, text_size = image.sections[".text"]
    data_va, data_size = image.sections[".data"]
    for name in CODE:
        if f[name]:
            check(image.section(f[name]) == ".text" and f[name] in image.funcs,
                  f"{name} {f[name]:#x} is a .pdata function start in .text")
    if f["graphicsWaitBegin"]:
        end = image.funcs.get(f["graphicsWaitBegin"])
        check(f["graphicsWaitEnd"] == end, f"graphicsWaitEnd {f['graphicsWaitEnd']:#x} is the helper's .pdata end")
        for name in DISPATCH:
            rva = f[name]
            check(f["graphicsWaitBegin"] < rva < f["graphicsWaitEnd"] and image.img[rva - 3:rva] == b"\xff\x50\x70",
                  f"{name} {rva:#x} returns from call [rax+0x70] inside the wait helper")
        # The helper has exactly these four Dispatch calls, in this order (init, fallback spin,
        # sliced spin, finish), so two of them cannot trade places unnoticed.
        sites = [at + 3 for at in image.find_all(b"\xff\x50\x70", f["graphicsWaitBegin"], f["graphicsWaitEnd"])]
        check(sites == [f[n] for n in DISPATCH], "the helper's Dispatch returns, in order, are "
              + ", ".join(f"{s:#x}" for s in sites))
    else:
        check(not any(f[n] for n in DISPATCH) and not f["graphicsWaitEnd"], "no wait-helper fields without its start")

    data = sorted((n, v) for n, v in f.items() if n not in NOT_DATA and v)
    stray = [f"{n}={v:#x}" for n, v in data if not data_va <= v < data_va + data_size]
    if f["recordLock"] and not data_va <= f["recordLock"] + 0x4c < data_va + data_size:
        stray.append("recordLock+0x4c")
    check(not stray, f"{len(data)} data fields inside .data [{data_va:#x}..{data_va + data_size:#x})"
          + (f" -- outside: {', '.join(stray)}" if stray else ""))

    compile_slots = [rva for rva, name in image.imports.items() if name.endswith("!D3DCompile")]
    if f["d3dCompileIat"]:
        check(f["d3dCompileIat"] in compile_slots, f"d3dCompileIat {f['d3dCompileIat']:#x} is the D3DCompile slot")
    else:
        check(not compile_slots, "d3dCompileIat 0: the runtime imports no D3DCompile")

    # Record opens with cmp byte [enabled],1; test byte [nativeFailure],1; test byte [initDone],1.
    at, gates = f["record"], []
    for opcode in (b"\x80\x3d", b"\xf6\x05", b"\xf6\x05"):
        found = image.img.find(opcode, at, f["record"] + 0x80)
        if found < 0:
            break
        # opcode, modrm, disp32, imm8: the disp32 does not end the instruction.
        gates.append(found + 7 + struct.unpack_from("<i", image.img, found + 2)[0])
        at = found + 7
    want = [f["enabled"], f["nativeFailure"], f["initDone"]]
    check(gates == want, "Record gates on enabled, nativeFailure, initDone: "
          + ", ".join(f"{g:#x}" for g in gates))

    calls = [image.rip(at, 6) for at in image.find_all(b"\xff\x15", f["notify"], f["notify"] + 0x40)]
    check(f["trampoline"] in calls, f"Notify calls through trampoline {f['trampoline']:#x}")

    # INI reader: lea rdx,"<Key>" ... store to the field, before the next lea rcx,"DlssNrOnAmd".
    section_strings = {at for at in image.find_all(b"DlssNrOnAmd\0")}
    lea_rcx = sorted(at for at in image.find_all(b"\x48\x8d\x0d", text_va, text_va + text_size)
                     if image.rip(at, 7) in section_strings)
    lea_rdx = {}
    for at in image.find_all(b"\x48\x8d\x15", text_va, text_va + text_size):
        lea_rdx.setdefault(image.rip(at, 7), []).append(at)
    if not lea_rcx:
        check(False, "found the [DlssNrOnAmd] reader")
    for key, member in INI_KEYS.items():
        field = f.get(member, 0)
        if not field:
            continue
        stored = False
        for string in image.find_all(key.encode() + b"\0"):
            for site in lea_rdx.get(string, []):
                stop = next((r for r in lea_rcx if r > site), site + 0x80)
                stop = min(stop, site + 0x80)
                # A store to [rip+disp32]: ModRM 00 xxx 101 right before the disp32, after a
                # mov r/m,reg (88/89), a movss/movsd/movups store (0f 11), a setcc (0f 9x), or a
                # mov r/m,imm (c6 imm8, c7 imm32). The target must be the field itself.
                for p in range(site + 7, stop - 3):
                    if image.img[p - 1] & 0xC7 != 0x05:
                        continue
                    op = image.img[p - 2]
                    if op in (0x88, 0x89) or (image.img[p - 3] == 0x0F and (op == 0x11 or 0x90 <= op <= 0x9F)):
                        end = p + 4
                    elif op in (0xC6, 0xC7):
                        end = p + (5 if op == 0xC6 else 8)
                    else:
                        continue
                    if end + struct.unpack_from("<i", image.img, p)[0] == field:
                        stored = True
        check(stored, f"[DlssNrOnAmd] {key} is stored at {member} {field:#x}")

    check_anchors(image, layout, check)

    if bootstrap is None:
        check(False, "bootstrap isolation entry exists")
        return
    start, ret, call = bootstrap
    site = ret - len(call)
    check(bytes(image.img[site:ret]) == call, f"bootstrap call bytes at {site:#x}")
    check(image.rip(site, 7) == start, f"bootstrap lea r8 -> thread start {start:#x}")
    check(image.imports.get(image.rip(ret - 6, 6), "").endswith("kernel32.dll!CreateThread"),
          "bootstrap call goes through KERNEL32!CreateThread")
    check(start in image.funcs and bytes(image.img[start:start + len(start_bytes)]) == start_bytes,
          f"thread start {start:#x} is a .pdata start with the pinned prologue")


def unpinned_fields(layout):
    """Non-zero data fields that no INI store, behaviour check or pinned site covers."""
    covered = set(INI_KEYS.values()) | BEHAVIOUR_CHECKED | {a[0] for a in ANCHORS.get(layout["name"], [])}
    if layout["name"] in SEED_OFFSETS:
        covered |= {"seedCounter", "seedSelfCheck"}
    return sorted(n for n, v in layout["fields"].items() if v and n not in NOT_DATA and n not in covered)


def check_anchors(image, layout, check):
    f = layout["fields"]
    for anchor in ANCHORS.get(layout["name"], []):
        field, site, pre, suf, where, callee = anchor[:6]
        add = anchor[6] if len(anchor) > 6 else 0
        pre, suf = bytes.fromhex(pre), bytes.fromhex(suf)
        end = site + len(pre) + 4 + len(suf)
        shape = image.img[site:site + len(pre)] == pre and image.img[end - len(suf):end] == suf
        target = end + struct.unpack_from("<i", image.img, site + len(pre))[0]
        ok = shape and f[field] != 0 and target == f[field] + add
        said = f"{field} {f[field]:#x}: site {site:#x} [{pre.hex(' ')} disp32 {suf.hex(' ')}]".replace(" ]", "]")
        said += f" reaches {target:#x}" + (f" = {field}+{add:#x}" if add else "")
        if where:
            begin = f[where]
            ok = ok and begin <= site < image.funcs.get(begin, begin)
            said += f", inside {where}"
        if callee:
            # A direct call to the callee follows within 0x40 bytes.
            calls = [at + 5 + struct.unpack_from("<i", image.img, at + 1)[0]
                     for at in range(end, end + 0x40) if image.img[at] == 0xE8]
            ok = ok and f[callee] in calls
            said += f", then call {callee}"
        check(ok, said)


def main(argv):
    bad = []

    def check(ok, said):
        print(("  ok   " if ok else "  FAIL ") + said)
        if not ok:
            bad.append(said)

    members, layouts, listed = parse_layouts()
    bootstraps, start_bytes = parse_bootstraps()
    print(f"{LAYOUT_H.relative_to(ROOT).as_posix()}: {len(members)} members, {len(layouts)} tables\n")
    check(sorted(listed) == sorted(layouts), "every table is in kAmdLayouts: " + ", ".join(listed))
    check(len({l["name"] for l in layouts.values()}) == len(layouts), "runtime names are unique")
    check(len({(l["size"], l["sha"]) for l in layouts.values()}) == len(layouts), "size+SHA pairs are unique")
    check(sorted(bootstraps) == sorted(layouts), "every table has one bootstrap entry")
    for var, layout in layouts.items():
        check(len(layout["fields"]) == len(members) - 3, f"{var}: {len(layout['fields'])} fields parsed")
        seed = SEED_OFFSETS.get(layout["name"])
        if seed:
            f = layout["fields"]
            check((f["seedCounter"], f["seedSelfCheck"]) ==
                  (f["engine"] + seed[0], f["engine"] + seed[1]),
                  f"{var}: pinned seed offsets relative to engine")
        if layout["name"] in NO_BINARY:
            print(f"  note {var}: no {layout['name']} binary at hand, its data fields have no pinned site")
            continue
        loose = unpinned_fields(layout)
        check(not loose, f"{var}: every data field has an INI, behaviour or pinned-site check"
              + (f" -- none for: {', '.join(loose)}" if loose else ""))
        zero = sorted({a[0] for a in ANCHORS.get(layout["name"], []) if not layout["fields"].get(a[0])})
        check(not zero, f"{var}: pinned sites name only mapped fields" + (f" -- unmapped: {', '.join(zero)}" if zero else ""))

    files = []
    for arg in argv[1:]:
        path = Path(arg)
        files += sorted(p for p in path.rglob("*.dll")) if path.is_dir() else [path]
    by_id = {(l["size"], l["sha"]): var for var, l in layouts.items()}
    seen = set()
    for path in files:
        data = path.read_bytes()
        var = by_id.get((len(data), hashlib.sha256(data).hexdigest()))
        if not var or var in seen:
            continue
        seen.add(var)
        layout = layouts[var]
        print(f"\n{var} ({layout['name']}) <- {path}")
        check_layout(Image(data), layout, bootstraps.get(var), start_bytes, check)
    for var in layouts:
        if var not in seen:
            print(f"\n{var} ({layouts[var]['name']}): no matching binary given, not checked")
    if argv[1:] and not seen:
        check(False, "at least one given file is a known runtime")

    print("\n" + (f"PASS ({len(seen)} runtime binaries checked)" if not bad else f"FAIL: {len(bad)} check(s)"))
    return 0 if not bad else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
