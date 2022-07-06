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
from datetime import datetime, timedelta
from enum import Enum, auto
from typing import Optional, Pattern, Union

from lava.exceptions import MesaCITimeoutError

CONSOLE_LOG = {
    "FG_GREEN": "\x1b[1;32;5;197m",
    "FG_RED": "\x1b[1;38;5;197m",
    "RESET": "\x1b[0m",
    "UNDERLINED": "\x1b[3m",
    "BOLD": "\x1b[1m",
    "DIM": "\x1b[2m",
}


class LogSectionType(Enum):
    LAVA_BOOT = auto()
    TEST_SUITE = auto()
    TEST_CASE = auto()
    LAVA_POST_PROCESSING = auto()


FALLBACK_GITLAB_SECTION_TIMEOUT = timedelta(minutes=10)
DEFAULT_GITLAB_SECTION_TIMEOUTS = {
    # Empirically, the devices boot time takes 3 minutes on average.
    LogSectionType.LAVA_BOOT: timedelta(minutes=5),
    # Test suite phase is where the initialization happens.
    LogSectionType.TEST_SUITE: timedelta(minutes=5),
    # Test cases may take a long time, this script has no right to interrupt
    # them. But if the test case takes almost 1h, it will never succeed due to
    # Gitlab job timeout.
    LogSectionType.TEST_CASE: timedelta(minutes=60),
    # LAVA post processing may refer to a test suite teardown, or the
    # adjustments to start the next test_case
    LogSectionType.LAVA_POST_PROCESSING: timedelta(minutes=5),
}
@dataclass
class GitlabSection:
    id: str
    header: str
    type: LogSectionType
    start_collapsed: bool = False
    escape: str = "\x1b[0K"
    colour: str = f"{CONSOLE_LOG['BOLD']}{CONSOLE_LOG['FG_GREEN']}"
    __start_time: Optional[datetime] = field(default=None, init=False)
    __end_time: Optional[datetime] = field(default=None, init=False)

    @classmethod
    def section_id_filter(cls, value) -> str:
        return str(re.sub(r"[^\w_-]+", "-", value))

    def __post_init__(self):
        self.id = self.section_id_filter(self.id)

    @property
    def has_started(self) -> bool:
        return self.__start_time is not None

    @property
    def has_finished(self) -> bool:
        return self.__end_time is not None

    def get_timestamp(self, time: datetime) -> str:
        unix_ts = datetime.timestamp(time)
        return str(int(unix_ts))

    def section(self, marker: str, header: str, time: datetime) -> str:
        preamble = f"{self.escape}section_{marker}"
        collapse = marker == "start" and self.start_collapsed
        collapsed = "[collapsed=true]" if collapse else ""
        section_id = f"{self.id}{collapsed}"

        timestamp = self.get_timestamp(time)
        before_header = ":".join([preamble, timestamp, section_id])
        colored_header = f"{self.colour}{header}\x1b[0m" if header else ""
        header_wrapper = "\r" + f"{self.escape}{colored_header}"

        return f"{before_header}{header_wrapper}"

    def __enter__(self):
        print(self.start())
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        print(self.end())

    def start(self) -> str:
        assert not self.has_finished, "Starting an already finished section"
        self.__start_time = datetime.now()
        return self.section(marker="start", header=self.header, time=self.__start_time)

    def end(self) -> str:
        assert self.has_started, "Ending an uninitalized section"
        self.__end_time = datetime.now()
        assert (
            self.__end_time >= self.__start_time
        ), "Section execution time will be negative"
        return self.section(marker="end", header="", time=self.__end_time)

    def delta_time(self) -> Optional[timedelta]:
        if self.__start_time and self.__end_time:
            return self.__end_time - self.__start_time

        if self.has_started:
            return datetime.now() - self.__start_time

        return None


@dataclass(frozen=True)
class LogSection:
    regex: Union[Pattern, str]
    levels: tuple[str]
    section_id: str
    section_header: str
    section_type: LogSectionType
    collapsed: bool = False

    def from_log_line_to_section(
        self, lava_log_line: dict[str, str]
    ) -> Optional[GitlabSection]:
        if lava_log_line["lvl"] not in self.levels:
            return

        if match := re.search(self.regex, lava_log_line["msg"]):
            section_id = self.section_id.format(*match.groups())
            section_header = self.section_header.format(*match.groups())
            return GitlabSection(
                id=section_id,
                header=section_header,
                type=self.section_type,
                start_collapsed=self.collapsed,
            )


LOG_SECTIONS = (
    LogSection(
        regex=re.compile(r"<?STARTTC>? ([^>]*)"),
        levels=("target", "debug"),
        section_id="{}",
        section_header="test_case {}",
        section_type=LogSectionType.TEST_CASE,
    ),
    LogSection(
        regex=re.compile(r"<?STARTRUN>? ([^>]*)"),
        levels=("target", "debug"),
        section_id="{}",
        section_header="test_suite {}",
        section_type=LogSectionType.TEST_SUITE,
    ),
    LogSection(
        regex=re.compile(r"ENDTC>? ([^>]+)"),
        levels=("target", "debug"),
        section_id="post-{}",
        section_header="Post test_case {}",
        collapsed=True,
        section_type=LogSectionType.LAVA_POST_PROCESSING,
    ),
)


@dataclass
class LogFollower:
    current_section: Optional[GitlabSection] = None
    timeout_durations: dict[LogSectionType, timedelta] = field(
        default_factory=lambda: DEFAULT_GITLAB_SECTION_TIMEOUTS,
    )
    fallback_timeout: timedelta = FALLBACK_GITLAB_SECTION_TIMEOUT
    _buffer: list[str] = field(default_factory=list, init=False)

    def __post_init__(self):
        section_is_created = bool(self.current_section)
        section_has_started = bool(
            self.current_section and self.current_section.has_started
        )
        assert (
            section_is_created == section_has_started
        ), "Can't follow logs beginning from uninitialized GitLab sections."

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        """Cleanup existing buffer if this object gets out from the context"""
        self.clear_current_section()
        last_lines = self.flush()
        for line in last_lines:
            print(line)

    def watchdog(self):
        if not self.current_section:
            return

        timeout_duration = self.timeout_durations.get(
            self.current_section.type, self.fallback_timeout
        )

        if self.current_section.delta_time() > timeout_duration:
            raise MesaCITimeoutError(
                f"Gitlab Section {self.current_section} has timed out",
                timeout_duration=timeout_duration,
            )

    def clear_current_section(self):
        if self.current_section and not self.current_section.has_finished:
            self._buffer.append(self.current_section.end())
            self.current_section = None

    def update_section(self, new_section: GitlabSection):
        # Sections can have redundant regex to find them to mitigate LAVA
        # interleaving kmsg and stderr/stdout issue.
        if self.current_section and self.current_section.id == new_section.id:
            return
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

    def detect_kernel_dump_line(self, line: dict[str, Union[str, list]]) -> bool:
        # line["msg"] can be a list[str] when there is a kernel dump
        if isinstance(line["msg"], list):
            return line["lvl"] == "debug"

        # result level has dict line["msg"]
        if not isinstance(line["msg"], str):
            return False

        # we have a line, check if it is a kernel message
        if re.search(r"\[[\d\s]{5}\.[\d\s]{6}\] +\S{2,}", line["msg"]):
            print_log(f"{CONSOLE_LOG['BOLD']}{line['msg']}{CONSOLE_LOG['RESET']}")
            return True

        return False

    def feed(self, new_lines: list[dict[str, str]]) -> bool:
        """Input data to be processed by LogFollower instance
        Returns true if the DUT (device under test) seems to be alive.
        """

        self.watchdog()

        # No signal of job health in the log
        is_job_healthy = False

        for line in new_lines:
            if self.detect_kernel_dump_line(line):
                continue

            # At least we are fed with a non-kernel dump log, it seems that the
            # job is progressing
            is_job_healthy = True
            self.manage_gl_sections(line)
            if parsed_line := parse_lava_line(line):
                self._buffer.append(parsed_line)

        return is_job_healthy

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


def parse_lava_line(line) -> Optional[str]:
    prefix = ""
    suffix = ""

    if line["lvl"] in ["results", "feedback", "debug"]:
        return
    elif line["lvl"] in ["warning", "error"]:
        prefix = CONSOLE_LOG["FG_RED"]
        suffix = CONSOLE_LOG["RESET"]
    elif line["lvl"] == "input":
        prefix = "$ "
        suffix = ""
    elif line["lvl"] == "target":
        fix_lava_color_log(line)
        fix_lava_gitlab_section_log(line)

    return f'{prefix}{line["msg"]}{suffix}'


def print_log(msg):
    # Reset color from timestamp, since `msg` can tint the terminal color
    print(f"{CONSOLE_LOG['RESET']}{datetime.now()}: {msg}")


def fatal_err(msg):
    print_log(msg)
    sys.exit(1)


def hide_sensitive_data(yaml_data, hide_tag="HIDEME"):
    return "".join(line for line in yaml_data.splitlines(True) if hide_tag not in line)
