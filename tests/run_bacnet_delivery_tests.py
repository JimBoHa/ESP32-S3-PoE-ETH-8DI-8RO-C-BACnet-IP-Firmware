#!/usr/bin/env python3
"""Compile the pinned BACnet implementation into an isolated, no-network test."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "third_party/bacnet-stack/src"
SOURCES = [
    "basic/service/h_cov.c", "basic/service/h_apdu.c", "basic/tsm/tsm.c", "basic/object/bi.c", "basic/object/bo.c",
    "basic/sys/keylist.c", "basic/sys/debug.c", "basic/sys/days.c",
    "bacaction.c", "bacaddr.c", "bacapp.c", "bacdcode.c", "bacdest.c",
    "bacdevobjpropref.c", "bacint.c", "bacreal.c", "bacstr.c", "bacerror.c", "bactext.c",
    "cov.c", "datetime.c", "hostnport.c", "indtext.c", "memcopy.c",
    "npdu.c", "proplist.c", "timestamp.c", "wp.c", "abort.c", "dcc.c", "reject.c",
]
DEFINES = [
    "BACDL_BIP=1", "BACAPP_MINIMAL=1", "BACAPP_HOST_N_PORT=1",
    "BACNET_STACK_DEPRECATED_DISABLE=1", "BACNET_PROTOCOL_REVISION=28",
    "MAX_COV_SUBSCRIPTIONS=16", "MAX_COV_ADDRESSES=8", "MAX_TSM_TRANSACTIONS=8",
    "CONFIG_BACNET_BASIC_COV_SUBSCRIPTIONS_SIZE=16", "PRINT_ENABLED=0", "BBMD_ENABLED=0",
]

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="bacnet-delivery-") as temporary:
        executable = Path(temporary) / "delivery"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-g", "-O1",
            "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-format", "-Wno-sign-compare",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            *("-D" + define for define in DEFINES), "-I" + str(SRC), "-I" + str(ROOT / "main"),
            str(ROOT / "tests/test_bacnet_delivery.c"),
            str(ROOT / "main/bacnet_cov_recovery.c"),
            *(str(SRC / "bacnet" / source) for source in SOURCES),
            "-lm", "-o", str(executable),
        ], check=True)
        for mode in ([], ["recovery"]):
            print("Mode:", "patched recovery" if mode else "unmodified upstream behavior", flush=True)
            for scenario in ("lost-change", "pending-change", "polarity", "commands", "healthy", "starvation", "cancel", "expiry", "superseded", "ignore-write"):
                subprocess.run([str(executable), scenario, *mode], check=True, timeout=15)
