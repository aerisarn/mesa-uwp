from contextlib import nullcontext as does_not_raise
from datetime import datetime, timedelta
from itertools import cycle
from typing import Callable, Generator, Iterable, Tuple, Union

import yaml
from freezegun import freeze_time


def create_lava_yaml_msg(
    dt: Callable = datetime.now, msg="test", lvl="target"
) -> dict[str, str]:
    return {"dt": str(dt()), "msg": msg, "lvl": lvl}


def generate_testsuite_result(
    name="test-mesa-ci", result="pass", metadata_extra=None, extra=None
):
    if metadata_extra is None:
        metadata_extra = {}
    if extra is None:
        extra = {}
    return {"metadata": {"result": result, **metadata_extra}, "name": name}


def jobs_logs_response(
    finished=False, msg=None, lvl="target", result=None
) -> Tuple[bool, str]:
    timed_msg = {"dt": str(datetime.now()), "msg": "New message", "lvl": lvl}
    if result:
        timed_msg["lvl"] = "target"
        timed_msg["msg"] = f"hwci: mesa: {result}"

    logs = [timed_msg] if msg is None else msg

    return finished, yaml.safe_dump(logs)



def level_generator():
    # Tests all known levels by default
    yield from cycle(("results", "feedback", "warning", "error", "debug", "target"))


def generate_n_logs(
    n=1,
    tick_fn: Union[Generator, Iterable[int], int] = 1,
    level_fn=level_generator,
    result="pass",
):
    """Simulate a log partitionated in n components"""
    level_gen = level_fn()

    if isinstance(tick_fn, Generator):
        tick_gen = tick_fn
    elif isinstance(tick_fn, Iterable):
        tick_gen = cycle(tick_fn)
    else:
        tick_gen = cycle((tick_fn,))

    with freeze_time(datetime.now()) as time_travel:
        tick_sec: int = next(tick_gen)
        while True:
            # Simulate a scenario where the target job is waiting for being started
            for _ in range(n - 1):
                level: str = next(level_gen)

                time_travel.tick(tick_sec)
                yield jobs_logs_response(finished=False, msg=[], lvl=level)

            time_travel.tick(tick_sec)
            yield jobs_logs_response(finished=True, result=result)


def to_iterable(tick_fn):
    if isinstance(tick_fn, Generator):
        tick_gen = tick_fn
    elif isinstance(tick_fn, Iterable):
        tick_gen = cycle(tick_fn)
    else:
        tick_gen = cycle((tick_fn,))
    return tick_gen
