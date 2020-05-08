/*
 * Copyright (c) 2020 Atmark Techno, Inc. All Rights Reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

    SECTION_PROLOGUE(.version,,)
	{
		KEEP(*(.version))
	} GROUP_LINK_IN(ROMABLE_REGION)
