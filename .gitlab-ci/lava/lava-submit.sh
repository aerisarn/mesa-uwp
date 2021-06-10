#!/bin/bash

set -e
set -x

rm -rf results
mkdir -p results

# Try to use the kernel and rootfs built in mainline first, to save cycles
if wget -q --method=HEAD "${ARTIFACTS_PREFIX}/${FDO_UPSTREAM_REPO}/${DISTRIBUTION_TAG}/${ARCH}/done"; then
	ARTIFACTS_URL="${ARTIFACTS_PREFIX}/${FDO_UPSTREAM_REPO}/${DISTRIBUTION_TAG}/${ARCH}"
else
	ARTIFACTS_URL="${ARTIFACTS_PREFIX}/${CI_PROJECT_PATH}/${DISTRIBUTION_TAG}/${ARCH}"
fi

artifacts/lava/lava_job_submitter.py \
	--template artifacts/lava/lava.yml.jinja2 \
	--pipeline-info "$CI_PIPELINE_URL on $CI_COMMIT_REF_NAME ${CI_NODE_INDEX}/${CI_NODE_TOTAL}" \
	--base-artifacts-url ${ARTIFACTS_URL} \
	--mesa-url ${MESA_URL} \
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
