#!/bin/bash

set -e
set -x

rm -rf results
mkdir -p results/job-rootfs-overlay/
artifacts/ci-common/generate-env.sh > results/job-rootfs-overlay/environment.sh

# Try to use the kernel and rootfs built in mainline first, so we're more
# likely to hit cache
if wget -q --method=HEAD "https://${BASE_SYSTEM_MAINLINE_HOST_PATH}/done"; then
	BASE_SYSTEM_HOST_PATH="${BASE_SYSTEM_MAINLINE_HOST_PATH}"
else
	BASE_SYSTEM_HOST_PATH="${BASE_SYSTEM_FORK_HOST_PATH}"
fi

tar zcf job-rootfs-overlay.tar.gz -C results/job-rootfs-overlay/ .
ci-fairy minio login "${CI_JOB_JWT}"
ci-fairy minio cp job-rootfs-overlay.tar.gz "minio://${JOB_ROOTFS_OVERLAY_PATH}"

artifacts/lava/lava_job_submitter.py \
	--template artifacts/lava/lava.yml.jinja2 \
	--pipeline-info "$CI_JOB_NAME: $CI_PIPELINE_URL on $CI_COMMIT_REF_NAME ${CI_NODE_INDEX}/${CI_NODE_TOTAL}" \
	--base-system-url-prefix "https://${BASE_SYSTEM_HOST_PATH}" \
	--mesa-build-url "${FDO_HTTP_CACHE_URI:-}https://${MESA_BUILD_PATH}" \
	--job-rootfs-overlay-url "${FDO_HTTP_CACHE_URI:-}https://${JOB_ROOTFS_OVERLAY_PATH}" \
	--device-type ${DEVICE_TYPE} \
	--dtb ${DTB} \
	--jwt "${CI_JOB_JWT}" \
	--kernel-image-name ${KERNEL_IMAGE_NAME} \
	--kernel-image-type "${KERNEL_IMAGE_TYPE}" \
	--boot-method ${BOOT_METHOD} \
	--lava-tags "${LAVA_TAGS}" | tee results/lava.log
