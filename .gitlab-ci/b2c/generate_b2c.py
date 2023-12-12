#!/usr/bin/env python3

# Copyright © 2022 Valve Corporation
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
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

from jinja2 import Environment, FileSystemLoader
from os import environ, path
import json


env = Environment(loader=FileSystemLoader(path.dirname(environ['B2C_JOB_TEMPLATE'])),
                  trim_blocks=True, lstrip_blocks=True)

template = env.get_template(path.basename(environ['B2C_JOB_TEMPLATE']))

values = {}
values['ci_job_id'] = environ['CI_JOB_ID']
values['container_cmd'] = environ['B2C_TEST_SCRIPT']
values['initramfs_url'] = environ['B2C_INITRAMFS_URL']
values['job_success_regex'] = environ['B2C_JOB_SUCCESS_REGEX']
values['job_warn_regex'] = environ['B2C_JOB_WARN_REGEX']
values['kernel_url'] = environ['B2C_KERNEL_URL']
values['log_level'] = environ['B2C_LOG_LEVEL']
values['poweroff_delay'] = environ['B2C_POWEROFF_DELAY']
values['session_end_regex'] = environ['B2C_SESSION_END_REGEX']
values['session_reboot_regex'] = environ['B2C_SESSION_REBOOT_REGEX']
try:
    values['tags'] = json.loads(environ['CI_RUNNER_TAGS'])
except json.decoder.JSONDecodeError:
    values['tags'] = environ['CI_RUNNER_TAGS'].split(",")
values['template'] = environ['B2C_JOB_TEMPLATE']
values['timeout_boot_minutes'] = environ['B2C_TIMEOUT_BOOT_MINUTES']
values['timeout_boot_retries'] = environ['B2C_TIMEOUT_BOOT_RETRIES']
values['timeout_first_minutes'] = environ['B2C_TIMEOUT_FIRST_MINUTES']
values['timeout_first_retries'] = environ['B2C_TIMEOUT_FIRST_RETRIES']
values['timeout_minutes'] = environ['B2C_TIMEOUT_MINUTES']
values['timeout_overall_minutes'] = environ['B2C_TIMEOUT_OVERALL_MINUTES']
values['timeout_retries'] = environ['B2C_TIMEOUT_RETRIES']
exclusions = environ['B2C_JOB_VOLUME_EXCLUSIONS'].split(",")
values['job_volume_exclusions'] = [excl for excl in exclusions if excl]
values['working_dir'] = environ['CI_PROJECT_DIR']

# Use the gateway's pull-through registry caches to reduce load on fd.o.
values['local_container'] = environ['IMAGE_UNDER_TEST']
values['local_container'] = values['local_container'].replace(
    'registry.freedesktop.org',
    '{{ fdo_proxy_registry }}'
)

values['cmdline_extras'] = environ.get('B2C_KERNEL_CMDLINE_EXTRAS', '')

with open(path.splitext(path.basename(environ['B2C_JOB_TEMPLATE']))[0], "w") as f:
    f.write(template.render(values))
