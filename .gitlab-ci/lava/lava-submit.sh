#!/bin/bash

set -e
set -x

rm -rf results
mkdir -p results

# Try to use the kernel and rootfs built in mainline first, so we're more
# likely to hit cache
if wget -q --method=HEAD "https://${BASE_SYSTEM_MAINLINE_HOST_PATH}/done"; then
	BASE_SYSTEM_HOST_PATH="${BASE_SYSTEM_MAINLINE_HOST_PATH}"
else
	BASE_SYSTEM_HOST_PATH="${BASE_SYSTEM_FORK_HOST_PATH}"
fi

artifacts/lava/lava_job_submitter.py \
	--template artifacts/lava/lava.yml.jinja2 \
	--pipeline-info "$CI_PIPELINE_URL on $CI_COMMIT_REF_NAME ${CI_NODE_INDEX}/${CI_NODE_TOTAL}" \
	--base-system-url-prefix "https://${BASE_SYSTEM_HOST_PATH}" \
	--mesa-build-url "${FDO_HTTP_CACHE_URI:-}https://${MESA_BUILD_PATH}" \
	--device-type ${DEVICE_TYPE} \
	--dtb ${DTB} \
	--env-vars "${ENV_VARS} ${FIXED_ENV_VARS}" \
	--jwt "${CI_JOB_JWT}" \
	--deqp-version ${DEQP_VERSION} \
	--kernel-image-name ${KERNEL_IMAGE_NAME} \
	--kernel-image-type "${KERNEL_IMAGE_TYPE}" \
	--gpu-version ${GPU_VERSION} \
	--boot-method ${BOOT_METHOD} \
	--lava-tags "${LAVA_TAGS}" \
	--ci-node-index "${CI_NODE_INDEX}" \
	--ci-node-total "${CI_NODE_TOTAL}" | tee results/lava.log
