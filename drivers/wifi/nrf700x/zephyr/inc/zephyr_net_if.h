/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @brief Header containing network stack interface specific declarations for
 * the Zephyr OS layer of the Wi-Fi driver.
 */

#ifndef __ZEPHYR_NET_IF_H__
#define __ZEPHYR_NET_IF_H__
#include <zephyr/device.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>
#include <fmac_structs.h>
#include <zephyr/net/wifi_mgmt.h>

void wifi_nrf_if_init(struct net_if *iface);
enum ethernet_hw_caps wifi_nrf_if_caps_get(const struct device *dev);
int wifi_nrf_if_send(const struct device *dev, struct net_pkt *pkt);

void wifi_nrf_if_rx_frm(void *os_vif_ctx, void *frm);

enum wifi_nrf_status wifi_nrf_if_state_chg(void *os_vif_ctx, enum wifi_nrf_fmac_if_state if_state);

int wifi_nrf_stats_get(const struct device *dev, struct net_stats_wifi *stats);

#endif /* __ZEPHYR_NET_IF_H__ */
