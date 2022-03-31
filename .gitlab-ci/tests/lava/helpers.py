from contextlib import nullcontext as does_not_raise
from datetime import datetime, timedelta
from itertools import cycle
from typing import Callable, Generator, Iterable, Tuple, Union

import yaml
from freezegun import freeze_time
from lava.utils.lava_log import (
    DEFAULT_GITLAB_SECTION_TIMEOUTS,
    FALLBACK_GITLAB_SECTION_TIMEOUT,
    LogSectionType,
)


def section_timeout(section_type: LogSectionType) -> int:
    return int(
        DEFAULT_GITLAB_SECTION_TIMEOUTS.get(
            section_type, FALLBACK_GITLAB_SECTION_TIMEOUT
        ).total_seconds()
    )


def create_lava_yaml_msg(
    dt: Callable = datetime.now, msg="test", lvl="target"
) -> dict[str, str]:
    return {"dt": str(dt()), "msg": msg, "lvl": lvl}


def jobs_logs_response(finished=False, msg=None, **kwargs) -> Tuple[bool, str]:
    timed_msg = create_lava_yaml_msg(**kwargs)
    logs = [timed_msg] if msg is None else msg

    return finished, yaml.safe_dump(logs)


def message_generator_new(
    messages: dict[LogSectionType, Iterable[int]]
) -> Iterable[tuple[dict, Iterable[int]]]:
    default = [1]
    for section_type in LogSectionType:
        delay = messages.get(section_type, default)
        yield mock_lava_signal(section_type), delay


def message_generator():
    for section_type in LogSectionType:
        yield mock_lava_signal(section_type)


def generate_n_logs(
    n=0,
    tick_fn: Union[Generator, Iterable[int], int] = 1,
    message_fn=message_generator,
):
    if isinstance(tick_fn, Generator):
        tick_gen = tick_fn
    elif isinstance(tick_fn, Iterable):
        tick_gen = cycle(tick_fn)
    else:
        tick_gen = cycle((tick_fn,))

    with freeze_time(datetime.now()) as time_travel:
        tick_sec: int = next(tick_gen)
        while True:
            # Simulate a complete run given by message_fn
            for msg in message_fn():
                yield jobs_logs_response(finished=False, msg=[msg])
                time_travel.tick(tick_sec)

            yield jobs_logs_response(finished=True)


def to_iterable(tick_fn):
    if isinstance(tick_fn, Generator):
        tick_gen = tick_fn
    elif isinstance(tick_fn, Iterable):
        tick_gen = cycle(tick_fn)
    else:
        tick_gen = cycle((tick_fn,))
    return tick_gen


def mock_logs(
    messages={},
):
    with freeze_time(datetime.now()) as time_travel:
        # Simulate a complete run given by message_fn
        for msg, tick_list in message_generator_new(messages):
            for tick_sec in tick_list:
                yield jobs_logs_response(finished=False, msg=[msg])
                time_travel.tick(tick_sec)

        yield jobs_logs_response(finished=True)


def mock_lava_signal(type: LogSectionType) -> dict[str, str]:
    return {
        LogSectionType.TEST_CASE: create_lava_yaml_msg(
            msg="<STARTTC> case", lvl="debug"
        ),
        LogSectionType.TEST_SUITE: create_lava_yaml_msg(
            msg="<STARTRUN> suite", lvl="debug"
        ),
        LogSectionType.LAVA_POST_PROCESSING: create_lava_yaml_msg(
            msg="<LAVA_SIGNAL_ENDTC case>", lvl="target"
        ),
    }.get(type, create_lava_yaml_msg())