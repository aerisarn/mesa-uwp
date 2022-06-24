#!/usr/bin/env python3
#
# Copyright (C) 2022 Collabora Limited
# Author: Guilherme Gallo <guilherme.gallo@collabora.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

"""
Some utilities to analyse logs, create gitlab sections and other quality of life
improvements
"""

import logging
import re
import sys
from dataclasses import dataclass, field
from datetime import datetime
from typing import Optional, Pattern, Union

# Helper constants to colorize the job output
CONSOLE_LOG = {
    "COLOR_GREEN": "\x1b[1;32;5;197m",
    "COLOR_RED": "\x1b[1;38;5;197m",
    "RESET": "\x1b[0m",
    "UNDERLINED": "\x1b[3m",
    "BOLD": "\x1b[1m",
    "DIM": "\x1b[2m",
}


@dataclass
class GitlabSection:
    id: str
    header: str
    start_collapsed: bool = False
    escape: str = "\x1b[0K"
    colour: str = f"{CONSOLE_LOG['BOLD']}{CONSOLE_LOG['COLOR_GREEN']}"

    def get_timestamp(self) -> str:
        unix_ts = datetime.timestamp(datetime.now())
        return str(int(unix_ts))

    def section(self, marker: str, header: str) -> str:
        preamble = f"{self.escape}section_{marker}"
        collapse = marker == "start" and self.start_collapsed
        collapsed = "[collapsed=true]" if collapse else ""
        section_id = f"{self.id}{collapsed}"

        timestamp = self.get_timestamp()
        before_header = ":".join([preamble, timestamp, section_id])
        colored_header = (
            f"{self.colour}{header}{CONSOLE_LOG['RESET']}" if header else ""
        )
        header_wrapper = "\r" + f"{self.escape}{colored_header}"

        return f"{before_header}{header_wrapper}"

    def start(self) -> str:
        return self.section(marker="start", header=self.header)

    def end(self) -> str:
        return self.section(marker="end", header="")


@dataclass(frozen=True)
class LogSection:
    regex: Union[Pattern, str]
    level: str
    section_id: str
    section_header: str
    collapsed: bool = False

    def from_log_line_to_section(
        self, lava_log_line: dict[str, str]
    ) -> Optional[GitlabSection]:
        if lava_log_line["lvl"] == self.level:
            if match := re.match(self.regex, lava_log_line["msg"]):
                section_id = self.section_id.format(*match.groups())
                section_header = self.section_header.format(*match.groups())
                return GitlabSection(
                    id=section_id,
                    header=section_header,
                    start_collapsed=self.collapsed,
                )


LOG_SECTIONS = (
    LogSection(
        regex=re.compile(r".*<STARTTC> (.*)"),
        level="debug",
        section_id="{}",
        section_header="test_case {}",
    ),
    LogSection(
        regex=re.compile(r".*<STARTRUN> (\S*)"),
        level="debug",
        section_id="{}",
        section_header="test_suite {}",
    ),
    LogSection(
        regex=re.compile(r"^<LAVA_SIGNAL_ENDTC ([^>]+)"),
        level="target",
        section_id="post-{}",
        section_header="Post test_case {}",
        collapsed=True,
    ),
)


@dataclass
class LogFollower:
    current_section: Optional[GitlabSection] = None
    sections: list[str] = field(default_factory=list)
    collapsed_sections: tuple[str] = ("setup",)
    _buffer: list[str] = field(default_factory=list, init=False)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        """Cleanup existing buffer if this object gets out from the context"""
        self.clear_current_section()
        last_lines = self.flush()
        for line in last_lines:
            print(line)

    def clear_current_section(self):
        if self.current_section:
            self._buffer.append(self.current_section.end())
            self.current_section = None

    def update_section(self, new_section: GitlabSection):
        self.clear_current_section()
        self.current_section = new_section
        self._buffer.append(new_section.start())

    def manage_gl_sections(self, line):
        if isinstance(line["msg"], list):
            logging.debug("Ignoring messages as list. Kernel dumps.")
            return

        for log_section in LOG_SECTIONS:
            if new_section := log_section.from_log_line_to_section(line):
                self.update_section(new_section)

    def feed(self, new_lines: list[dict[str, str]]) -> None:
        for line in new_lines:
            self.manage_gl_sections(line)
            self._buffer.append(line)

    def flush(self) -> list[str]:
        buffer = self._buffer
        self._buffer = []
        return buffer


def fix_lava_color_log(line):
    """This function is a temporary solution for the color escape codes mangling
    problem. There is some problem in message passing between the LAVA
    dispatcher and the device under test (DUT). Here \x1b character is missing
    before `[:digit::digit:?:digit:?m` ANSI TTY color codes, or the more
    complicated ones with number values for text format before background and
    foreground colors.
    When this problem is fixed on the LAVA side, one should remove this function.
    """
    line["msg"] = re.sub(r"(\[(\d+;){0,2}\d{1,3}m)", "\x1b" + r"\1", line["msg"])


def fix_lava_gitlab_section_log(line):
    """This function is a temporary solution for the Gitlab section markers
    mangling problem. Gitlab parses the following lines to define a collapsible
    gitlab section in their log:
    - \x1b[0Ksection_start:timestamp:section_id[collapsible=true/false]\r\x1b[0Ksection_header
    - \x1b[0Ksection_end:timestamp:section_id\r\x1b[0K
    There is some problem in message passing between the LAVA dispatcher and the
    device under test (DUT), that digests \x1b and \r control characters
    incorrectly. When this problem is fixed on the LAVA side, one should remove
    this function.
    """
    if match := re.match(r"\[0K(section_\w+):(\d+):(\S+)\[0K([\S ]+)?", line["msg"]):
        marker, timestamp, id_collapsible, header = match.groups()
        # The above regex serves for both section start and end lines.
        # When the header is None, it means we are dealing with `section_end` line
        header = header or ""
        line["msg"] = f"\x1b[0K{marker}:{timestamp}:{id_collapsible}\r\x1b[0K{header}"


def filter_debug_messages(line: dict[str, str]) -> bool:
    """Filter some LAVA debug messages that does not add much information to the
    developer and may clutter the trace log."""
    if line["lvl"] != "debug":
        return False
    # line["msg"] can be a list[str] when there is a kernel dump
    if not isinstance(line["msg"], str):
        return False

    if re.match(
        # Sometimes LAVA dumps this messages lots of times when the LAVA job is
        # reaching the end.
        r"^Listened to connection for namespace",
        line["msg"],
    ):
        return True
    return False


def parse_lava_lines(new_lines) -> list[str]:
    parsed_lines: list[str] = []
    for line in new_lines:
        prefix = ""
        suffix = ""

        if line["lvl"] in ["results", "feedback"]:
            continue
        elif line["lvl"] in ["warning", "error"]:
            prefix = CONSOLE_LOG["COLOR_RED"]
            suffix = CONSOLE_LOG["RESET"]
        elif filter_debug_messages(line):
            continue
        elif line["lvl"] == "input":
            prefix = "$ "
            suffix = ""
        elif line["lvl"] == "target":
            fix_lava_color_log(line)
            fix_lava_gitlab_section_log(line)

        line = f'{prefix}{line["msg"]}{suffix}'
        parsed_lines.append(line)

    return parsed_lines


def print_log(msg):
    # Reset color from timestamp, since `msg` can tint the terminal color
    print(f"{CONSOLE_LOG['RESET']}{datetime.now()}: {msg}")


def fatal_err(msg):
    print_log(msg)
    sys.exit(1)


def hide_sensitive_data(yaml_data, hide_tag="HIDEME"):
    return "".join(line for line in yaml_data.splitlines(True) if hide_tag not in line)
