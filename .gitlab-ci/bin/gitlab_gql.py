#!/usr/bin/env python3

import re
from argparse import ArgumentParser, Namespace
from dataclasses import dataclass, field
from itertools import chain
from pathlib import Path
from typing import Any, Pattern

from gql import Client, gql
from gql.transport.aiohttp import AIOHTTPTransport
from graphql import DocumentNode

Dag = dict[str, list[str]]


@dataclass
class GitlabGQL:
    _transport: Any = field(init=False)
    client: Client = field(init=False)
    url: str = "https://gitlab.freedesktop.org/api/graphql"

    def __post_init__(self):
        self._setup_gitlab_gql_client()

    def _setup_gitlab_gql_client(self) -> Client:
        # Select your transport with a defined url endpoint
        self._transport = AIOHTTPTransport(url=self.url)

        # Create a GraphQL client using the defined transport
        self.client = Client(
            transport=self._transport, fetch_schema_from_transport=True
        )

    def query(self, gql_file: Path | str, params: dict[str, Any]) -> dict[str, Any]:
        # Provide a GraphQL query
        source_path = Path(__file__).parent
        pipeline_query_file = source_path / gql_file

        query: DocumentNode
        with open(pipeline_query_file, "r") as f:
            pipeline_query = f.read()
            query = gql(pipeline_query)

        # Execute the query on the transport
        return self.client.execute(query, variable_values=params)


def create_job_needs_dag(
    gl_gql: GitlabGQL, params
) -> tuple[Dag, dict[str, dict[str, Any]]]:

    result = gl_gql.query("pipeline_details.gql", params)
    dag = {}
    jobs = {}
    pipeline = result["project"]["pipeline"]
    if not pipeline:
        raise RuntimeError(f"Could not find any pipelines for {params}")

    for stage in pipeline["stages"]["nodes"]:
        for stage_job in stage["groups"]["nodes"]:
            for job in stage_job["jobs"]["nodes"]:
                needs = job.pop("needs")["nodes"]
                jobs[job["name"]] = job
                dag[job["name"]] = {node["name"] for node in needs}

    for job, needs in dag.items():
        needs: set
        partial = True

        while partial:
            next_depth = {n for dn in needs for n in dag[dn]}
            partial = not needs.issuperset(next_depth)
            needs = needs.union(next_depth)

        dag[job] = needs

    return dag, jobs


def filter_dag(dag: Dag, regex: Pattern) -> Dag:
    return {job: needs for job, needs in dag.items() if re.match(regex, job)}


def print_dag(dag: Dag) -> None:
    for job, needs in dag.items():
        print(f"{job}:")
        print(f"\t{' '.join(needs)}")
        print()


def parse_args() -> Namespace:
    parser = ArgumentParser()
    parser.add_argument("-pp", "--project-path", type=str, default="mesa/mesa")
    parser.add_argument("--sha", type=str, required=True)
    parser.add_argument("--regex", type=str, required=False)
    parser.add_argument("--print-dag", action="store_true")

    return parser.parse_args()


def main():
    args = parse_args()
    gl_gql = GitlabGQL()

    if args.print_dag:
        dag, jobs = create_job_needs_dag(
            gl_gql, {"projectPath": args.project_path, "sha": args.sha}
        )

        if args.regex:
            dag = filter_dag(dag, re.compile(args.regex))
        print_dag(dag)


if __name__ == "__main__":
    main()
