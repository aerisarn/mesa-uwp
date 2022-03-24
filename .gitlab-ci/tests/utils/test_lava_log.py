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

import pytest
from lava.utils.lava_log import GitlabSection

GITLAB_SECTION_SCENARIOS = {
    "start collapsed": (
        "start",
        True,
        f"\x1b[0Ksection_start:mock_date:my_first_section[collapsed=true]\r\x1b[0K{GitlabSection.colour}my_header\x1b[0m",
    ),
    "start non_collapsed": (
        "start",
        False,
        f"\x1b[0Ksection_start:mock_date:my_first_section\r\x1b[0K{GitlabSection.colour}my_header\x1b[0m",
    ),
    "end collapsed": (
        "end",
        True,
        "\x1b[0Ksection_end:mock_date:my_first_section\r\x1b[0K",
    ),
    "end non_collapsed": (
        "end",
        False,
        "\x1b[0Ksection_end:mock_date:my_first_section\r\x1b[0K",
    ),
}

@pytest.mark.parametrize(
    "method, collapsed, expectation",
    GITLAB_SECTION_SCENARIOS.values(),
    ids=GITLAB_SECTION_SCENARIOS.keys(),
)
def test_gitlab_section(method, collapsed, expectation):
    gs = GitlabSection(id="my_first_section", header="my_header", start_collapsed=collapsed)
    gs.get_timestamp = lambda: "mock_date"
    result = getattr(gs, method)()
    assert result == expectation
