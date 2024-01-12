from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from lava.utils import LogFollower

from lava.exceptions import MesaCIKnownIssueException
from lava.utils.console_format import CONSOLE_LOG
from lava.utils.constants import KNOWN_ISSUE_R8152_MAX_CONSECUTIVE_COUNTER
from lava.utils.log_section import LogSectionType


@dataclass
class LAVALogHints:
    log_follower: LogFollower
    r8152_issue_consecutive_counter: int = field(default=0, init=False)

    def detect_failure(self, new_lines: list[dict[str, Any]]):
        for line in new_lines:
            self.detect_r8152_issue(line)

    def detect_r8152_issue(self, line):
        if (
            self.log_follower.phase == LogSectionType.TEST_CASE and line["lvl"] == "target"
        ):
            if re.search(r"r8152 \S+ eth0: Tx status -71", line["msg"]):
                self.r8152_issue_consecutive_counter += 1
                return

            if self.r8152_issue_consecutive_counter >= KNOWN_ISSUE_R8152_MAX_CONSECUTIVE_COUNTER:
                if re.search(
                    r"nfs: server \d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3} not responding, still trying",
                    line["msg"],
                ):
                    raise MesaCIKnownIssueException(
                        f"{CONSOLE_LOG['FG_MAGENTA']}"
                        "Probable network issue failure encountered, retrying the job"
                        f"{CONSOLE_LOG['RESET']}"
                    )

        # Reset the status, as the `nfs... still trying` complaint was not detected
        self.r8152_issue_consecutive_counter = 0
