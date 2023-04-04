#!/usr/bin/env python3
#
# Copyright (C) 2020 - 2023 Collabora Limited
# Authors:
#     Gustavo Padovan <gustavo.padovan@collabora.com>
#     Guilherme Gallo <guilherme.gallo@collabora.com>
#
# SPDX-License-Identifier: MIT

"""Send a job to LAVA, track it and collect log back"""


import argparse
import contextlib
import pathlib
import sys
import time
from datetime import datetime, timedelta
from io import StringIO
from os import getenv
from typing import Any, Optional

from lava.exceptions import (
    MesaCIException,
    MesaCIParseException,
    MesaCIRetryError,
    MesaCITimeoutError,
)
from lava.utils import CONSOLE_LOG
from lava.utils import DEFAULT_GITLAB_SECTION_TIMEOUTS as GL_SECTION_TIMEOUTS
from lava.utils import (
    GitlabSection,
    LAVAJob,
    LogFollower,
    LogSectionType,
    call_proxy,
    fatal_err,
    generate_lava_yaml_payload,
    hide_sensitive_data,
    print_log,
    setup_lava_proxy,
)
from lavacli.utils import flow_yaml as lava_yaml

# Timeout in seconds to decide if the device from the dispatched LAVA job has
# hung or not due to the lack of new log output.
DEVICE_HANGING_TIMEOUT_SEC = int(getenv("LAVA_DEVICE_HANGING_TIMEOUT_SEC",  5*60))

# How many seconds the script should wait before try a new polling iteration to
# check if the dispatched LAVA job is running or waiting in the job queue.
WAIT_FOR_DEVICE_POLLING_TIME_SEC = int(
    getenv("LAVA_WAIT_FOR_DEVICE_POLLING_TIME_SEC", 1)
)

# How many seconds the script will wait to let LAVA finalize the job and give
# the final details.
WAIT_FOR_LAVA_POST_PROCESSING_SEC = int(getenv("LAVA_WAIT_LAVA_POST_PROCESSING_SEC", 5))
WAIT_FOR_LAVA_POST_PROCESSING_RETRIES = int(
    getenv("LAVA_WAIT_LAVA_POST_PROCESSING_RETRIES", 3)
)

# How many seconds to wait between log output LAVA RPC calls.
LOG_POLLING_TIME_SEC = int(getenv("LAVA_LOG_POLLING_TIME_SEC", 5))

# How many retries should be made when a timeout happen.
NUMBER_OF_RETRIES_TIMEOUT_DETECTION = int(
    getenv("LAVA_NUMBER_OF_RETRIES_TIMEOUT_DETECTION", 2)
)

def find_exception_from_metadata(metadata, job_id):
    if "result" not in metadata or metadata["result"] != "fail":
        return
    if "error_type" in metadata:
        error_type = metadata["error_type"]
        if error_type == "Infrastructure":
            raise MesaCIException(
                f"LAVA job {job_id} failed with Infrastructure Error. Retry."
            )
        if error_type == "Job":
            # This happens when LAVA assumes that the job cannot terminate or
            # with mal-formed job definitions. As we are always validating the
            # jobs, only the former is probable to happen. E.g.: When some LAVA
            # action timed out more times than expected in job definition.
            raise MesaCIException(
                f"LAVA job {job_id} failed with JobError "
                "(possible LAVA timeout misconfiguration/bug). Retry."
            )
    if "case" in metadata and metadata["case"] == "validate":
        raise MesaCIException(
            f"LAVA job {job_id} failed validation (possible download error). Retry."
        )
    return metadata


def find_lava_error(job) -> None:
    # Look for infrastructure errors and retry if we see them.
    results_yaml = call_proxy(job.proxy.results.get_testjob_results_yaml, job.job_id)
    results = lava_yaml.load(results_yaml)
    for res in results:
        metadata = res["metadata"]
        find_exception_from_metadata(metadata, job.job_id)

    # If we reach this far, it means that the job ended without hwci script
    # result and no LAVA infrastructure problem was found
    job.status = "fail"


def show_job_data(job, colour=f"{CONSOLE_LOG['BOLD']}{CONSOLE_LOG['FG_GREEN']}"):
    with GitlabSection(
        "job_data",
        "LAVA job info",
        type=LogSectionType.LAVA_POST_PROCESSING,
        start_collapsed=True,
        colour=colour,
    ):
        wait_post_processing_retries: int = WAIT_FOR_LAVA_POST_PROCESSING_RETRIES
        while not job.is_post_processed() and wait_post_processing_retries > 0:
            # Wait a little until LAVA finishes processing metadata
            time.sleep(WAIT_FOR_LAVA_POST_PROCESSING_SEC)
            wait_post_processing_retries -= 1

        if not job.is_post_processed():
            waited_for_sec: int = (
                WAIT_FOR_LAVA_POST_PROCESSING_RETRIES * WAIT_FOR_DEVICE_POLLING_TIME_SEC
            )
            print_log(
                f"Waited for {waited_for_sec} seconds"
                "for LAVA to post-process the job, it haven't finished yet. "
                "Dumping it's info anyway"
            )

        details: dict[str, str] = job.show()
        for field, value in details.items():
            print(f"{field:<15}: {value}")
        job.refresh_log()


def fetch_logs(job, max_idle_time, log_follower) -> None:
    is_job_hanging(job, max_idle_time)

    time.sleep(LOG_POLLING_TIME_SEC)
    new_log_lines = fetch_new_log_lines(job)
    parsed_lines = parse_log_lines(job, log_follower, new_log_lines)

    for line in parsed_lines:
        print_log(line)


def is_job_hanging(job, max_idle_time):
    # Poll to check for new logs, assuming that a prolonged period of
    # silence means that the device has died and we should try it again
    if datetime.now() - job.last_log_time > max_idle_time:
        max_idle_time_min = max_idle_time.total_seconds() / 60

        raise MesaCITimeoutError(
            f"{CONSOLE_LOG['BOLD']}"
            f"{CONSOLE_LOG['FG_YELLOW']}"
            f"LAVA job {job.job_id} does not respond for {max_idle_time_min} "
            "minutes. Retry."
            f"{CONSOLE_LOG['RESET']}",
            timeout_duration=max_idle_time,
        )


def parse_log_lines(job, log_follower, new_log_lines):
    if log_follower.feed(new_log_lines):
        # If we had non-empty log data, we can assure that the device is alive.
        job.heartbeat()
    parsed_lines = log_follower.flush()

    # Only parse job results when the script reaches the end of the logs.
    # Depending on how much payload the RPC scheduler.jobs.logs get, it may
    # reach the LAVA_POST_PROCESSING phase.
    if log_follower.current_section.type in (
        LogSectionType.TEST_CASE,
        LogSectionType.LAVA_POST_PROCESSING,
    ):
        parsed_lines = job.parse_job_result_from_log(parsed_lines)
    return parsed_lines


def fetch_new_log_lines(job):
    # The XMLRPC binary packet may be corrupted, causing a YAML scanner error.
    # Retry the log fetching several times before exposing the error.
    for _ in range(5):
        with contextlib.suppress(MesaCIParseException):
            new_log_lines = job.get_logs()
            break
    else:
        raise MesaCIParseException
    return new_log_lines


def submit_job(job):
    try:
        job.submit()
    except Exception as mesa_ci_err:
        raise MesaCIException(
            f"Could not submit LAVA job. Reason: {mesa_ci_err}"
        ) from mesa_ci_err


def wait_for_job_get_started(job):
    print_log(f"Waiting for job {job.job_id} to start.")
    while not job.is_started():
        time.sleep(WAIT_FOR_DEVICE_POLLING_TIME_SEC)
    job.refresh_log()
    print_log(f"Job {job.job_id} started.")


def bootstrap_log_follower() -> LogFollower:
    gl = GitlabSection(
        id="lava_boot",
        header="LAVA boot",
        type=LogSectionType.LAVA_BOOT,
        start_collapsed=True,
    )
    print(gl.start())
    return LogFollower(current_section=gl)


def follow_job_execution(job, log_follower):
    with log_follower:
        max_idle_time = timedelta(seconds=DEVICE_HANGING_TIMEOUT_SEC)
        # Start to check job's health
        job.heartbeat()
        while not job.is_finished:
            fetch_logs(job, max_idle_time, log_follower)

    # Mesa Developers expect to have a simple pass/fail job result.
    # If this does not happen, it probably means a LAVA infrastructure error
    # happened.
    if job.status not in ["pass", "fail"]:
        find_lava_error(job)


def print_job_final_status(job):
    if job.status == "running":
        job.status = "hung"

    color = LAVAJob.COLOR_STATUS_MAP.get(job.status, CONSOLE_LOG["FG_RED"])
    print_log(
        f"{color}"
        f"LAVA Job finished with status: {job.status}"
        f"{CONSOLE_LOG['RESET']}"
    )

    job.refresh_log()
    job.log["status"] = job.status
    show_job_data(job, colour=f"{CONSOLE_LOG['BOLD']}{color}")


def execute_job_with_retries(proxy, job_definition, retry_count) -> Optional[LAVAJob]:
    for attempt_no in range(1, retry_count + 2):
        # Need to get the logger value from its object to enable autosave
        # features, if AutoSaveDict is enabled from StructuredLogging module
        job = LAVAJob(proxy, job_definition)

        try:
            submit_job(job)
            wait_for_job_get_started(job)
            log_follower: LogFollower = bootstrap_log_follower()
            follow_job_execution(job, log_follower)
            return job

        except (MesaCIException, KeyboardInterrupt) as exception:
            job.handle_exception(exception)
            print_log(
                f"{CONSOLE_LOG['BOLD']}"
                f"Finished executing LAVA job in the attempt #{attempt_no}"
                f"{CONSOLE_LOG['RESET']}"
            )

        finally:
            print_job_final_status(job)


def retriable_follow_job(proxy, job_definition) -> LAVAJob:
    number_of_retries = NUMBER_OF_RETRIES_TIMEOUT_DETECTION

    if finished_job := execute_job_with_retries(
        proxy, job_definition, number_of_retries
    ):
        return finished_job

    # Job failed in all attempts
    raise MesaCIRetryError(
        f"{CONSOLE_LOG['BOLD']}"
        f"{CONSOLE_LOG['FG_RED']}"
        "Job failed after it exceeded the number of "
        f"{number_of_retries} retries."
        f"{CONSOLE_LOG['RESET']}",
        retry_count=number_of_retries,
    )


def treat_mesa_job_name(args):
    # Remove mesa job names with spaces, which breaks the lava-test-case command
    args.mesa_job_name = args.mesa_job_name.split(" ")[0]


def main(args):
    proxy = setup_lava_proxy()

    # Overwrite the timeout for the testcases with the value offered by the
    # user. The testcase running time should be at least 4 times greater than
    # the other sections (boot and setup), so we can safely ignore them.
    # If LAVA fails to stop the job at this stage, it will fall back to the
    # script section timeout with a reasonable delay.
    GL_SECTION_TIMEOUTS[LogSectionType.TEST_CASE] = timedelta(minutes=args.job_timeout)

    job_definition_stream = StringIO()
    lava_yaml.dump(generate_lava_yaml_payload(args), job_definition_stream)
    job_definition = job_definition_stream.getvalue()

    if args.dump_yaml:
        with GitlabSection(
            "yaml_dump",
            "LAVA job definition (YAML)",
            type=LogSectionType.LAVA_BOOT,
            start_collapsed=True,
        ):
            print(hide_sensitive_data(job_definition))
    job = LAVAJob(proxy, job_definition)

    if errors := job.validate():
        fatal_err(f"Error in LAVA job definition: {errors}")
    print_log("LAVA job definition validated successfully")

    if args.validate_only:
        return

    finished_job = retriable_follow_job(proxy, job_definition)
    exit_code = 0 if finished_job.status == "pass" else 1
    sys.exit(exit_code)


def create_parser():
    parser = argparse.ArgumentParser("LAVA job submitter")

    parser.add_argument("--pipeline-info")
    parser.add_argument("--rootfs-url-prefix")
    parser.add_argument("--kernel-url-prefix")
    parser.add_argument("--build-url")
    parser.add_argument("--job-rootfs-overlay-url")
    parser.add_argument("--job-timeout", type=int)
    parser.add_argument("--first-stage-init")
    parser.add_argument("--ci-project-dir")
    parser.add_argument("--device-type")
    parser.add_argument("--dtb", nargs='?', default="")
    parser.add_argument("--kernel-image-name")
    parser.add_argument("--kernel-image-type", nargs='?', default="")
    parser.add_argument("--boot-method")
    parser.add_argument("--lava-tags", nargs='?', default="")
    parser.add_argument("--jwt-file", type=pathlib.Path)
    parser.add_argument("--validate-only", action='store_true')
    parser.add_argument("--dump-yaml", action='store_true')
    parser.add_argument("--visibility-group")
    parser.add_argument("--mesa-job-name")

    return parser


if __name__ == "__main__":
    # given that we proxy from DUT -> LAVA dispatcher -> LAVA primary -> us ->
    # GitLab runner -> GitLab primary -> user, safe to say we don't need any
    # more buffering
    sys.stdout.reconfigure(line_buffering=True)
    sys.stderr.reconfigure(line_buffering=True)

    parser = create_parser()

    parser.set_defaults(func=main)
    args = parser.parse_args()
    treat_mesa_job_name(args)
    args.func(args)
