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

from dataclasses import dataclass
from datetime import datetime

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

    def __enter__(self):
        print(self.start())
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        print(self.end())

    def start(self) -> str:
        return self.section(marker="start", header=self.header)

    def end(self) -> str:
        return self.section(marker="end", header="")
