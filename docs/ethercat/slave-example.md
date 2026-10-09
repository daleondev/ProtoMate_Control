# Minimal EtherCAT echo slave

Build preset: **`ethercat-slave-stm32`**. This standalone SOES example runs on
the NUCLEO-H753ZI and accesses the EVB-LAN9255 over SPI5. It starts automatically,
keeps the SAM flash erased, and has no robot functionality.

The master writes one 32-bit unsigned value and the slave echoes it back:

| Direction, from master's perspective | Object | PDO | Type |
|---|---|---|---|
| Output: `Command / Value` | `0x7000:01` | `0x1600`, SM2 | `UDINT`, 4 bytes |
| Input: `Response / Echo` | `0x6000:01` | `0x1A00`, SM3 | `UDINT`, 4 bytes |

Start with a **10 ms master cycle**, free-running synchronization. The firmware
polls the ESC from one thread and sleeps 1 ms between iterations. Echo updates
are asynchronous; allow several cycles after changing the command. No
distributed-clock synchronization, IRQ wiring, FoE or EoE is implemented.

## ESI and wiring

Install [ProtoMateEcho.xml](../../examples/ethercat_slave/ProtoMateEcho.xml).
The same XML generates the firmware's 2 KiB SII image and identity at build time:
vendor `0x00000000`, product `0x92550001`, revision `0x00000001`.
Vendor zero is an **unassigned lab identity**, not an ETG-assigned product identity.

Keep the [SPI test wiring](spi-test.md#then-evb-connection-and-straps),
**all four J17 straps at zero**, separate USB power and common ground. Connect
the EtherCAT master's Ethernet cable directly to **EVB J1**. IRQ/SYNC/reset
wires and any motor hardware are unnecessary. Keep Atmel-ICE disconnected.

The physical EEPROM is untouched. The STM32 supplies both the startup header
and subsequent EEPROM reads from its firmware image; no `boot` UART command is
needed. Virtual EEPROM writes are rejected. The slave supports CoE reads,
including identity and PDO mapping; process values are written through PDOs.

## Build and flash

```sh
cmake --preset ethercat-slave-stm32
cmake --build --preset ethercat-slave-stm32
openocd -f interface/stlink.cfg -f target/stm32h7x_dual_bank.cfg \
  -c "gdb_port disabled; tcl_port disabled; telnet_port disabled" \
  -c "program build/ethercat-slave-stm32/Application.elf verify reset exit"
```

The existing `ethercat-test-stm32` preset remains the interactive SPI diagnostic.
SOES must be present at `external/SOES`; its source is compiled without modifying
the submodule. The generated SII binary is
`build/ethercat-slave-stm32/examples/ethercat_slave/generated/ProtoMateEcho.bin`.
It is provided for inspection; do not program it into the physical EEPROM for
this emulated-EEPROM setup.

UART is 115200 baud, 8N1. State changes and a five-second status report show:
`AL=01` INIT, `02` PREOP, `04` SAFEOP, `08` OP. `error=0000` means no AL error.
The example has no UART commands. The shared motor enable remains inactive.

## TwinCAT

1. Copy `ProtoMateEcho.xml` into the TwinCAT installation's `Config\Io\EtherCAT`
   directory, commonly `C:\TwinCAT\3.1\Config\Io\EtherCAT`, then restart XAE.
   See [Beckhoff's ESI installation notes](https://infosys.beckhoff.com/content/1033/el6090/1036998411.html).
2. Select the Ethernet adapter physically connected to EVB J1 and install/select
   its TwinCAT real-time driver. Scan for the EtherCAT device and slave.
3. Expect **ProtoMate LAN9255 Echo**, with one four-byte output and input. Use
   free-run mode and a 10 ms task. Leave the fixed PDO assignment unchanged.
4. Link `Command / Value` to an output `UDINT` and `Response / Echo` to an input
   `UDINT` in a cyclic task. Activate the configuration and request OP.
5. Set the output to `12345678`; the input must become `12345678` within a few
   cycles. Repeat with `0` and `16#FFFFFFFF`.

For a small PLC program, declare and link these variables; change `Command`
online using **Write Values**:

```iecst
PROGRAM MAIN
VAR
    Command : UDINT := 0;
    ValueToSlave AT %Q* : UDINT;
    EchoFromSlave AT %I* : UDINT;
END_VAR

ValueToSlave := Command;
```

When PDO output traffic stops, the SOES software watchdog expires after 150
service iterations (not an exact millisecond duration), moves to SAFEOP+ERROR
(`AL=14`, error `001B`), and clears the local command/echo. Acknowledge the error
and resume cyclic data to return to OP. The master may retain its last input
sample while communication is stopped; use EtherCAT state/WKC to judge validity.
SPI failures halt the example and print `STOPPED`; restore wiring/power and
reset the Nucleo. This is a bench example, not production recovery logic.

## Linux verification

[test_master.py](../../examples/ethercat_slave/test_master.py) uses PySOEM on a
dedicated Ethernet adapter. It verifies discovery, the entire emulated EEPROM,
CoE identity, PDO sizes, state transitions, 1,000 cycles, watchdog expiry and
recovery, then returns the slave to INIT. Do not run it alongside TwinCAT.

```sh
python -m venv /tmp/protomate-ethercat-venv
/tmp/protomate-ethercat-venv/bin/pip install pysoem
sudo /tmp/protomate-ethercat-venv/bin/python \
  examples/ethercat_slave/test_master.py enp17s0u1c2
```

`enp17s0u1c2` is the USB Ethernet adapter used on this bench; use the actual
connected interface on another PC. Linux requires root or suitable raw-socket
capabilities. An Ethernet IP address is unnecessary.

Verified on the connected boards on 2026-10-09: INIT → PREOP → SAFEOP → OP,
full 2 KiB SII readback, CoE identity and mapping, 1,000 PDO cycles with expected
WKC **3** and correct echoes, watchdog clearing and recovery. TwinCAT itself
has not been exercised here. The ESI also validates against the EtherCATInfo
XML schema; host tests check CRC, identity, category layout and transport bounds.

The compact transport uses the LAN9253 CSR interface for both registers and
process RAM, as supported by datasheet §11.13.1. It favors simplicity over
throughput; faster SPI/FIFO transfers and DC support are later work.
