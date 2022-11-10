/*
 *Copyright (c) 2022 Nordic Semiconductor ASA
 *
 *SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef FOTA_DOWNLOAD_UTIL_H__
#define FOTA_DOWNLOAD_UTIL_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Find correct MCUBoot update resource locator entry in space separated string.
 *
 * When supporting upgrades of the MCUBoot bootloader, two variants of the new firmware are
 * presented, each at a different address. This function is told which slot is active, and sets the
 * update pointer to point to the correct non-active entry in the dual resource locator.
 *
 * @param[in, out] file		 Pointer to dual resource locator with space separator. Note
 *				 that the space separator will be replaced by a NULL terminator.
 *
 * @param[in]      s0_active     Boolean indicating if S0 is the currently active slot.
 * @param[out]     selected_path Pointer to correct file MCUBoot bootloader upgrade. Will be set to
 *				 NULL if no space separator is found.
 *
 * @retval - 0 If successful (note that this does not imply that a space separator was found)
	   - negative errno otherwise.
 */
int fota_download_parse_dual_resource_locator(char *const file, bool s0_active,
					     const char **selected_path);

#ifdef __cplusplus
}
#endif

#endif /*FOTA_DOWNLOAD_UTIL_H__ */
