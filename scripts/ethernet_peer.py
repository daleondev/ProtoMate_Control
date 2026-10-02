#!/usr/bin/env python3
"""Raw-frame peer for the STM32 Ethernet cable test (Linux, standard library only).

Run with sudo solely to open AF_PACKET on the requested interface. The process
drops back to the invoking user before processing packets. It only responds to
reference-compatible test frames with experimental EtherType 0x88b5 and magic SCET.
"""

import argparse
import errno
import json
import os
from pathlib import Path
import signal
import socket
import struct
import time

ETHERTYPE = 0x88B5
SOL_PACKET = 263
PACKET_AUXDATA = 8
TP_STATUS_VLAN_VALID = 1 << 4
TP_STATUS_VLAN_TPID_VALID = 1 << 6


def frame_payload(frame):
    offset = 12
    ethertype = int.from_bytes(frame[offset:offset + 2], "big")
    while ethertype in (0x8100, 0x88A8):
        offset += 4
        if len(frame) < offset + 2:
            return None
        ethertype = int.from_bytes(frame[offset:offset + 2], "big")
    offset += 2
    if ethertype != ETHERTYPE or frame[offset:offset + 4] != b"SCET":
        return None
    return offset


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interface", required=True)
    parser.add_argument("--log", default="/tmp/nucleo-h753zi-ethernet-peer.json")
    parser.add_argument("--seconds", type=float, default=3600)
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    mac = bytes.fromhex(Path(f"/sys/class/net/{args.interface}/address").read_text().strip().replace(":", ""))
    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
    sock.bind((args.interface, 0))
    sock.setsockopt(SOL_PACKET, PACKET_AUXDATA, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2 * 1024 * 1024)
    sock.settimeout(0.5)

    if os.geteuid() == 0 and "SUDO_UID" in os.environ:
        os.setgroups([])
        os.setgid(int(os.environ["SUDO_GID"]))
        os.setuid(int(os.environ["SUDO_UID"]))

    def terminate(*_):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, terminate)

    state = {"interface": args.interface, "mac": mac.hex(":"), "ready": True,
             "received": 0, "sent": 0, "errors": [], "sizes": [], "boards": []}
    log = Path(args.log)

    def publish():
        log.write_text(json.dumps(state, indent=2) + "\n")

    def send(frame):
        try:
            sock.send(frame)
        except OSError as error:
            if error.errno != errno.EMSGSIZE:
                raise
            message = f"host rejected {len(frame)}-byte reply: {error}"
            state["errors"].append(message)
            publish()
            print(message, flush=True)
            return
        state["sent"] += 1

    publish()
    print(f"Ready on {args.interface} ({mac.hex(':')}); log: {log}", flush=True)
    deadline = time.monotonic() + args.seconds
    try:
        while time.monotonic() < deadline:
            try:
                data, ancillary, _, address = sock.recvmsg(2048, 128)
            except TimeoutError:
                continue
            if address[2] == socket.PACKET_OUTGOING or len(data) < 25:
                continue
            # Some NICs remove the outer VLAN header before AF_PACKET delivery.
            for level, kind, aux in ancillary:
                if level == SOL_PACKET and kind == PACKET_AUXDATA and len(aux) >= 20:
                    status, _, _, _, _, tci, tpid = struct.unpack("=IIIHHHH", aux[:20])
                    if status & TP_STATUS_VLAN_VALID:
                        if not status & TP_STATUS_VLAN_TPID_VALID:
                            tpid = 0x8100
                        data = data[:12] + struct.pack("!HH", tpid, tci) + data[12:]
            offset = frame_payload(data)
            if offset is None or len(data) < offset + 11 or data[offset + 4] >= 128:
                continue
            if data[:6] not in (mac, b"\xff" * 6):
                continue
            operation = data[offset + 4]
            sequence = int.from_bytes(data[offset + 5:offset + 9], "big")
            declared = int.from_bytes(data[offset + 9:offset + 11], "big")
            expected_size = max(60, declared)
            error = None
            if not offset + 11 <= declared <= 1522 or len(data) != expected_size:
                error = f"seq={sequence}: length={len(data)}, declared={declared}"
            else:
                expected = bytes(((sequence * 31 + index * 17) & 255)
                                 for index in range(11, declared - offset))
                if data[offset + 11:declared] != expected or any(data[declared:]):
                    error = f"seq={sequence}: payload corruption"
            if error:
                state["errors"].append(error)
                publish()
                print(error, flush=True)
                continue

            state["received"] += 1
            if len(data) not in state["sizes"]:
                state["sizes"].append(len(data))
            board = data[6:12].hex(":")
            if board not in state["boards"]:
                state["boards"].append(board)
            reply = bytearray(data)
            reply[:6], reply[6:12] = data[6:12], mac
            reply[offset + 4] |= 128
            if operation == 7:
                # A 1500-MTU Linux adapter may receive the maximum QinQ frame
                # but cannot send it through AF_PACKET. Acknowledge its fully
                # validated contents using a shorter frame instead.
                reply = reply[:64]
                reply[offset + 9:offset + 11] = (64).to_bytes(2, "big")
            if operation == 3:  # Wrong EtherType before the valid reply.
                wrong = bytearray(reply)
                wrong[offset - 2:offset] = b"\x88\xb6"
                send(wrong)
            if operation == 4:  # Oversized-for-caller packet, then another valid one.
                send(reply)
            if operation == 5:  # Fill the board's RX ring before a stop/restart.
                for _ in range(4):
                    send(reply)
            send(reply)
            if state["received"] % 64 == 0 or operation in (1, 6):
                publish()
                print(f"frames received={state['received']} sent={state['sent']}", flush=True)
    except KeyboardInterrupt:
        pass
    except OSError as error:
        state["errors"].append(str(error))
        raise
    finally:
        state["ready"] = False
        publish()
        sock.close()


if __name__ == "__main__":
    main()
