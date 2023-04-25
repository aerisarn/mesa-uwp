"""
In a few words: some devices in Mesa CI has problematic serial connection, they
may hang (become silent) intermittently. Every time it hangs for minutes, the
job is retried, causing delays in the overall pipeline executing, ultimately
blocking legit MRs to merge.

To reduce reliance on UART, we explored LAVA features, such as running docker
containers as a test alongside the DUT one, to be able to create an SSH server
in the DUT the earliest possible and an SSH client in a docker container, to
establish a SSH session between both, allowing the console output to be passed
via SSH pseudo terminal, instead of relying in the error-prone UART.

In more detail, we aim to use "export -p" to share the initial boot environment
with SSH LAVA test-cases.
The "init-stage1.sh" script handles tasks such as system mounting and network
setup, which are necessary for allocating a pseudo-terminal under "/dev/pts".
Although these chores are not required for establishing an SSH session, they are
essential for proper functionality to the target script given by HWCI_SCRIPT
environment variable.

Therefore, we have divided the job definition into four parts:

1. [DUT] Logging in to DUT and run the SSH server with root access.
2. [DUT] Running the "init-stage1.sh" script for the first SSH test case.
3. [DUT] Export the first boot environment to `/dut-env-vars.sh` file.
4. [SSH] Enabling the pseudo-terminal for colors and running the "init-stage2.sh"
script after sourcing "dut-env-vars.sh" again for the second SSH test case.
"""

import re

from os import getenv
from pathlib import Path
from typing import Any
from ruamel.yaml.scalarstring import LiteralScalarString

# How many attempts should be made when a timeout happen during LAVA device boot.
NUMBER_OF_ATTEMPTS_LAVA_BOOT = int(getenv("LAVA_NUMBER_OF_ATTEMPTS_LAVA_BOOT", 3))

# Supports any integers in [0, 100].
# The scheduler considers the job priority when ordering the queue
# to consider which job should run next.
JOB_PRIORITY = int(getenv("LAVA_JOB_PRIORITY", 75))

# Very early SSH server setup. Uses /dut_ready file to flag it is done.
SSH_SERVER_COMMANDS = {
    "auto_login": {
        "login_commands": [
            "dropbear -R -B",
            "touch /dut_ready",
        ],
        "login_prompt": "ogin:",
        # To login as root, the username should be empty
        "username": "",
    }
}

# TODO: Extract this inline script to a shell file, like we do with
# init-stage[12].sh
# The current way is difficult to maintain because one has to deal with escaping
# characters for both Python and the resulting job definition YAML.
# Plus, it always good to lint bash scripts with shellcheck.
DOCKER_COMMANDS = [
    """set -ex
timeout 1m bash << EOF
while [ -z "$(lava-target-ip)" ]; do
    echo Waiting for DUT to join LAN;
    sleep 1;
done
EOF

ping -c 5 -w 60 $(lava-target-ip)

lava_ssh_test_case() {
    set -x
    local test_case="${1}"
    shift
    lava-test-case \"${test_case}\" --shell \\
        ssh ${SSH_PTY_ARGS:--T} \\
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \\
        root@$(lava-target-ip) \"${@}\"
}""",
]


def to_yaml_block(steps_array: list[str], escape_vars=[]) -> LiteralScalarString:
    def escape_envvar(match):
        return "\\" + match.group(0)

    filtered_array = [s for s in steps_array if s.strip() and not s.startswith("#")]
    final_str = "\n".join(filtered_array)

    for escape_var in escape_vars:
        # Find env vars and add '\\' before them
        final_str = re.sub(rf"\${escape_var}*", escape_envvar, final_str)
    return LiteralScalarString(final_str)


def artifact_download_steps(args):
    """
    This function is responsible for setting up the SSH server in the DUT and to
    export the first boot environment to a file.
    """
    # Putting JWT pre-processing and mesa download, within init-stage1.sh file,
    # as we do with non-SSH version.
    download_steps = [
        "set -ex",
        "source /dut-env-vars.sh",
        "curl -L --retry 4 -f --retry-all-errors --retry-delay 60 "
        f"{args.job_rootfs_overlay_url} | tar -xz -C /",
        f"mkdir -p {args.ci_project_dir}",
        f"curl -L --retry 4 -f --retry-all-errors --retry-delay 60 {args.build_url} | "
        f"tar --zstd -x -C {args.ci_project_dir}",
    ]

    # If the JWT file is provided, we will use it to authenticate with the cloud
    # storage provider and will hide it from the job output in Gitlab.
    if args.jwt_file:
        with open(args.jwt_file) as jwt_file:
            download_steps += [
                "set +x",
                f'echo -n "{jwt_file.read()}" > "{args.jwt_file}"  # HIDEME',
                "set -x",
                f'echo "export CI_JOB_JWT_FILE={args.jwt_file}" >> /set-job-env-vars.sh',
            ]
    else:
        download_steps += [
            "echo Could not find jwt file, disabling S3 requests...",
            "sed -i '/MINIO_RESULTS_UPLOAD/d' /set-job-env-vars.sh",
        ]

    return download_steps


def generate_dut_test(args):
    # Commands executed on DUT.
    # Trying to execute the minimal number of commands, because the console data is
    # retrieved via UART, which is hang-prone in some devices.

    first_stage_steps: list[str] = Path(args.first_stage_init).read_text().splitlines()
    return {
        "namespace": "dut",
        "definitions": [
            {
                "from": "inline",
                "name": "setup-ssh-server",
                "path": "inline-setup-ssh-server",
                "repository": {
                    "metadata": {
                        "format": "Lava-Test Test Definition 1.0",
                        "name": "dut-env-export",
                    },
                    "run": {
                        "steps": [
                            to_yaml_block(first_stage_steps),
                            "export -p > /dut-env-vars.sh",  # Exporting the first boot environment
                        ],
                    },
                },
            }
        ],
    }


def generate_docker_test(args):
    # This is a growing list of commands that will be executed by the docker
    # guest, which will be the SSH client.
    docker_commands = []

    # LAVA test wrapping Mesa CI job in a SSH session.
    init_stages_test = {
        "namespace": "container",
        "timeout": {"minutes": args.job_timeout_min},
        "failure_retry": 1,
        "definitions": [
            {
                "name": "docker_ssh_client",
                "from": "inline",
                "path": "inline/docker_ssh_client.yaml",
                "repository": {
                    "metadata": {
                        "name": "mesa",
                        "description": "Mesa test plan",
                        "format": "Lava-Test Test Definition 1.0",
                    },
                    "run": {"steps": docker_commands},
                },
            }
        ],
        "docker": {
            "image": "registry.gitlab.collabora.com/lava/health-check-docker:wip-laura-ping-ssh-support",
        },
    }

    docker_commands += [
        to_yaml_block(DOCKER_COMMANDS, escape_vars=["LAVA_TARGET_IP"]),
        "lava_ssh_test_case 'wait_for_dut_login' << EOF",
        "while [ ! -e /dut_ready ]; do sleep 1; done;",
        "EOF",
        to_yaml_block(
            (
                "lava_ssh_test_case 'artifact_download' 'bash --' << EOF",
                *artifact_download_steps(args),
                "EOF",
            )
        ),
        "export SSH_PTY_ARGS=-tt",
        # Putting CI_JOB name as the testcase name, it may help LAVA farm
        # maintainers with monitoring
        f"lava_ssh_test_case 'mesa-ci_{args.mesa_job_name}' "
        # Changing directory to /, as the HWCI_SCRIPT expects that
        "'\"cd / && /init-stage2.sh\"'",
    ]

    return init_stages_test


def generate_lava_yaml_payload(args) -> dict[str, Any]:
    # General metadata and permissions
    values = {
        "job_name": f"mesa: {args.pipeline_info}",
        "device_type": args.device_type,
        "visibility": {"group": [args.visibility_group]},
        "priority": JOB_PRIORITY,
        "context": {
            "extra_nfsroot_args": " init=/init rootwait usbcore.quirks=0bda:8153:k"
        },
        "timeouts": {
            "job": {"minutes": args.job_timeout_min},
            "actions": {
                "depthcharge-retry": {
                    # Could take between 1 and 1.5 min in slower boots
                    "minutes": 4
                },
                "depthcharge-start": {
                    # Should take less than 1 min.
                    "minutes": 1,
                },
                "depthcharge-action": {
                    # This timeout englobes the entire depthcharge timing,
                    # including retries
                    "minutes": 5
                    * NUMBER_OF_ATTEMPTS_LAVA_BOOT,
                },
            },
        },
    }

    if args.lava_tags:
        values["tags"] = args.lava_tags.split(",")

    # URLs to our kernel rootfs to boot from, both generated by the base
    # container build
    deploy = {
        "namespace": "dut",
        "timeout": {"minutes": 10},
        "to": "tftp",
        "os": "oe",
        "kernel": {"url": f"{args.kernel_url_prefix}/{args.kernel_image_name}"},
        "nfsrootfs": {
            "url": f"{args.rootfs_url_prefix}/lava-rootfs.tar.zst",
            "compression": "zstd",
        },
    }
    if args.kernel_image_type:
        deploy["kernel"]["type"] = args.kernel_image_type
    if args.dtb_filename:
        deploy["dtb"] = {"url": f"{args.kernel_url_prefix}/{args.dtb_filename}.dtb"}

    # always boot over NFS
    boot = {
        "namespace": "dut",
        "failure_retry": NUMBER_OF_ATTEMPTS_LAVA_BOOT,
        "method": args.boot_method,
        "commands": "nfs",
        "prompts": ["lava-shell:"],
        **SSH_SERVER_COMMANDS,
    }

    # only declaring each job as a single 'test' since LAVA's test parsing is
    # not useful to us
    values["actions"] = [
        {"deploy": deploy},
        {"boot": boot},
        {"test": generate_dut_test(args)},
        {"test": generate_docker_test(args)},
    ]

    return values
