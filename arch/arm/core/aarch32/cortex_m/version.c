/*
 * Copyright (c) 2020 Atmark Techno, Inc. All Rights Reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */


#include <zephyr/types.h>
#include <toolchain.h>
#include <version.h>

struct version {
	const uint8_t *build_version;
	const uint8_t *kernel_version;
};

Z_GENERIC_SECTION(.version) struct version _version = {
#ifdef BUILD_VERSION
	.build_version = STRINGIFY(BUILD_VERSION),
#endif
	.kernel_version = KERNEL_VERSION_STRING,
};
