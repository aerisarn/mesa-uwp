#!/usr/bin/env python3
# Copyright © 2023 Collabora Ltd.
# Authors:
#   Helen Koike <helen.koike@collabora.com>
#
# For the dependencies, see the requirements.txt
# SPDX-License-Identifier: MIT


import argparse
import gitlab
import plotly.express as px
from datetime import datetime, timedelta
from gitlab_common import read_token, GITLAB_URL, get_gitlab_pipeline_from_url


def calculate_queued_at(job):
    # we can have queued_duration without started_at when a job is canceled
    if not job.queued_duration or not job.started_at:
        return None
    started_at = job.started_at.replace("Z", "+00:00")
    return datetime.fromisoformat(started_at) - timedelta(seconds=job.queued_duration)



def create_task_name(job):
    status_color = {"success": "green", "failed": "red"}.get(job.status, "grey")
    return f"{job.name}\t(<span style='color: {status_color}'>{job.status}</span>,<a href='{job.web_url}'>{job.id}</a>)"


def add_gantt_bar(job, tasks):
    queued_at = calculate_queued_at(job)
    task_name = create_task_name(job)

    tasks.append(
        {
            "Job": task_name,
            "Start": job.created_at,
            "Finish": queued_at,
            "Phase": "Waiting dependencies",
        }
    )
    tasks.append(
        {
            "Job": task_name,
            "Start": queued_at,
            "Finish": job.started_at,
            "Phase": "Queued",
        }
    )
    tasks.append(
        {
            "Job": task_name,
            "Start": job.started_at,
            "Finish": job.finished_at,
            "Phase": "Running",
        }
    )


def generate_gantt_chart(pipeline):
    if pipeline.yaml_errors:
        raise ValueError("Pipeline YAML errors detected")

    # Convert the data into a list of dictionaries for plotly
    tasks = []

    for job in pipeline.jobs.list(all=True, include_retried=True):
        add_gantt_bar(job, tasks)

    # Make it easier to see retried jobs
    tasks.sort(key=lambda x: x["Job"])

    title = f"Gantt chart of jobs in pipeline <a href='{pipeline.web_url}'>{pipeline.web_url}</a>."
    title += (
        f" Total duration {str(timedelta(seconds=pipeline.duration))}"
        if pipeline.duration
        else ""
    )

    # Create a Gantt chart
    fig = px.timeline(
        tasks,
        x_start="Start",
        x_end="Finish",
        y="Job",
        color="Phase",
        title=title,
    )

    # Calculate the height dynamically
    fig.update_layout(height=len(tasks) * 10, yaxis_tickfont_size=14)

    return fig


def parse_args() -> None:
    parser = argparse.ArgumentParser(
        description="Generate the Gantt chart from a given pipeline."
    )
    parser.add_argument("pipeline_url", type=str, help="URLs to the pipeline.")
    parser.add_argument(
        "--token",
        metavar="token",
        help="force GitLab token, otherwise it's read from ~/.config/gitlab-token",
    )
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()

    token = read_token(args.token)

    gl = gitlab.Gitlab(url=GITLAB_URL, private_token=token, retry_transient_errors=True)

    pipeline, _ = get_gitlab_pipeline_from_url(gl, args.pipeline_url)
    fig = generate_gantt_chart(pipeline)
    fig.show()
