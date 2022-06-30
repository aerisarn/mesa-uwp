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
from itertools import repeat

import pytest
from lava.exceptions import MesaCIException, MesaCIRetryError
from lava.lava_job_submitter import (
    DEVICE_HANGING_TIMEOUT_SEC,
    NUMBER_OF_RETRIES_TIMEOUT_DETECTION,
    LAVAJob,
    follow_job_execution,
    retriable_follow_job,
)
from lava.utils.lava_log import LogSectionType

from .lava.helpers import (
    create_lava_yaml_msg,
    generate_n_logs,
    generate_testsuite_result,
    jobs_logs_response,
    mock_logs,
    section_timeout,
)

NUMBER_OF_MAX_ATTEMPTS = NUMBER_OF_RETRIES_TIMEOUT_DETECTION + 1


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


@pytest.mark.parametrize("exception", [RuntimeError, SystemError, KeyError])
def test_submit_and_follow_respects_exceptions(mock_sleep, mock_proxy, exception):
    with pytest.raises(MesaCIException):
        proxy = mock_proxy(side_effect=exception)
        job = LAVAJob(proxy, '')
        follow_job_execution(job)


NETWORK_EXCEPTION = xmlrpc.client.ProtocolError("", 0, "test", {})
XMLRPC_FAULT = xmlrpc.client.Fault(0, "test")

PROXY_SCENARIOS = {
    "finish case": (generate_n_logs(1), does_not_raise(), True, {}),
    "boot works at last retry": (
        mock_logs(
            {
                LogSectionType.LAVA_BOOT: [
                    section_timeout(LogSectionType.LAVA_BOOT) + 1
                ]
                * NUMBER_OF_RETRIES_TIMEOUT_DETECTION
                + [1]
            },
        ),
        does_not_raise(),
        True,
        {},
    ),
    "post process test case took too long": pytest.param(
        mock_logs(
            {
                LogSectionType.LAVA_POST_PROCESSING: [
                    section_timeout(LogSectionType.LAVA_POST_PROCESSING) + 1
                ]
                * (NUMBER_OF_MAX_ATTEMPTS + 1)
            },
        ),
        pytest.raises(MesaCIRetryError),
        True,
        {},
        marks=pytest.mark.xfail(
            reason=(
                "The time travel mock is not behaving as expected. "
                "It makes a gitlab section end in the past when an "
                "exception happens."
            )
        ),
    ),
    "timed out more times than retry attempts": (
        generate_n_logs(n=4, tick_fn=9999999),
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


@pytest.mark.parametrize(
    "test_log, expectation, job_result, proxy_args",
    PROXY_SCENARIOS.values(),
    ids=PROXY_SCENARIOS.keys(),
)
def test_retriable_follow_job(
    mock_sleep,
    test_log,
    expectation,
    job_result,
    proxy_args,
    mock_proxy,
):
    with expectation:
        proxy = mock_proxy(side_effect=test_log, **proxy_args)
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


LAVA_RESULT_LOG_SCENARIOS = {
    # the submitter should accept xtrace logs
    "Bash xtrace echo with kmsg interleaving": (
        create_lava_yaml_msg(
            msg="echo hwci: mesa: pass[  737.673352] <LAVA_SIGNAL_ENDTC mesa-ci>",
            lvl="target",
        ),
        "pass",
    ),
    # the submitter should accept xtrace logs
    "kmsg result print": (
        create_lava_yaml_msg(
            msg="[  737.673352] hwci: mesa: pass",
            lvl="target",
        ),
        "pass",
    ),
    # if the job result echo has a very bad luck, it still can be interleaved
    # with kmsg
    "echo output with kmsg interleaving": (
        create_lava_yaml_msg(
            msg="hwci: mesa: pass[  737.673352] <LAVA_SIGNAL_ENDTC mesa-ci>",
            lvl="target",
        ),
        "pass",
    ),
    "fail case": (
        create_lava_yaml_msg(
            msg="hwci: mesa: fail",
            lvl="target",
        ),
        "fail",
    ),
}


@pytest.mark.parametrize(
    "message, expectation",
    LAVA_RESULT_LOG_SCENARIOS.values(),
    ids=LAVA_RESULT_LOG_SCENARIOS.keys(),
)
def test_filter_debug_messages(message, expectation, mock_proxy):
    job = LAVAJob(mock_proxy(), "")
    job.parse_job_result_from_log([message])

    assert job.status == expectation


@pytest.mark.skip(reason="Integration test. Needs a LAVA log raw file at /tmp/log.yaml")
def test_full_yaml_log(mock_proxy, frozen_time):
    import itertools
    import random
    from datetime import datetime

    import yaml

    def time_travel_from_log_chunk(data_chunk):
        if not data_chunk:
            return

        first_log_time = data_chunk[0]["dt"]
        frozen_time.move_to(first_log_time)
        yield

        last_log_time = data_chunk[-1]["dt"]
        frozen_time.move_to(last_log_time)
        return

    def time_travel_to_test_time():
        # Suppose that the first message timestamp of the entire LAVA job log is
        # the same of from the job submitter execution
        with open("/tmp/log.yaml", "r") as f:
            first_log = f.readline()
            first_log_time = yaml.safe_load(first_log)[0]["dt"]
            frozen_time.move_to(first_log_time)

    def load_lines() -> list:
        with open("/tmp/log.yaml", "r") as f:
            data = yaml.safe_load(f)
            chain = itertools.chain(data)
            try:
                while True:
                    data_chunk = [next(chain) for _ in range(random.randint(0, 50))]
                    # Suppose that the first message timestamp is the same of
                    # log fetch RPC call
                    time_travel_from_log_chunk(data_chunk)
                    yield False, []
                    # Travel to the same datetime of the last fetched log line
                    # in the chunk
                    time_travel_from_log_chunk(data_chunk)
                    yield False, data_chunk
            except StopIteration:
                yield True, data_chunk
                return

    proxy = mock_proxy()

    def reset_logs(*args):
        proxy.scheduler.jobs.logs.side_effect = load_lines()

    proxy.scheduler.jobs.submit = reset_logs
    with pytest.raises(MesaCIRetryError):
        time_travel_to_test_time()
        retriable_follow_job(proxy, "")
