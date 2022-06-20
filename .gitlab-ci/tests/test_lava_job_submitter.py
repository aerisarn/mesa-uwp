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

import xmlrpc.client
from contextlib import nullcontext as does_not_raise
from datetime import datetime
from itertools import cycle, repeat
from typing import Callable, Generator, Iterable, Tuple, Union
from unittest.mock import MagicMock, patch

import pytest
import yaml
from freezegun import freeze_time
from lava.exceptions import MesaCIException, MesaCIRetryError, MesaCITimeoutError
from lava.lava_job_submitter import (
    DEVICE_HANGING_TIMEOUT_SEC,
    NUMBER_OF_RETRIES_TIMEOUT_DETECTION,
    LAVAJob,
    filter_debug_messages,
    fix_lava_color_log,
    fix_lava_gitlab_section_log,
    follow_job_execution,
    hide_sensitive_data,
    retriable_follow_job,
)

NUMBER_OF_MAX_ATTEMPTS = NUMBER_OF_RETRIES_TIMEOUT_DETECTION + 1


def create_lava_yaml_msg(
    dt: Callable = datetime.now, msg="test", lvl="target"
) -> dict[str, str]:
    return {"dt": str(dt()), "msg": msg, "lvl": lvl}


def jobs_logs_response(finished=False, msg=None, lvl="target", result=None) -> Tuple[bool, str]:
    timed_msg = {"dt": str(datetime.now()), "msg": "New message", "lvl": lvl}
    if result:
        timed_msg["lvl"] = "target"
        timed_msg["msg"] = f"hwci: mesa: {result}"

    logs = [timed_msg] if msg is None else msg

    return finished, yaml.safe_dump(logs)


RESULT_GET_TESTJOB_RESULTS = [{"metadata": {"result": "test"}}]


def generate_testsuite_result(name="test-mesa-ci", result="pass", metadata_extra = None, extra = None):
    if metadata_extra is None:
        metadata_extra = {}
    if extra is None:
        extra = {}
    return {"metadata": {"result": result, **metadata_extra}, "name": name}


@pytest.fixture
def mock_proxy():
    def create_proxy_mock(
        job_results=RESULT_GET_TESTJOB_RESULTS,
        testsuite_results=[generate_testsuite_result()],
        **kwargs
    ):
        proxy_mock = MagicMock()
        proxy_submit_mock = proxy_mock.scheduler.jobs.submit
        proxy_submit_mock.return_value = "1234"

        proxy_results_mock = proxy_mock.results.get_testjob_results_yaml
        proxy_results_mock.return_value = yaml.safe_dump(job_results)

        proxy_test_suites_mock = proxy_mock.results.get_testsuite_results_yaml
        proxy_test_suites_mock.return_value = yaml.safe_dump(testsuite_results)

        proxy_logs_mock = proxy_mock.scheduler.jobs.logs
        proxy_logs_mock.return_value = jobs_logs_response()

        for key, value in kwargs.items():
            setattr(proxy_logs_mock, key, value)

        return proxy_mock

    yield create_proxy_mock


@pytest.fixture
def mock_proxy_waiting_time(mock_proxy):
    def update_mock_proxy(frozen_time, **kwargs):
        wait_time = kwargs.pop("wait_time", 0)
        proxy_mock = mock_proxy(**kwargs)
        proxy_job_state = proxy_mock.scheduler.job_state
        proxy_job_state.return_value = {"job_state": "Running"}
        proxy_job_state.side_effect = frozen_time.tick(wait_time)

        return proxy_mock

    return update_mock_proxy


@pytest.fixture
def mock_sleep():
    """Mock time.sleep to make test faster"""
    with patch("time.sleep", return_value=None):
        yield


@pytest.fixture
def frozen_time(mock_sleep):
    with freeze_time() as frozen_time:
        yield frozen_time


@pytest.mark.parametrize("exception", [RuntimeError, SystemError, KeyError])
def test_submit_and_follow_respects_exceptions(mock_sleep, mock_proxy, exception):
    with pytest.raises(MesaCIException):
        proxy = mock_proxy(side_effect=exception)
        job = LAVAJob(proxy, '')
        follow_job_execution(job)


def level_generator():
    # Tests all known levels by default
    yield from cycle(( "results", "feedback", "warning", "error", "debug", "target" ))

def generate_n_logs(n=1, tick_fn: Union[Generator, Iterable[int], int]=1, level_fn=level_generator, result="pass"):
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


NETWORK_EXCEPTION = xmlrpc.client.ProtocolError("", 0, "test", {})
XMLRPC_FAULT = xmlrpc.client.Fault(0, "test")

PROXY_SCENARIOS = {
    "finish case": (generate_n_logs(1), does_not_raise(), True, {}),
    "works at last retry": (
        generate_n_logs(n=NUMBER_OF_MAX_ATTEMPTS, tick_fn=[ DEVICE_HANGING_TIMEOUT_SEC + 1 ] * NUMBER_OF_RETRIES_TIMEOUT_DETECTION + [1]),
        does_not_raise(),
        True,
        {},
    ),
    "timed out more times than retry attempts": (
        generate_n_logs(
            n=NUMBER_OF_MAX_ATTEMPTS + 1, tick_fn=DEVICE_HANGING_TIMEOUT_SEC + 1
        ),
        pytest.raises(MesaCIRetryError),
        False,
        {},
    ),
    "long log case, no silence": (
        generate_n_logs(n=1000, tick_fn=0),
        does_not_raise(),
        True,
        {},
    ),
    "no retries, testsuite succeed": (
        generate_n_logs(n=1, tick_fn=0),
        does_not_raise(),
        True,
        {
            "testsuite_results": [
                generate_testsuite_result(result="pass")
            ]
        },
    ),
    "no retries, but testsuite fails": (
        generate_n_logs(n=1, tick_fn=0, result="fail"),
        does_not_raise(),
        False,
        {
            "testsuite_results": [
                generate_testsuite_result(result="fail")
            ]
        },
    ),
    "no retries, one testsuite fails": (
        generate_n_logs(n=1, tick_fn=0, result="fail"),
        does_not_raise(),
        False,
        {
            "testsuite_results": [
                generate_testsuite_result(result="fail"),
                generate_testsuite_result(result="pass")
            ]
        },
    ),
    "very long silence": (
        generate_n_logs(n=NUMBER_OF_MAX_ATTEMPTS + 1, tick_fn=100000),
        pytest.raises(MesaCIRetryError),
        False,
        {},
    ),
    # If a protocol error happens, _call_proxy will retry without affecting timeouts
    "unstable connection, ProtocolError followed by final message": (
        (NETWORK_EXCEPTION, jobs_logs_response(finished=True, result="pass")),
        does_not_raise(),
        True,
        {},
    ),
    # After an arbitrary number of retries, _call_proxy should call sys.exit
    "unreachable case, subsequent ProtocolErrors": (
        repeat(NETWORK_EXCEPTION),
        pytest.raises(SystemExit),
        False,
        {},
    ),
    "XMLRPC Fault": ([XMLRPC_FAULT], pytest.raises(SystemExit, match="1"), False, {}),
}


@patch("time.sleep", return_value=None)  # mock sleep to make test faster
@pytest.mark.parametrize(
    "side_effect, expectation, job_result, proxy_args",
    PROXY_SCENARIOS.values(),
    ids=PROXY_SCENARIOS.keys(),
)
def test_retriable_follow_job(
    mock_sleep, side_effect, expectation, job_result, proxy_args, mock_proxy
):
    with expectation:
        proxy = mock_proxy(side_effect=side_effect, **proxy_args)
        job: LAVAJob = retriable_follow_job(proxy, "")
        assert job_result == (job.status == "pass")


WAIT_FOR_JOB_SCENARIOS = {
    "one log run taking (sec):": (generate_n_logs(1), True),
}


@pytest.mark.parametrize("wait_time", (0, DEVICE_HANGING_TIMEOUT_SEC * 2))
@pytest.mark.parametrize(
    "side_effect, has_finished",
    WAIT_FOR_JOB_SCENARIOS.values(),
    ids=WAIT_FOR_JOB_SCENARIOS.keys(),
)
def test_simulate_a_long_wait_to_start_a_job(
    frozen_time,
    wait_time,
    side_effect,
    has_finished,
    mock_proxy_waiting_time,
):
    start_time = datetime.now()
    job: LAVAJob = retriable_follow_job(
        mock_proxy_waiting_time(
            frozen_time, side_effect=side_effect, wait_time=wait_time
        ),
        "",
    )

    end_time = datetime.now()
    delta_time = end_time - start_time

    assert has_finished == (job.status == "pass")
    assert delta_time.total_seconds() >= wait_time


SENSITIVE_DATA_SCENARIOS = {
    "no sensitive data tagged": (
        ["bla  bla", "mytoken: asdkfjsde1341=="],
        ["bla  bla", "mytoken: asdkfjsde1341=="],
        "HIDEME",
    ),
    "sensitive data tagged": (
        ["bla  bla", "mytoken: asdkfjsde1341== # HIDEME"],
        ["bla  bla"],
        "HIDEME",
    ),
    "sensitive data tagged with custom word": (
        ["bla  bla", "mytoken: asdkfjsde1341== # DELETETHISLINE", "third line"],
        ["bla  bla", "third line"],
        "DELETETHISLINE",
    ),
}


@pytest.mark.parametrize(
    "input, expectation, tag",
    SENSITIVE_DATA_SCENARIOS.values(),
    ids=SENSITIVE_DATA_SCENARIOS.keys(),
)
def test_hide_sensitive_data(input, expectation, tag):
    yaml_data = yaml.safe_dump(input)
    yaml_result = hide_sensitive_data(yaml_data, tag)
    result = yaml.safe_load(yaml_result)

    assert result == expectation


CORRUPTED_LOG_SCENARIOS = {
    "too much subsequent corrupted data": (
        [(False, "{'msg': 'Incomplete}")] * 100 + [jobs_logs_response(True)],
        pytest.raises((MesaCIRetryError)),
    ),
    "one subsequent corrupted data": (
        [(False, "{'msg': 'Incomplete}")] * 2 + [jobs_logs_response(True)],
        does_not_raise(),
    ),
}


@pytest.mark.parametrize(
    "data_sequence, expected_exception",
    CORRUPTED_LOG_SCENARIOS.values(),
    ids=CORRUPTED_LOG_SCENARIOS.keys(),
)
def test_log_corruption(mock_sleep, data_sequence, expected_exception, mock_proxy):
    proxy_mock = mock_proxy()
    proxy_logs_mock = proxy_mock.scheduler.jobs.logs
    proxy_logs_mock.side_effect = data_sequence
    with expected_exception:
        retriable_follow_job(proxy_mock, "")


COLOR_MANGLED_SCENARIOS = {
    "Mangled error message at target level": (
        create_lava_yaml_msg(msg="[0m[0m[31mERROR - dEQP error: ", lvl="target"),
        "\x1b[0m\x1b[0m\x1b[31mERROR - dEQP error: ",
    ),
    "Mangled pass message at target level": (
        create_lava_yaml_msg(
            msg="[0mPass: 26718, ExpectedFail: 95, Skip: 25187, Duration: 8:18, Remaining: 13",
            lvl="target",
        ),
        "\x1b[0mPass: 26718, ExpectedFail: 95, Skip: 25187, Duration: 8:18, Remaining: 13",
    ),
    "Mangled error message with bold formatting at target level": (
        create_lava_yaml_msg(msg="[1;31mReview the image changes...", lvl="target"),
        "\x1b[1;31mReview the image changes...",
    ),
    "Mangled error message with high intensity background at target level": (
        create_lava_yaml_msg(msg="[100mReview the image changes...", lvl="target"),
        "\x1b[100mReview the image changes...",
    ),
    "Mangled error message with underline+bg color+fg color at target level": (
        create_lava_yaml_msg(msg="[4;41;97mReview the image changes...", lvl="target"),
        "\x1b[4;41;97mReview the image changes...",
    ),
    "Bad input for color code.": (
        create_lava_yaml_msg(
            msg="[4;97 This message is missing the `m`.", lvl="target"
        ),
        "[4;97 This message is missing the `m`.",
    ),
}


@pytest.mark.parametrize(
    "message, fixed_message",
    COLOR_MANGLED_SCENARIOS.values(),
    ids=COLOR_MANGLED_SCENARIOS.keys(),
)
def test_fix_lava_color_log(message, fixed_message):
    fix_lava_color_log(message)

    assert message["msg"] == fixed_message


GITLAB_SECTION_MANGLED_SCENARIOS = {
    "Mangled section_start at target level": (
        create_lava_yaml_msg(
            msg="[0Ksection_start:1652658415:deqp[collapsed=false][0Kdeqp-runner",
            lvl="target",
        ),
        "\x1b[0Ksection_start:1652658415:deqp[collapsed=false]\r\x1b[0Kdeqp-runner",
    ),
    "Mangled section_start at target level with header with spaces": (
        create_lava_yaml_msg(
            msg="[0Ksection_start:1652658415:deqp[collapsed=false][0Kdeqp runner stats",
            lvl="target",
        ),
        "\x1b[0Ksection_start:1652658415:deqp[collapsed=false]\r\x1b[0Kdeqp runner stats",
    ),
    "Mangled section_end at target level": (
        create_lava_yaml_msg(
            msg="[0Ksection_end:1652658415:test_setup[0K",
            lvl="target",
        ),
        "\x1b[0Ksection_end:1652658415:test_setup\r\x1b[0K",
    ),
}

@pytest.mark.parametrize(
    "message, fixed_message",
    GITLAB_SECTION_MANGLED_SCENARIOS.values(),
    ids=GITLAB_SECTION_MANGLED_SCENARIOS.keys(),
)
def test_fix_lava_gitlab_section_log(message, fixed_message):
    fix_lava_gitlab_section_log(message)

    assert message["msg"] == fixed_message


LAVA_DEBUG_SPAM_MESSAGES = {
    "Listened to connection in debug level": (
        create_lava_yaml_msg(
            msg="Listened to connection for namespace 'common' for up to 1s",
            lvl="debug",
        ),
        True,
    ),
    "Listened to connection in debug level - v2": (
        create_lava_yaml_msg(
            msg="Listened to connection for namespace 'prepare' for up to 9s",
            lvl="debug",
        ),
        True,
    ),
    "Listened to connection in target level": (
        create_lava_yaml_msg(
            msg="Listened to connection for namespace 'common' for up to 1s",
            lvl="target",
        ),
        False,
    ),
}


@pytest.mark.parametrize(
    "message, expectation",
    LAVA_DEBUG_SPAM_MESSAGES.values(),
    ids=LAVA_DEBUG_SPAM_MESSAGES.keys(),
)
def test_filter_debug_messages(message, expectation):
    assert filter_debug_messages(message) == expectation
