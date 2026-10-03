#!/usr/bin/env python3
"""
Full-screen STM32 serial terminal with ANSI/VT100 rendering.

Install:
    python -m pip install pyserial
    # Windows only:
    python -m pip install windows-curses

Examples:
    python serial_tui.py
    python serial_tui.py /dev/ttyACM0 -b 115200
    python serial_tui.py COM5 -b 921600 --newline crlf --log stm32.raw.log

The log file stores the exact received bytes, including ANSI escape sequences.
Press F1 in the application for the complete key reference.
"""

from __future__ import annotations

import argparse
import codecs
import curses
import locale
import os
import queue
import signal
import sys
import threading
import time
import unicodedata
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import BinaryIO, Iterable, Optional, Union

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None


APP_NAME = "STM32 Serial TUI"
COMMON_BAUDS = [
    9_600,
    19_200,
    38_400,
    57_600,
    115_200,
    230_400,
    460_800,
    500_000,
    576_000,
    921_600,
    1_000_000,
    1_500_000,
    2_000_000,
]


@dataclass(frozen=True)
class Style:
    """Terminal rendition state. Colors are xterm indexes or -1 for default."""

    fg: int = -1
    bg: int = -1
    bold: bool = False
    dim: bool = False
    italic: bool = False
    underline: bool = False
    blink: bool = False
    reverse: bool = False


DEFAULT_STYLE = Style()


@dataclass
class Cell:
    text: str = " "
    style: Style = DEFAULT_STYLE
    continuation: bool = False


def display_width(ch: str) -> int:
    """Return a practical terminal-cell width without an external dependency."""

    if not ch or unicodedata.combining(ch):
        return 0
    if unicodedata.category(ch) in ("Cc", "Cf"):
        return 0
    return 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1


class TerminalBuffer:
    """A bounded virtual terminal with scrollback and common VT100 operations."""

    def __init__(self, width: int = 80, max_lines: int = 10_000) -> None:
        self.width = max(1, width)
        self.max_lines = max(100, max_lines)
        self.lines: list[list[Cell]] = [[]]
        self.cursor_row = 0
        self.cursor_col = 0
        self.saved_cursor = (0, 0)
        self.style = DEFAULT_STYLE
        self.title = ""
        self.bell_count = 0

    def set_width(self, width: int) -> None:
        # Existing output stays stable; new output wraps at the new width.
        self.width = max(1, width)
        self.cursor_col = min(self.cursor_col, self.width - 1)

    def clear(self) -> None:
        self.lines = [[]]
        self.cursor_row = 0
        self.cursor_col = 0
        self.saved_cursor = (0, 0)
        self.style = DEFAULT_STYLE

    def _line(self, row: Optional[int] = None) -> list[Cell]:
        target = self.cursor_row if row is None else max(0, row)
        while len(self.lines) <= target:
            self.lines.append([])
        return self.lines[target]

    @staticmethod
    def _blank(style: Style = DEFAULT_STYLE) -> Cell:
        return Cell(" ", style)

    def _ensure_col(self, line: list[Cell], col: int) -> None:
        if col >= len(line):
            line.extend(self._blank(self.style) for _ in range(col + 1 - len(line)))

    def _trim_scrollback(self) -> None:
        overflow = len(self.lines) - self.max_lines
        if overflow > 0:
            del self.lines[:overflow]
            self.cursor_row = max(0, self.cursor_row - overflow)
            saved_row, saved_col = self.saved_cursor
            self.saved_cursor = (max(0, saved_row - overflow), saved_col)

    def _newline(self) -> None:
        self.cursor_row += 1
        self.cursor_col = 0
        self._line()
        self._trim_scrollback()

    def put_char(self, ch: str) -> None:
        width = display_width(ch)
        if width == 0:
            if unicodedata.combining(ch) and self.cursor_col > 0:
                line = self._line()
                index = min(self.cursor_col - 1, len(line) - 1)
                while index >= 0 and line[index].continuation:
                    index -= 1
                if index >= 0:
                    line[index].text += ch
            return

        if self.cursor_col >= self.width or (
            width == 2 and self.cursor_col == self.width - 1
        ):
            self._newline()

        line = self._line()
        self._ensure_col(line, self.cursor_col)

        # Overwriting either half of a wide character clears the other half.
        if line[self.cursor_col].continuation and self.cursor_col > 0:
            line[self.cursor_col - 1] = self._blank(self.style)
        elif (
            self.cursor_col + 1 < len(line)
            and line[self.cursor_col + 1].continuation
        ):
            line[self.cursor_col + 1] = self._blank(self.style)

        line[self.cursor_col] = Cell(ch, self.style)
        if width == 2:
            self._ensure_col(line, self.cursor_col + 1)
            line[self.cursor_col + 1] = Cell("", self.style, continuation=True)
        self.cursor_col += width

    def control(self, ch: str) -> None:
        if ch == "\n":
            self._newline()
        elif ch == "\r":
            self.cursor_col = 0
        elif ch == "\b":
            self.cursor_col = max(0, self.cursor_col - 1)
        elif ch == "\t":
            next_tab = min(self.width, ((self.cursor_col // 8) + 1) * 8)
            while self.cursor_col < next_tab:
                self.put_char(" ")
        elif ch == "\a":
            self.bell_count += 1

    def erase_previous_character(self) -> None:
        """Apply local terminal-style erase for an outgoing Backspace/DEL."""

        if self.cursor_col <= 0:
            return

        line = self._line()
        target = self.cursor_col - 1
        if target < len(line) and line[target].continuation:
            target = max(0, target - 1)

        cell_width = 1
        if target + 1 < len(line) and line[target + 1].continuation:
            cell_width = 2
        for index in range(target, min(len(line), target + cell_width)):
            line[index] = self._blank(self.style)

        self.cursor_col = target
        while line and line[-1].text == " " and not line[-1].continuation:
            line.pop()

    def cursor_up(self, count: int) -> None:
        self.cursor_row = max(0, self.cursor_row - max(1, count))

    def cursor_down(self, count: int) -> None:
        self.cursor_row += max(1, count)
        self._line()
        self._trim_scrollback()

    def cursor_forward(self, count: int) -> None:
        self.cursor_col = min(self.width - 1, self.cursor_col + max(1, count))

    def cursor_back(self, count: int) -> None:
        self.cursor_col = max(0, self.cursor_col - max(1, count))

    def cursor_position(self, row: int, col: int) -> None:
        # ANSI positions are 1-based and apply to the active terminal area.
        self.cursor_row = max(0, row - 1)
        self.cursor_col = min(self.width - 1, max(0, col - 1))
        self._line()

    def erase_line(self, mode: int) -> None:
        line = self._line()
        if mode == 0:
            if self.cursor_col < len(line):
                del line[self.cursor_col :]
        elif mode == 1:
            self._ensure_col(line, self.cursor_col)
            for index in range(self.cursor_col + 1):
                line[index] = self._blank(self.style)
        elif mode == 2:
            line.clear()

    def erase_display(self, mode: int) -> None:
        if mode == 2:
            # ED clears cells but does not reset SGR state or move the cursor.
            self.lines = [[] for _ in range(self.cursor_row + 1)]
            self._line()
            return
        if mode == 3:
            # ED 3 clears saved scrollback. With no separate screen/history
            # boundary, preserve the active cursor row and discard older rows.
            active = self._line()
            self.lines = [active]
            self.cursor_row = 0
            self.saved_cursor = (0, self.saved_cursor[1])
            return
        if mode == 0:
            self.erase_line(0)
            del self.lines[self.cursor_row + 1 :]
        elif mode == 1:
            for row in range(self.cursor_row):
                self.lines[row] = []
            self.erase_line(1)

    def delete_chars(self, count: int) -> None:
        line = self._line()
        count = max(1, count)
        if self.cursor_col < len(line):
            del line[self.cursor_col : self.cursor_col + count]

    def erase_chars(self, count: int) -> None:
        line = self._line()
        count = max(1, count)
        self._ensure_col(line, min(self.width - 1, self.cursor_col + count - 1))
        for index in range(self.cursor_col, min(self.width, self.cursor_col + count)):
            line[index] = self._blank(self.style)

    def insert_blanks(self, count: int) -> None:
        line = self._line()
        count = max(1, count)
        self._ensure_col(line, self.cursor_col)
        blanks = [self._blank(self.style) for _ in range(count)]
        line[self.cursor_col : self.cursor_col] = blanks
        del line[self.width :]

    def set_sgr(self, raw_params: list[str]) -> None:
        params: list[int] = []
        for raw in raw_params:
            # ISO 8613-6 colon forms are normalized to semicolon-like fields.
            parts = raw.split(":")
            if len(parts) >= 3 and parts[0] in ("38", "48") and parts[1] == "5":
                # 38:5:index
                parts = [parts[0], "5", parts[-1]]
            elif len(parts) >= 5 and parts[0] in ("38", "48") and parts[1] == "2":
                # Both 38:2:r:g:b and 38:2::r:g:b are in use.
                parts = [parts[0], "2", *parts[-3:]]
            for part in parts:
                try:
                    params.append(int(part) if part else 0)
                except ValueError:
                    params.append(0)
        if not params:
            params = [0]

        style = self.style
        index = 0
        while index < len(params):
            code = params[index]
            if code == 0:
                style = DEFAULT_STYLE
            elif code == 1:
                style = replace(style, bold=True)
            elif code == 2:
                style = replace(style, dim=True)
            elif code == 3:
                style = replace(style, italic=True)
            elif code == 4:
                style = replace(style, underline=True)
            elif code in (5, 6):
                style = replace(style, blink=True)
            elif code == 7:
                style = replace(style, reverse=True)
            elif code in (21, 22):
                style = replace(style, bold=False, dim=False)
            elif code == 23:
                style = replace(style, italic=False)
            elif code == 24:
                style = replace(style, underline=False)
            elif code == 25:
                style = replace(style, blink=False)
            elif code == 27:
                style = replace(style, reverse=False)
            elif 30 <= code <= 37:
                style = replace(style, fg=code - 30)
            elif code == 39:
                style = replace(style, fg=-1)
            elif 40 <= code <= 47:
                style = replace(style, bg=code - 40)
            elif code == 49:
                style = replace(style, bg=-1)
            elif 90 <= code <= 97:
                style = replace(style, fg=code - 90 + 8)
            elif 100 <= code <= 107:
                style = replace(style, bg=code - 100 + 8)
            elif code in (38, 48):
                is_fg = code == 38
                if index + 2 < len(params) and params[index + 1] == 5:
                    color = max(0, min(255, params[index + 2]))
                    style = replace(style, **({"fg": color} if is_fg else {"bg": color}))
                    index += 2
                elif index + 4 < len(params) and params[index + 1] == 2:
                    red, green, blue = (
                        max(0, min(255, value))
                        for value in params[index + 2 : index + 5]
                    )
                    color = rgb_to_xterm(red, green, blue)
                    style = replace(style, **({"fg": color} if is_fg else {"bg": color}))
                    index += 4
            index += 1
        self.style = style

    def handle_csi(self, params_text: str, final: str) -> None:
        private = params_text.startswith(("?", ">", "!"))
        if private:
            params_text = params_text[1:]
        raw_params = params_text.split(";") if params_text else []

        def number(position: int = 0, default: int = 1) -> int:
            if position >= len(raw_params) or raw_params[position] == "":
                return default
            try:
                return int(raw_params[position])
            except ValueError:
                return default

        if final == "m":
            self.set_sgr(raw_params)
        elif final == "A":
            self.cursor_up(number())
        elif final == "B":
            self.cursor_down(number())
        elif final == "C":
            self.cursor_forward(number())
        elif final == "D":
            self.cursor_back(number())
        elif final == "E":
            self.cursor_down(number())
            self.cursor_col = 0
        elif final == "F":
            self.cursor_up(number())
            self.cursor_col = 0
        elif final in ("G", "`"):
            self.cursor_col = min(self.width - 1, max(0, number(default=1) - 1))
        elif final in ("H", "f"):
            self.cursor_position(number(0, 1), number(1, 1))
        elif final == "J":
            self.erase_display(number(default=0))
        elif final == "K":
            self.erase_line(number(default=0))
        elif final == "P":
            self.delete_chars(number())
        elif final == "X":
            self.erase_chars(number())
        elif final == "@":
            self.insert_blanks(number())
        elif final == "s":
            self.saved_cursor = (self.cursor_row, self.cursor_col)
        elif final == "u":
            self.cursor_row, self.cursor_col = self.saved_cursor
            self._line()
        # DEC private modes (cursor visibility, alternate screen, etc.) are
        # intentionally ignored: the TUI owns the physical terminal.

    def visible_lines(self, height: int, scroll_offset: int = 0) -> list[list[Cell]]:
        end = max(0, len(self.lines) - max(0, scroll_offset))
        start = max(0, end - max(0, height))
        result = self.lines[start:end]
        if len(result) < height:
            # A terminal starts at its top-left corner. Blank rows belong below
            # early output; once the area fills, normal scrollback takes over.
            result = result + ([[]] * (height - len(result)))
        return result

    def plain_text(self) -> str:
        rendered = []
        for line in self.lines:
            rendered.append("".join(cell.text for cell in line if not cell.continuation).rstrip())
        return "\n".join(rendered)


def rgb_to_xterm(red: int, green: int, blue: int) -> int:
    """Quantize 24-bit RGB to the closest xterm-256 palette entry."""

    if red == green == blue:
        if red < 8:
            return 16
        if red > 248:
            return 231
        return 232 + round((red - 8) / 247 * 24)

    def component(value: int) -> int:
        return max(0, min(5, round(value / 255 * 5)))

    return 16 + 36 * component(red) + 6 * component(green) + component(blue)


XTERM_16_RGB = (
    (0, 0, 0),
    (205, 0, 0),
    (0, 205, 0),
    (205, 205, 0),
    (0, 0, 238),
    (205, 0, 205),
    (0, 205, 205),
    (229, 229, 229),
    (127, 127, 127),
    (255, 0, 0),
    (0, 255, 0),
    (255, 255, 0),
    (92, 92, 255),
    (255, 0, 255),
    (0, 255, 255),
    (255, 255, 255),
)


def xterm_to_rgb(color: int) -> tuple[int, int, int]:
    color = max(0, min(255, color))
    if color < 16:
        return XTERM_16_RGB[color]
    if color < 232:
        color -= 16
        levels = (0, 95, 135, 175, 215, 255)
        return levels[color // 36], levels[(color // 6) % 6], levels[color % 6]
    level = 8 + (color - 232) * 10
    return level, level, level


def xterm_to_ansi(color: int, palette_size: int) -> int:
    """Map an xterm color to the perceptually closest 8/16-color entry."""

    palette_size = 16 if palette_size >= 16 else 8
    if color < palette_size:
        return color
    red, green, blue = xterm_to_rgb(color)
    return min(
        range(palette_size),
        key=lambda index: sum(
            (actual - wanted) ** 2
            for actual, wanted in zip(XTERM_16_RGB[index], (red, green, blue))
        ),
    )


class AnsiParser:
    """Incremental parser for text, CSI, OSC, and two-byte escape sequences."""

    def __init__(self, terminal: TerminalBuffer) -> None:
        self.terminal = terminal
        self.state = "text"
        self.buffer = ""
        self.osc = ""

    def feed(self, text: str) -> None:
        for ch in text:
            if self.state == "text":
                if ch == "\x1b":
                    self.state = "esc"
                elif ch in "\n\r\b\t\a":
                    self.terminal.control(ch)
                elif ord(ch) >= 0x20 and ch != "\x7f":
                    self.terminal.put_char(ch)
            elif self.state == "esc":
                if ch == "[":
                    self.buffer = ""
                    self.state = "csi"
                elif ch == "]":
                    self.osc = ""
                    self.state = "osc"
                elif ch == "7":
                    self.terminal.saved_cursor = (
                        self.terminal.cursor_row,
                        self.terminal.cursor_col,
                    )
                    self.state = "text"
                elif ch == "8":
                    (
                        self.terminal.cursor_row,
                        self.terminal.cursor_col,
                    ) = self.terminal.saved_cursor
                    self.state = "text"
                elif ch == "c":
                    self.terminal.clear()
                    self.state = "text"
                elif ch in ("D", "E"):
                    self.terminal.control("\n")
                    self.state = "text"
                elif ch == "M":
                    self.terminal.cursor_up(1)
                    self.state = "text"
                elif ch == "\x1b":
                    self.state = "esc"
                else:
                    self.state = "text"
            elif self.state == "csi":
                if "\x40" <= ch <= "\x7e":
                    self.terminal.handle_csi(self.buffer, ch)
                    self.buffer = ""
                    self.state = "text"
                elif ch == "\x1b":
                    self.state = "esc"
                elif len(self.buffer) < 128:
                    self.buffer += ch
                else:
                    self.buffer = ""
                    self.state = "text"
            elif self.state == "osc":
                if ch == "\a":
                    self._finish_osc()
                elif ch == "\x1b":
                    self.state = "osc_esc"
                elif len(self.osc) < 1024:
                    self.osc += ch
            elif self.state == "osc_esc":
                if ch == "\\":
                    self._finish_osc()
                else:
                    if len(self.osc) < 1023:
                        self.osc += "\x1b" + ch
                    self.state = "osc"

    def _finish_osc(self) -> None:
        if ";" in self.osc:
            command, value = self.osc.split(";", 1)
            if command in ("0", "1", "2"):
                self.terminal.title = value
        self.osc = ""
        self.state = "text"


@dataclass(frozen=True)
class SerialConfig:
    port: str
    baud: int = 115_200
    bytesize: int = 8
    parity: str = "N"
    stopbits: float = 1
    rtscts: bool = False
    xonxoff: bool = False
    dsrdtr: bool = False
    reconnect: bool = True
    reset_pulse_ms: int = 0
    exclusive: bool = False


@dataclass(frozen=True)
class WorkerEvent:
    kind: str
    payload: Union[bytes, str, None] = None
    when: float = field(default_factory=time.monotonic)


class SerialWorker(threading.Thread):
    """Own the serial object so all device I/O remains outside curses."""

    def __init__(self, config: SerialConfig) -> None:
        super().__init__(name="stm32-serial", daemon=True)
        self.config = config
        self.events: queue.Queue[WorkerEvent] = queue.Queue()
        self.tx: queue.Queue[bytes] = queue.Queue()
        self.stop_event = threading.Event()
        self.reopen_event = threading.Event()
        self.connected = False
        self.rx_bytes = 0
        self.tx_bytes = 0
        self._serial = None

    def send(self, data: bytes) -> None:
        if data:
            self.tx.put(data)

    def request_reconnect(self) -> None:
        self.reopen_event.set()

    def stop(self) -> None:
        self.stop_event.set()
        self.reopen_event.set()

    def _emit(self, kind: str, payload: Union[bytes, str, None] = None) -> None:
        self.events.put(WorkerEvent(kind, payload))

    def _open(self):
        kwargs = dict(
            port=self.config.port,
            baudrate=self.config.baud,
            bytesize=self.config.bytesize,
            parity=self.config.parity,
            stopbits=self.config.stopbits,
            timeout=0.05,
            write_timeout=0.5,
            rtscts=self.config.rtscts,
            xonxoff=self.config.xonxoff,
            dsrdtr=self.config.dsrdtr,
        )
        if os.name != "nt" and self.config.exclusive:
            kwargs["exclusive"] = True
        handle = serial.Serial(**kwargs)
        if self.config.reset_pulse_ms:
            handle.dtr = False
            time.sleep(self.config.reset_pulse_ms / 1000)
            handle.reset_input_buffer()
            handle.dtr = True
        return handle

    def _close(self) -> None:
        if self._serial is not None:
            try:
                self._serial.close()
            except Exception:
                pass
            self._serial = None
        if self.connected:
            self.connected = False
            self._emit("disconnected")

    def run(self) -> None:
        retry_at = 0.0
        last_error = ""
        while not self.stop_event.is_set():
            if self.reopen_event.is_set():
                self.reopen_event.clear()
                self._close()
                retry_at = 0.0

            if self._serial is None:
                if time.monotonic() < retry_at:
                    self.stop_event.wait(0.05)
                    continue
                try:
                    self._emit("connecting")
                    self._serial = self._open()
                    self.connected = True
                    last_error = ""
                    self._emit("connected")
                except Exception as exc:
                    message = str(exc)
                    if message != last_error:
                        self._emit("error", message)
                        last_error = message
                    if not self.config.reconnect:
                        break
                    retry_at = time.monotonic() + 1.0
                    continue

            try:
                waiting = self._serial.in_waiting
                data = self._serial.read(max(1, min(waiting, 65_536)))
                if data:
                    self.rx_bytes += len(data)
                    self._emit("data", data)

                while True:
                    try:
                        outgoing = self.tx.get_nowait()
                    except queue.Empty:
                        break
                    self._serial.write(outgoing)
                    self._serial.flush()
                    self.tx_bytes += len(outgoing)
                    self._emit("sent", outgoing)
            except Exception as exc:
                self._emit("error", str(exc))
                self._close()
                retry_at = time.monotonic() + 1.0
                if not self.config.reconnect:
                    break

        self._close()
        self._emit("stopped")


class ColorManager:
    """Map ANSI styles to curses attributes and cache color pairs."""

    def __init__(self) -> None:
        self.enabled = False
        self.colors = 0
        self.max_pairs = 1
        self.next_pair = 1
        self.pairs: dict[tuple[int, int], int] = {}
        self.ui_pairs: dict[str, int] = {}
        try:
            curses.start_color()
            curses.use_default_colors()
            self.enabled = curses.has_colors()
            self.colors = curses.COLORS if self.enabled else 0
            self.max_pairs = curses.COLOR_PAIRS if self.enabled else 1
        except curses.error:
            return

        self.ui_pairs["header"] = self._allocate(0, 6)
        self.ui_pairs["ok"] = self._allocate(2, -1)
        self.ui_pairs["warn"] = self._allocate(3, -1)
        self.ui_pairs["error"] = self._allocate(1, -1)
        self.ui_pairs["hint"] = self._allocate(6, -1)
        self.ui_pairs["selection"] = self._allocate(0, 6)

    def _map_color(self, color: int) -> int:
        if color < 0:
            return -1
        if self.colors >= 256:
            return min(color, 255)
        if self.colors >= 16:
            return xterm_to_ansi(color, 16)
        if self.colors >= 8:
            return xterm_to_ansi(color, 8)
        return -1

    def _allocate(self, fg: int, bg: int) -> int:
        if not self.enabled or self.next_pair >= self.max_pairs:
            return 0
        key = (self._map_color(fg), self._map_color(bg))
        if key in self.pairs:
            return self.pairs[key]
        pair = self.next_pair
        try:
            curses.init_pair(pair, key[0], key[1])
        except curses.error:
            return 0
        self.pairs[key] = pair
        self.next_pair += 1
        return pair

    def ui(self, name: str, extra: int = 0) -> int:
        pair = self.ui_pairs.get(name, 0)
        return (curses.color_pair(pair) if pair else 0) | extra

    def attr(self, style: Style) -> int:
        fg, bg = style.fg, style.bg
        if style.reverse:
            fg, bg = bg, fg
        pair = self._allocate(fg, bg)
        attr = curses.color_pair(pair) if pair else 0
        if style.bold or (self.colors < 16 and fg >= 8):
            attr |= curses.A_BOLD
        if style.dim:
            attr |= curses.A_DIM
        if style.underline:
            attr |= curses.A_UNDERLINE
        if style.blink:
            attr |= curses.A_BLINK
        if style.italic and hasattr(curses, "A_ITALIC"):
            attr |= curses.A_ITALIC
        if style.reverse and pair == 0:
            attr |= curses.A_REVERSE
        return attr


class LineEditor:
    def __init__(self) -> None:
        self.text = ""
        self.cursor = 0
        self.history: list[str] = []
        self.history_index: Optional[int] = None
        self.saved_text = ""

    def insert(self, text: str) -> None:
        self.text = self.text[: self.cursor] + text + self.text[self.cursor :]
        self.cursor += len(text)

    def backspace(self) -> None:
        if self.cursor:
            self.text = self.text[: self.cursor - 1] + self.text[self.cursor :]
            self.cursor -= 1

    def delete(self) -> None:
        if self.cursor < len(self.text):
            self.text = self.text[: self.cursor] + self.text[self.cursor + 1 :]

    def commit(self) -> str:
        value = self.text
        if value and (not self.history or self.history[-1] != value):
            self.history.append(value)
            del self.history[:-500]
        self.text = ""
        self.cursor = 0
        self.history_index = None
        self.saved_text = ""
        return value

    def previous(self) -> None:
        if not self.history:
            return
        if self.history_index is None:
            self.saved_text = self.text
            self.history_index = len(self.history) - 1
        else:
            self.history_index = max(0, self.history_index - 1)
        self.text = self.history[self.history_index]
        self.cursor = len(self.text)

    def next(self) -> None:
        if self.history_index is None:
            return
        if self.history_index < len(self.history) - 1:
            self.history_index += 1
            self.text = self.history[self.history_index]
        else:
            self.history_index = None
            self.text = self.saved_text
        self.cursor = len(self.text)


class SerialTui:
    def __init__(self, screen, args: argparse.Namespace) -> None:
        self.screen = screen
        self.args = args
        self.colors = ColorManager()
        self.terminal = TerminalBuffer(width=80, max_lines=args.scrollback)
        self.ansi = AnsiParser(self.terminal)
        self.decoder = codecs.getincrementaldecoder(args.encoding)(errors=args.errors)
        self.editor = LineEditor()
        self.worker: Optional[SerialWorker] = None
        self.config: Optional[SerialConfig] = None
        self.running = True
        self.mode = args.mode
        self.local_echo = args.local_echo
        self.scroll_offset = 0
        self.status = "Select a serial port"
        self.status_kind = "warn"
        self.status_until = 0.0
        self.last_error = ""
        self.log_handle: Optional[BinaryIO] = None
        self.log_path = Path(args.log).expanduser() if args.log else None
        self.rx_bytes = 0
        self.tx_bytes = 0
        self.started_at = time.monotonic()
        self.last_render = 0.0
        self.force_render = True
        self.new_data_while_scrolled = False
        self.newline = {
            "lf": b"\n",
            "crlf": b"\r\n",
            "cr": b"\r",
            "none": b"",
        }[args.newline]

    def initialize(self) -> None:
        locale.setlocale(locale.LC_ALL, "")
        self.screen.keypad(True)
        self.screen.nodelay(True)
        self.screen.timeout(25)
        # raw() makes control bytes such as Ctrl-C available to get_wch()
        # instead of turning them into process-level terminal signals.
        curses.raw()
        try:
            curses.curs_set(1)
        except curses.error:
            pass
        try:
            curses.set_escdelay(25)
        except (AttributeError, curses.error):
            pass
        try:
            curses.mousemask(curses.ALL_MOUSE_EVENTS | curses.REPORT_MOUSE_POSITION)
        except curses.error:
            pass

        if self.log_path:
            try:
                self.log_handle = self.log_path.open("ab", buffering=0)
            except OSError as exc:
                self.set_status(f"Cannot open log: {exc}", "error", sticky=True)

    def make_config(self, port: str, baud: Optional[int] = None) -> SerialConfig:
        flow = self.args.flow
        return SerialConfig(
            port=port,
            baud=baud if baud is not None else self.args.baud,
            bytesize=self.args.bytesize,
            parity=self.args.parity.upper(),
            stopbits=self.args.stopbits,
            rtscts=flow == "rtscts",
            xonxoff=flow == "xonxoff",
            dsrdtr=flow == "dsrdtr",
            reconnect=not self.args.no_reconnect,
            reset_pulse_ms=self.args.dtr_reset,
            exclusive=self.args.exclusive,
        )

    def start_worker(self, config: SerialConfig) -> None:
        self.stop_worker()
        self.config = config
        self.decoder.reset()
        self.worker = SerialWorker(config)
        self.worker.start()
        self.set_status(f"Opening {config.port}…", "warn", sticky=True)

    def stop_worker(self) -> None:
        if self.worker is not None:
            self.worker.stop()
            self.worker.join(timeout=0.75)
            self.worker = None

    def set_status(
        self, message: str, kind: str = "hint", duration: float = 3.0, sticky: bool = False
    ) -> None:
        self.status = message
        self.status_kind = kind
        self.status_until = float("inf") if sticky else time.monotonic() + duration
        self.force_render = True

    def drain_events(self) -> None:
        if self.worker is None:
            return
        changed = False
        while True:
            try:
                event = self.worker.events.get_nowait()
            except queue.Empty:
                break
            changed = True
            if event.kind == "data":
                data = event.payload
                assert isinstance(data, bytes)
                self.rx_bytes += len(data)
                if self.log_handle:
                    try:
                        self.log_handle.write(data)
                    except OSError as exc:
                        try:
                            self.log_handle.close()
                        except OSError:
                            pass
                        self.log_handle = None
                        self.set_status(f"Logging stopped: {exc}", "error", sticky=True)
                self.ansi.feed(self.decoder.decode(data))
                if self.scroll_offset:
                    self.new_data_while_scrolled = True
                else:
                    self.new_data_while_scrolled = False
            elif event.kind == "sent":
                data = event.payload
                assert isinstance(data, bytes)
                self.tx_bytes += len(data)
            elif event.kind == "connecting":
                self.set_status("Connecting…", "warn", sticky=True)
            elif event.kind == "connected":
                self.last_error = ""
                label = self.config.port if self.config else "serial device"
                self.set_status(f"Connected to {label}", "ok")
            elif event.kind == "disconnected":
                self.set_status("Disconnected; waiting to reconnect…", "warn", sticky=True)
            elif event.kind == "error":
                self.last_error = str(event.payload)
                self.set_status(self.last_error, "error", duration=6.0)
            elif event.kind == "stopped" and self.worker and not self.worker.connected:
                if self.args.no_reconnect:
                    self.set_status("Serial connection stopped", "error", sticky=True)
        if changed:
            self.force_render = True

    def send(self, data: bytes, echo: bool = True) -> bool:
        if not self.worker:
            self.set_status("No serial port selected", "error")
            return False
        if not self.worker.connected:
            self.set_status("Not connected; data was queued", "warn")
        self.worker.send(data)
        if self.local_echo and echo:
            self.ansi.feed(data.decode(self.args.encoding, errors=self.args.errors))
        self.force_render = True
        return True

    def send_raw_backspace(self) -> None:
        """Send the configured erase byte and mirror canonical local echo."""

        sent = self.send(bytes([self.args.backspace]), echo=False)
        if sent and self.local_echo:
            self.terminal.erase_previous_character()
            self.force_render = True

    def encode(self, text: str) -> bytes:
        try:
            return text.encode(self.args.encoding, errors=self.args.errors)
        except (LookupError, UnicodeError) as exc:
            self.set_status(f"Encoding error: {exc}", "error")
            return b""

    def run(self) -> int:
        self.initialize()
        port = self.args.port
        if port is None:
            port = self.choose_port(startup=True)
            if not port:
                return 0
        self.start_worker(self.make_config(port))

        while self.running:
            self.drain_events()
            now = time.monotonic()
            if self.force_render or now - self.last_render >= 0.1:
                self.render()
                self.last_render = now
                self.force_render = False
            try:
                key = self.screen.get_wch()
            except curses.error:
                continue
            self.handle_key(key)

        return 0

    def handle_key(self, key: Union[int, str]) -> None:
        if key in (curses.KEY_F10, "\x11"):  # F10 or Ctrl-Q
            self.running = False
            return
        if key == curses.KEY_F1:
            self.show_help()
            return
        if key == curses.KEY_F2:
            port = self.choose_port()
            if port:
                baud = self.config.baud if self.config else self.args.baud
                self.start_worker(self.make_config(port, baud))
            return
        if key == curses.KEY_F3:
            baud = self.choose_baud()
            if baud and self.config:
                self.start_worker(replace(self.config, baud=baud))
            return
        if key == curses.KEY_F4:
            self.mode = "raw" if self.mode == "line" else "line"
            self.set_status(f"Input mode: {self.mode}", "hint")
            return
        if key == curses.KEY_F5:
            if self.worker:
                self.worker.request_reconnect()
                self.set_status("Reconnecting…", "warn", sticky=True)
            return
        if key in (curses.KEY_F6, "\x0c"):  # F6 or Ctrl-L
            self.terminal.clear()
            self.scroll_offset = 0
            self.set_status("Display cleared", "hint")
            return
        if key == curses.KEY_F8:
            self.local_echo = not self.local_echo
            self.set_status(f"Local echo {'on' if self.local_echo else 'off'}", "hint")
            return
        if key == curses.KEY_RESIZE:
            self.force_render = True
            return
        if key == curses.KEY_PPAGE:
            self.scroll_offset = min(
                max(0, len(self.terminal.lines) - 1), self.scroll_offset + self.content_height()
            )
            self.force_render = True
            return
        if key == curses.KEY_NPAGE:
            self.scroll_offset = max(0, self.scroll_offset - self.content_height())
            if self.scroll_offset == 0:
                self.new_data_while_scrolled = False
            self.force_render = True
            return
        if key == curses.KEY_MOUSE:
            self.handle_mouse()
            return

        if self.mode == "raw":
            self.handle_raw_key(key)
        else:
            self.handle_line_key(key)

    def handle_mouse(self) -> None:
        try:
            _, _, _, _, state = curses.getmouse()
        except curses.error:
            return
        if state & getattr(curses, "BUTTON4_PRESSED", 0):
            self.scroll_offset = min(len(self.terminal.lines) - 1, self.scroll_offset + 3)
        elif state & getattr(curses, "BUTTON5_PRESSED", 0):
            self.scroll_offset = max(0, self.scroll_offset - 3)
        if self.scroll_offset == 0:
            self.new_data_while_scrolled = False
        self.force_render = True

    def handle_raw_key(self, key: Union[int, str]) -> None:
        mappings = {
            curses.KEY_UP: b"\x1b[A",
            curses.KEY_DOWN: b"\x1b[B",
            curses.KEY_RIGHT: b"\x1b[C",
            curses.KEY_LEFT: b"\x1b[D",
            curses.KEY_HOME: b"\x1b[H",
            curses.KEY_END: b"\x1b[F",
            curses.KEY_DC: b"\x1b[3~",
            curses.KEY_IC: b"\x1b[2~",
        }
        if isinstance(key, int):
            if key in mappings:
                self.send(mappings[key])
            elif key in (curses.KEY_BACKSPACE, 127):
                self.send_raw_backspace()
            return
        if key in ("\n", "\r"):
            self.send(self.newline)
        elif key in ("\b", "\x7f"):
            self.send_raw_backspace()
        elif key == "\x03":  # Ctrl-C belongs to the MCU, not the TUI.
            self.send(b"\x03")
        elif key == "\x1d":  # Ctrl-] is also a conventional terminal escape.
            self.running = False
        else:
            self.send(self.encode(key))

    def handle_line_key(self, key: Union[int, str]) -> None:
        if key in ("\n", "\r", curses.KEY_ENTER):
            value = self.editor.commit()
            self.send(self.encode(value) + self.newline)
        elif key in (curses.KEY_BACKSPACE, 127, "\b", "\x7f"):
            self.editor.backspace()
        elif key == curses.KEY_DC:
            self.editor.delete()
        elif key == curses.KEY_LEFT:
            self.editor.cursor = max(0, self.editor.cursor - 1)
        elif key == curses.KEY_RIGHT:
            self.editor.cursor = min(len(self.editor.text), self.editor.cursor + 1)
        elif key in (curses.KEY_HOME, "\x01"):
            self.editor.cursor = 0
        elif key in (curses.KEY_END, "\x05"):
            self.editor.cursor = len(self.editor.text)
        elif key == curses.KEY_UP:
            self.editor.previous()
        elif key == curses.KEY_DOWN:
            self.editor.next()
        elif key == "\x15":  # Ctrl-U
            self.editor.text = self.editor.text[self.editor.cursor :]
            self.editor.cursor = 0
        elif key == "\x0b":  # Ctrl-K
            self.editor.text = self.editor.text[: self.editor.cursor]
        elif key == "\x17":  # Ctrl-W
            left = self.editor.text[: self.editor.cursor].rstrip()
            split = left.rfind(" ") + 1
            self.editor.text = (
                self.editor.text[:split] + self.editor.text[self.editor.cursor :]
            )
            self.editor.cursor = split
        elif key == "\x03":
            self.send(b"\x03")
        elif isinstance(key, str) and key >= " " and key != "\x7f":
            self.editor.insert(key)
        self.force_render = True

    def content_height(self) -> int:
        height, _ = self.screen.getmaxyx()
        return max(1, height - 3)

    def render(self) -> None:
        height, width = self.screen.getmaxyx()
        if height < 5 or width < 24:
            self.screen.erase()
            self.safe_addstr(0, 0, "Terminal too small", curses.A_BOLD, max(1, width - 1))
            self.screen.refresh()
            return

        content_height = height - 3
        self.terminal.set_width(width)
        self.screen.erase()
        self.render_header(width)
        lines = self.terminal.visible_lines(content_height, self.scroll_offset)
        for row, line in enumerate(lines, start=1):
            self.render_terminal_line(row, line, width)
        self.render_status(height - 2, width)
        self.render_input(height - 1, width)
        self.screen.noutrefresh()
        curses.doupdate()

    def render_header(self, width: int) -> None:
        connected = bool(self.worker and self.worker.connected)
        state = "● ONLINE" if connected else "○ OFFLINE"
        port = self.config.port if self.config else "no port"
        baud = self.config.baud if self.config else self.args.baud
        elapsed = max(0.001, time.monotonic() - self.started_at)
        stats = (
            f" RX {human_bytes(self.rx_bytes)}  TX {human_bytes(self.tx_bytes)}"
            f"  {self.rx_bytes / elapsed:,.0f} B/s "
        )
        left = f" {APP_NAME}  {state}  {port} @ {baud:,} "
        available = max(0, width - len(stats))
        text = left[:available].ljust(available) + stats[-min(len(stats), width) :]
        self.safe_addstr(
            0, 0, text.ljust(width), self.colors.ui("header", curses.A_BOLD), width
        )

    def render_terminal_line(self, row: int, line: list[Cell], width: int) -> None:
        col = 0
        run_text = ""
        run_style: Optional[Style] = None
        run_start = 0

        def flush() -> None:
            nonlocal run_text
            if run_text and run_style is not None:
                self.safe_addstr(
                    row, run_start, run_text, self.colors.attr(run_style), width - run_start
                )
            run_text = ""

        for cell in line[:width]:
            if cell.continuation:
                continue
            if cell.style != run_style:
                flush()
                run_style = cell.style
                run_start = col
            run_text += cell.text
            col += max(1, display_width(cell.text[0]) if cell.text else 1)
            if col >= width:
                break
        flush()

    def render_status(self, row: int, width: int) -> None:
        if self.scroll_offset:
            marker = (
                f" ↑ {self.scroll_offset} lines"
                + (" • NEW DATA" if self.new_data_while_scrolled else "")
                + " • PgDn to follow "
            )
            attr = self.colors.ui("warn", curses.A_BOLD)
        elif time.monotonic() <= self.status_until:
            marker = f" {self.status} "
            attr = self.colors.ui(self.status_kind)
        elif self.log_handle and self.log_path:
            marker = f" Recording raw RX → {self.log_path} "
            attr = self.colors.ui("error", curses.A_BOLD)
        else:
            marker = " F1 Help  F2 Port  F3 Baud  F4 Mode  F5 Reconnect  F10 Quit "
            attr = self.colors.ui("hint")
        self.safe_addstr(row, 0, marker.ljust(width), attr, width)

    def render_input(self, row: int, width: int) -> None:
        connected = bool(self.worker and self.worker.connected)
        dot = "●" if connected else "○"
        echo = " echo" if self.local_echo else ""
        prefix = f" {dot} {self.mode}{echo} › "
        prefix_attr = self.colors.ui("ok" if connected else "error", curses.A_BOLD)
        self.safe_addstr(row, 0, prefix, prefix_attr, width)

        if self.mode == "raw":
            message = "Keystrokes go directly to the MCU (Ctrl-] or F10 exits)"
            self.safe_addstr(row, len(prefix), message, curses.A_DIM, width - len(prefix))
            cursor_x = min(width - 1, len(prefix))
        else:
            field_width = max(1, width - len(prefix) - 1)
            left = max(0, self.editor.cursor - field_width + 1)
            visible = self.editor.text[left : left + field_width]
            self.safe_addstr(row, len(prefix), visible, 0, field_width)
            cursor_x = min(
                width - 1,
                len(prefix) + len(self.editor.text[left : self.editor.cursor]),
            )
        try:
            self.screen.move(row, cursor_x)
            curses.curs_set(1)
        except curses.error:
            pass

    def safe_addstr(self, row: int, col: int, text: str, attr: int, limit: int) -> None:
        if limit <= 0 or not text:
            return
        try:
            self.screen.addnstr(row, col, text, limit, attr)
        except curses.error:
            # Writing the lower-right cell raises on several valid terminals.
            pass

    def centered_window(self, height: int, width: int):
        screen_height, screen_width = self.screen.getmaxyx()
        height = min(height, max(3, screen_height - 2))
        width = min(width, max(20, screen_width - 2))
        top = max(0, (screen_height - height) // 2)
        left = max(0, (screen_width - width) // 2)
        window = curses.newwin(height, width, top, left)
        window.keypad(True)
        return window

    def choose_port(self, startup: bool = False) -> Optional[str]:
        selected = 0
        while True:
            ports = sorted(list(list_ports.comports()), key=lambda item: item.device)
            rows = max(8, min(20, len(ports) + 6))
            window = self.centered_window(rows, 78)
            window.erase()
            window.box()
            title = " Select serial port "
            try:
                window.addstr(0, 2, title, self.colors.ui("header", curses.A_BOLD))
                if not ports:
                    window.addstr(2, 3, "No serial ports detected.", curses.A_BOLD)
                    window.addstr(4, 3, "M: enter a device path   R: refresh")
                else:
                    visible_rows = rows - 4
                    selected = min(selected, len(ports) - 1)
                    first = max(
                        0, min(selected - visible_rows // 2, len(ports) - visible_rows)
                    )
                    for line_no, info in enumerate(
                        ports[first : first + visible_rows], start=2
                    ):
                        index = first + line_no - 2
                        label = f"{info.device:<18} {info.description or 'Serial device'}"
                        attr = (
                            self.colors.ui("selection", curses.A_BOLD)
                            if index == selected
                            else 0
                        )
                        window.addnstr(line_no, 2, label.ljust(73), 73, attr)
                    window.addnstr(
                        rows - 2,
                        2,
                        "Enter: connect   M: manual path   R: refresh   Esc: cancel",
                        73,
                        curses.A_DIM,
                    )
            except curses.error:
                pass
            window.refresh()
            key = window.get_wch()
            if key in (curses.KEY_UP, "k") and ports:
                selected = (selected - 1) % len(ports)
            elif key in (curses.KEY_DOWN, "j") and ports:
                selected = (selected + 1) % len(ports)
            elif key in ("\n", "\r", curses.KEY_ENTER) and ports:
                del window
                self.force_render = True
                return ports[selected].device
            elif key in ("m", "M"):
                value = self.prompt_text("Serial device path", "")
                if value:
                    del window
                    self.force_render = True
                    return value
            elif key in ("r", "R"):
                continue
            elif key in ("\x1b", "q", "Q", curses.KEY_F10):
                del window
                self.force_render = True
                if not startup:
                    self.set_status("Port selection cancelled", "hint")
                return None

    def choose_baud(self) -> Optional[int]:
        current = self.config.baud if self.config else self.args.baud
        choices = sorted(set(COMMON_BAUDS + [current]))
        selected = choices.index(current)
        while True:
            rows = min(20, len(choices) + 4)
            window = self.centered_window(rows, 42)
            window.erase()
            window.box()
            try:
                window.addstr(0, 2, " Select baud rate ", curses.A_BOLD)
                visible_rows = rows - 3
                first = max(
                    0, min(selected - visible_rows // 2, len(choices) - visible_rows)
                )
                for line_no, baud in enumerate(
                    choices[first : first + visible_rows], start=1
                ):
                    index = first + line_no - 1
                    attr = (
                        self.colors.ui("selection", curses.A_BOLD)
                        if index == selected
                        else 0
                    )
                    window.addnstr(
                        line_no, 2, f"{baud:,}".ljust(37), 37, attr
                    )
                window.addnstr(
                    rows - 2, 2, "Enter: select  M: custom  Esc: cancel", 37, curses.A_DIM
                )
            except curses.error:
                pass
            window.refresh()
            key = window.get_wch()
            if key in (curses.KEY_UP, "k"):
                selected = (selected - 1) % len(choices)
            elif key in (curses.KEY_DOWN, "j"):
                selected = (selected + 1) % len(choices)
            elif key in ("\n", "\r", curses.KEY_ENTER):
                del window
                self.force_render = True
                return choices[selected]
            elif key in ("m", "M"):
                value = self.prompt_text("Custom baud rate", str(current))
                if value:
                    try:
                        baud = int(value.replace("_", "").replace(",", ""))
                        if baud <= 0:
                            raise ValueError
                        del window
                        self.force_render = True
                        return baud
                    except ValueError:
                        self.set_status("Baud rate must be a positive integer", "error")
            elif key in ("\x1b", "q", "Q"):
                del window
                self.force_render = True
                return None

    def prompt_text(self, title: str, initial: str) -> Optional[str]:
        value = initial
        cursor = len(value)
        while True:
            window = self.centered_window(7, 66)
            window.erase()
            window.box()
            try:
                window.addstr(0, 2, f" {title} ", curses.A_BOLD)
                window.addnstr(2, 3, value.ljust(59), 59)
                window.addnstr(4, 3, "Enter: accept   Esc: cancel", 59, curses.A_DIM)
                window.move(2, min(61, 3 + cursor))
            except curses.error:
                pass
            window.refresh()
            try:
                key = window.get_wch()
            except curses.error:
                continue
            if key in ("\n", "\r", curses.KEY_ENTER):
                return value.strip() or None
            if key == "\x1b":
                return None
            if key in (curses.KEY_BACKSPACE, "\b", "\x7f", 127) and cursor:
                value = value[: cursor - 1] + value[cursor:]
                cursor -= 1
            elif key == curses.KEY_DC and cursor < len(value):
                value = value[:cursor] + value[cursor + 1 :]
            elif key == curses.KEY_LEFT:
                cursor = max(0, cursor - 1)
            elif key == curses.KEY_RIGHT:
                cursor = min(len(value), cursor + 1)
            elif key == curses.KEY_HOME:
                cursor = 0
            elif key == curses.KEY_END:
                cursor = len(value)
            elif isinstance(key, str) and key >= " " and len(value) < 240:
                value = value[:cursor] + key + value[cursor:]
                cursor += len(key)

    def show_help(self) -> None:
        help_lines = [
            ("Global", curses.A_BOLD),
            ("F1 help  F2 port  F3 baud  F4 line/raw mode", 0),
            ("F5 reconnect  F6/Ctrl-L clear  F8 local echo  F10/Ctrl-Q quit", 0),
            ("PgUp/PgDn or mouse wheel scrolls output; PgDn returns to live data", 0),
            ("", 0),
            ("Line mode", curses.A_BOLD),
            ("Enter sends the edited command; ↑/↓ browse command history", 0),
            ("Ctrl-A/E home/end  Ctrl-U/K erase to start/end  Ctrl-W erase word", 0),
            ("Ctrl-C sends ETX (0x03) to the microcontroller", 0),
            ("", 0),
            ("Raw mode", curses.A_BOLD),
            ("Printable keys, arrows, Home/End and Delete are sent immediately", 0),
            ("Ctrl-C sends ETX; Ctrl-] or F10 exits", 0),
            ("", 0),
            ("ANSI support", curses.A_BOLD),
            ("SGR reset/attributes; 8/16/256 colors; RGB quantized to xterm-256", 0),
            ("Cursor motion, position, save/restore, erase, insert/delete, OSC title", 0),
            ("", 0),
            ("Press Esc, F1, Enter, or Q to close this help.", curses.A_DIM),
        ]
        height = min(24, len(help_lines) + 4)
        window = self.centered_window(height, 78)
        window.erase()
        window.box()
        try:
            window.addstr(0, 2, " Help ", self.colors.ui("header", curses.A_BOLD))
            for row, (text, attr) in enumerate(help_lines[: height - 2], start=1):
                window.addnstr(row, 3, text, 71, attr)
        except curses.error:
            pass
        window.refresh()
        while True:
            key = window.get_wch()
            if key in ("\x1b", "\n", "\r", "q", "Q", curses.KEY_F1, curses.KEY_F10):
                break
        del window
        self.force_render = True

    def close(self) -> None:
        self.stop_worker()
        if self.log_handle:
            try:
                self.log_handle.close()
            except OSError:
                pass
            self.log_handle = None
        try:
            curses.noraw()
        except curses.error:
            pass


def human_bytes(value: int) -> str:
    if value < 1_000:
        return f"{value} B"
    if value < 1_000_000:
        return f"{value / 1_000:.1f} kB"
    if value < 1_000_000_000:
        return f"{value / 1_000_000:.1f} MB"
    return f"{value / 1_000_000_000:.1f} GB"


def existing_encoding(value: str) -> str:
    try:
        codecs.lookup(value)
    except LookupError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    return value


def parse_backspace(value: str) -> int:
    aliases = {"bs": 8, "backspace": 8, "del": 127}
    lowered = value.lower()
    if lowered in aliases:
        return aliases[lowered]
    try:
        number = int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("use BS, DEL, or a byte value") from exc
    if not 0 <= number <= 255:
        raise argparse.ArgumentTypeError("backspace byte must be between 0 and 255")
    return number


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Full-screen STM32 serial terminal with ANSI/VT100 rendering.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("port", nargs="?", help="serial device, e.g. /dev/ttyACM0 or COM5")
    parser.add_argument("-b", "--baud", type=int, default=115_200)
    parser.add_argument("--bytesize", type=int, choices=(5, 6, 7, 8), default=8)
    parser.add_argument(
        "--parity", choices=("N", "E", "O", "M", "S", "n", "e", "o", "m", "s"), default="N"
    )
    parser.add_argument("--stopbits", type=float, choices=(1, 1.5, 2), default=1)
    parser.add_argument(
        "--flow", choices=("none", "rtscts", "xonxoff", "dsrdtr"), default="none"
    )
    parser.add_argument(
        "--newline", choices=("lf", "crlf", "cr", "none"), default="crlf"
    )
    parser.add_argument("--encoding", type=existing_encoding, default="utf-8")
    parser.add_argument(
        "--errors",
        choices=("strict", "replace", "ignore", "backslashreplace"),
        default="replace",
        help="text encoding error policy",
    )
    parser.add_argument("--mode", choices=("line", "raw"), default="line")
    parser.add_argument(
        "--local-echo",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="show transmitted text; use --no-local-echo if the device echoes it",
    )
    parser.add_argument(
        "--backspace",
        type=parse_backspace,
        default=127,
        metavar="BS|DEL|BYTE",
        help="byte sent for Backspace in raw mode",
    )
    parser.add_argument("--scrollback", type=int, default=10_000)
    parser.add_argument("--log", metavar="PATH", help="append exact received bytes to PATH")
    parser.add_argument("--no-reconnect", action="store_true")
    parser.add_argument(
        "--dtr-reset",
        type=int,
        default=0,
        metavar="MS",
        help="pulse DTR low for this many milliseconds after opening",
    )
    parser.add_argument(
        "--exclusive",
        action="store_true",
        help="request exclusive access on supported POSIX systems",
    )
    parser.add_argument(
        "--self-test", action="store_true", help="run built-in parser tests and exit"
    )
    return parser


def run_self_test() -> int:
    terminal = TerminalBuffer(width=40)
    parser = AnsiParser(terminal)

    parser.feed("plain \x1b[31")
    parser.feed("mred\x1b[0m normal")
    assert terminal.lines[0][6].style.fg == 1
    assert terminal.lines[0][9].style == DEFAULT_STYLE

    terminal.clear()
    parser.feed("\x1b[38;5;196mR\x1b[48;2;0;128;255mB")
    assert terminal.lines[0][0].style.fg == 196
    assert terminal.lines[0][1].style.bg == rgb_to_xterm(0, 128, 255)
    parser.feed("\x1b[38:2::255:64:0mC")
    assert terminal.lines[0][2].style.fg == rgb_to_xterm(255, 64, 0)
    assert xterm_to_ansi(196, 8) == 1
    assert xterm_to_ansi(46, 8) == 2
    assert xterm_to_ansi(21, 8) == 4

    terminal.clear()
    parser.feed("progress 10%\rprogress 90%\n")
    assert terminal.plain_text().splitlines()[0] == "progress 90%"

    terminal.clear()
    parser.feed("abcdef\x1b[3D\x1b[KXYZ")
    assert terminal.plain_text() == "abcXYZ"

    terminal.clear()
    parser.feed("line one\nline two\x1b[1A\x1b[1GUP")
    assert terminal.plain_text().splitlines()[0].startswith("UP")

    terminal.clear()
    parser.feed("\x1b]0;STM32 console\x07ok")
    assert terminal.title == "STM32 console"

    terminal.clear()
    parser.feed("wide:界!")
    assert terminal.cursor_col == 8
    assert terminal.lines[0][6].continuation
    terminal.erase_previous_character()
    assert terminal.plain_text() == "wide:界"
    terminal.erase_previous_character()
    assert terminal.plain_text() == "wide:"
    assert terminal.cursor_col == 5

    terminal.clear()
    parser.feed("starts at top")
    visible = terminal.visible_lines(4)
    assert visible[0] is terminal.lines[0]
    assert visible[1:] == [[], [], []]

    defaults = build_argument_parser().parse_args([])
    assert defaults.local_echo is True
    tui = SerialTui(None, defaults)

    class EchoWorker:
        connected = True

        def __init__(self) -> None:
            self.sent = b""

        def send(self, data: bytes) -> None:
            self.sent += data

    echo_worker = EchoWorker()
    tui.worker = echo_worker  # type: ignore[assignment]
    tui.send(b"status\r\n")
    assert echo_worker.sent == b"status\r\n"
    assert tui.terminal.plain_text().splitlines()[0] == "status"
    tui.terminal.clear()
    echo_worker.sent = b""
    tui.send(b"mistkae")
    tui.send_raw_backspace()
    assert echo_worker.sent == b"mistkae\x7f"
    assert tui.terminal.plain_text() == "mistka"
    defaults.backspace = 8
    tui.send_raw_backspace()
    assert echo_worker.sent == b"mistkae\x7f\x08"
    assert tui.terminal.plain_text() == "mistk"

    print("All ANSI/VT100 parser self-tests passed.")
    return 0


def curses_main(screen, args: argparse.Namespace) -> int:
    app = SerialTui(screen, args)
    try:
        return app.run()
    finally:
        app.close()


def main(argv: Optional[Iterable[str]] = None) -> int:
    args = build_argument_parser().parse_args(argv)
    if args.baud <= 0:
        raise SystemExit("--baud must be a positive integer")
    if args.scrollback < 100:
        raise SystemExit("--scrollback must be at least 100 lines")
    if args.dtr_reset < 0:
        raise SystemExit("--dtr-reset cannot be negative")
    if args.self_test:
        return run_self_test()
    if serial is None or list_ports is None:
        print(
            "pyserial is required. Install it with:\n"
            f"  {sys.executable} -m pip install pyserial",
            file=sys.stderr,
        )
        return 2
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        print("This full-screen interface requires an interactive terminal.", file=sys.stderr)
        return 2

    # Curses owns SIGINT so Ctrl-C can be forwarded to the STM32.
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    try:
        return curses.wrapper(curses_main, args)
    except KeyboardInterrupt:
        return 130
    except curses.error as exc:
        print(f"Unable to initialize the terminal UI: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
