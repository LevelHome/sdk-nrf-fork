/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 * Work based on EnOcean Proxy Server model, Copyright (c) 2020 Silvair sp. z o.o.
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <bluetooth/mesh/vnd/silvair_enocean_srv.h>
#include <bluetooth/enocean.h>
#include "model_utils.h"

#define BT_MESH_SILVAIR_ENOCEAN_PROXY_SUB_OP_GET    0x00
#define BT_MESH_SILVAIR_ENOCEAN_PROXY_SUB_OP_SET    0x01
#define BT_MESH_SILVAIR_ENOCEAN_PROXY_SUB_OP_DELETE 0x02
#define BT_MESH_SILVAIR_ENOCEAN_PROXY_SUB_OP_STATUS 0x03

#define PHASE_B_DELAY 150UL
#define ENOCEAN_TIMEOUT_COMP (CONFIG_BT_MESH_SILVAIR_ENOCEAN_WAIT_TIME + \
		PHASE_B_DELAY)
#define MAX_TICKS ((CONFIG_BT_MESH_SILVAIR_ENOCEAN_DELTA_TIMEOUT - ENOCEAN_TIMEOUT_COMP) / \
		  CONFIG_BT_MESH_SILVAIR_ENOCEAN_TICK_TIME)

enum bt_mesh_silvair_enocean_status {
	BT_MESH_SILVAIR_ENOCEAN_STATUS_SET = 0x00,
	BT_MESH_SILVAIR_ENOCEAN_STATUS_NOT_SET = 0x01,
	BT_MESH_SILVAIR_ENOCEAN_STATUS_UNSPECIFIED_ERROR = 0x02,
};

enum phase {
	PHASE_B,
	PHASE_C,
	PHASE_D,
};

static bool initialized;
static sys_slist_t models_list;

static struct bt_mesh_silvair_enocean_srv *model_resolve(const bt_addr_le_t *addr)
{
	struct bt_mesh_silvair_enocean_srv *srv;

	SYS_SLIST_FOR_EACH_CONTAINER(&models_list, srv, entry) {
		if (!bt_addr_le_cmp(&srv->addr, addr)) {
			return srv;
		}
	}

	return NULL;
}

static uint32_t delay_get(struct bt_mesh_model_pub *pub, uint32_t pub_num)
{
	return (BT_MESH_PUB_MSG_TOTAL(pub) - pub_num) * 50;
}

static void send_onoff(struct bt_mesh_silvair_enocean_button *button,
		       bool on_off)
{
	struct bt_mesh_model_transition transition;
	struct bt_mesh_onoff_set msg = {
		.on_off = on_off,
		.transition = &transition,
	};

	button->onoff.pub.retransmit = BT_MESH_PUB_TRANSMIT(3, 50);

	bt_mesh_dtt_srv_transition_get(button->onoff.model, &transition);
	transition.delay = delay_get(&button->onoff.pub, 1);

	bt_mesh_onoff_cli_set_unack(&button->onoff, NULL, &msg);
}

static void send_delta(struct bt_mesh_silvair_enocean_button *button, enum phase phase)
{
	struct bt_mesh_model_transition transition;
	int32_t delta = CONFIG_BT_MESH_SILVAIR_ENOCEAN_DELTA *
		(1 + button->tick_count);

	if (!button->target) {
		delta *= -1;
	}

	struct bt_mesh_lvl_delta_set msg = {
		.delta = delta,
		.new_transaction = button->tick_count == 0,
		.transition = &transition,
	};

	if (CONFIG_BT_MESH_SILVAIR_ENOCEAN_DELTA_TRANSITION_TIME == -1) {
		bt_mesh_dtt_srv_transition_get(button->lvl.model, &transition);
	} else {
		transition.time = CONFIG_BT_MESH_SILVAIR_ENOCEAN_DELTA_TRANSITION_TIME;
	}

	transition.delay = 0;

	switch (phase) {
	case PHASE_B:
		button->lvl.pub.retransmit = BT_MESH_PUB_TRANSMIT(3, 50);
		transition.delay = delay_get(&button->lvl.pub, 1);
		break;
	case PHASE_C:
		button->lvl.pub.retransmit = 0;
		break;
	case PHASE_D:
		button->lvl.pub.retransmit = BT_MESH_PUB_TRANSMIT(4, 50);
		break;
	}

	bt_mesh_lvl_cli_delta_set_unack(&button->lvl, NULL, &msg);
}

static void state_machine_button_process(
		struct bt_mesh_silvair_enocean_button *button,
		enum bt_enocean_button_action action, bool on_off)
{
	switch (action) {
	case BT_ENOCEAN_BUTTON_PRESS:
		button->state = BT_MESH_SILVAIR_ENOCEAN_STATE_WAIT;
		button->target = on_off;
		k_work_reschedule(&button->timer,
			K_MSEC(CONFIG_BT_MESH_SILVAIR_ENOCEAN_WAIT_TIME));
		break;
	case BT_ENOCEAN_BUTTON_RELEASE:
		if (button->state != BT_MESH_SILVAIR_ENOCEAN_STATE_DELTA ||
		    button->target != on_off) {
			send_onoff(button, on_off);
		} else if (button->state == BT_MESH_SILVAIR_ENOCEAN_STATE_DELTA) {
			send_delta(button, PHASE_D);
		}

		/* If cancel fails, the handler will do nothing because the
		 * state is IDLE
		 */
		k_work_cancel_delayable(&button->timer);
		button->state = BT_MESH_SILVAIR_ENOCEAN_STATE_IDLE;
		break;
	}
}

static void button_cb(struct bt_enocean_device *device,
		      enum bt_enocean_button_action action, uint8_t changed,
		      const uint8_t *opt_data, size_t opt_data_len)
{
	struct bt_mesh_silvair_enocean_srv *srv = model_resolve(&device->addr);

	if (!srv) {
		return;
	}

	for (int i = 0; i < BT_MESH_SILVAIR_ENOCEAN_PROXY_BUTTONS * 2; i++) {
		if (BIT(3 - i) & changed) {
			state_machine_button_process(&srv->buttons[i / 2],
						     action, i % 2);
		}
	}
}

static void status_pub(struct bt_mesh_model *model, struct bt_mesh_msg_ctx *ctx,
		       enum bt_mesh_silvair_enocean_status status, const uint8_t *addr)
{
	BT_MESH_MODEL_BUF_DEFINE(buf, BT_MESH_SILVAIR_ENOCEAN_PROXY_OP, 8);
	bt_mesh_model_msg_init(&buf, BT_MESH_SILVAIR_ENOCEAN_PROXY_OP);

	net_buf_simple_add_u8(&buf, BT_MESH_SILVAIR_ENOCEAN_PROXY_SUB_OP_STATUS);
	net_buf_simple_add_u8(&buf, status);

	if (addr) {
		net_buf_simple_add_mem(&buf, addr, 6);
	}

	(void)model_send(model, ctx, &buf);
}

static void timeout(struct k_work *work)
{
	struct bt_mesh_silvair_enocean_button *button =
		CONTAINER_OF(work, struct bt_mesh_silvair_enocean_button,
			     timer.work);
	k_timeout_t tick_timeout =
		K_MSEC(CONFIG_BT_MESH_SILVAIR_ENOCEAN_TICK_TIME);

	switch (button->state) {
	case BT_MESH_SILVAIR_ENOCEAN_STATE_IDLE:
		break;
	case BT_MESH_SILVAIR_ENOCEAN_STATE_WAIT:
		button->state = BT_MESH_SILVAIR_ENOCEAN_STATE_DELTA;
		button->tick_count = 0;
		send_delta(button, PHASE_B);
		break;
	case BT_MESH_SILVAIR_ENOCEAN_STATE_DELTA:
		send_delta(button, PHASE_C);

		if (button->tick_count++ < MAX_TICKS) {
			k_work_reschedule(&button->timer, tick_timeout);
		} else {
			button->state = BT_MESH_SILVAIR_ENOCEAN_STATE_IDLE;
		}

		break;
	}
}

static void enocean_loaded_cb(struct bt_enocean_device *device)
{
	struct bt_mesh_silvair_enocean_srv *srv = model_resolve(&device->addr);

	if (!srv) {
		bt_enocean_decommission(device);
	}
}

#if CONFIG_BT_MESH_SILVAIR_ENOCEAN_AUTO_COMMISSION
static void commissioned_cb(struct bt_enocean_device *device)
{
	struct bt_mesh_silvair_enocean_srv *srv;

	if (model_resolve(&device->addr)) {
		/* This can only happen when device is commissioned by SET message. */
		return;
	}

	srv = model_resolve(BT_ADDR_LE_NONE);
	if (!srv) {
		bt_enocean_decommission(device);
		bt_enocean_commissioning_disable();
		return;
	}

	bt_addr_le_copy(&srv->addr, &device->addr);
#if CONFIG_BT_ENOCEAN_STORE
	(void)bt_mesh_model_data_store(srv->mod, true, NULL, &srv->addr,
				       sizeof(srv->addr));
#endif
	status_pub(srv->mod, NULL, BT_MESH_SILVAIR_ENOCEAN_STATUS_SET, srv->addr.a.val);
}

static void decommissioned_cb(struct bt_enocean_device *device)
{
	struct bt_mesh_silvair_enocean_srv *srv;

	srv = model_resolve(&device->addr);
	if (!srv) {
		return;
	}
	bt_addr_le_copy(&srv->addr, BT_ADDR_LE_NONE);
#if CONFIG_BT_ENOCEAN_STORE
	(void)bt_mesh_model_data_store(srv->mod, true, NULL, NULL, 0);
#endif
	status_pub(srv->mod, NULL, BT_MESH_SILVAIR_ENOCEAN_STATUS_SET, BT_ADDR_LE_NONE->a.val);
}
#endif

static const struct bt_enocean_callbacks enocean_cbs = {
	.button = button_cb,
	.loaded = enocean_loaded_cb,
#if CONFIG_BT_MESH_SILVAIR_ENOCEAN_AUTO_COMMISSION
	.commissioned = commissioned_cb,
	.decommissioned = decommissioned_cb
#endif
};

static void find_and_decommission(struct bt_enocean_device *dev, void *user_data)
{
	bt_addr_le_t *addr = user_data;

	if (!bt_addr_le_cmp(&dev->addr, addr)) {
		bt_enocean_decommission(dev);
	}
}

static void decommission_device(struct bt_mesh_silvair_enocean_srv *srv)
{
	bt_addr_le_t addr = srv->addr;

	bt_addr_le_copy(&srv->addr, BT_ADDR_LE_NONE);
#if CONFIG_BT_ENOCEAN_STORE
	(void)bt_mesh_model_data_store(srv->mod, true, NULL, NULL, 0);
#endif
	bt_enocean_foreach(find_and_decommission, &addr);
}

static int handle_get(struct bt_mesh_model *model,
		       struct bt_mesh_msg_ctx *ctx,
		       struct net_buf_simple *buf)
{
	struct bt_mesh_silvair_enocean_srv *srv = model->user_data;

	if (buf->len != 0) {
		return -EMSGSIZE;
	}

	if (!bt_addr_le_cmp(&srv->addr, BT_ADDR_LE_NONE)) {
		status_pub(model, ctx, BT_MESH_SILVAIR_ENOCEAN_STATUS_NOT_SET, NULL);
	} else {
		status_pub(model, ctx, BT_MESH_SILVAIR_ENOCEAN_STATUS_SET, srv->addr.a.val);
	}

	return 0;
}

static int handle_set(struct bt_mesh_model *model, struct bt_mesh_msg_ctx *ctx,
		      struct net_buf_simple *buf)
{
	if (buf->len != BT_MESH_SILVAIR_ENOCEAN_PROXY_MSG_MAXLEN - 1) {
		return -EMSGSIZE;
	}

	struct bt_mesh_silvair_enocean_srv *srv = model->user_data;
	uint8_t *key;
	uint8_t *addr_raw;

	key = net_buf_simple_pull_mem(buf, 16);
	addr_raw = net_buf_simple_pull_mem(buf, 6);

	if (bt_addr_le_cmp(&srv->addr, BT_ADDR_LE_NONE)) {
		bt_addr_le_t addr = {
			.type = srv->addr.type,
		};

		memcpy(addr.a.val, addr_raw, 6);
		if (bt_addr_le_cmp(&srv->addr, &addr)) {
			status_pub(model, ctx, BT_MESH_SILVAIR_ENOCEAN_STATUS_UNSPECIFIED_ERROR,
				   NULL);
			return -EINVAL;
		}
	} else {
		/* PTM 215B User Manual: By default, PTM 215B uses static source
		 * addresses.
		 */
		srv->addr.type = BT_ADDR_LE_RANDOM;

		/* Copy address before calling bt_enocean_commission() to not associate device with
		 * another model.
		 */
		memcpy(srv->addr.a.val, addr_raw, 6);

		int err = bt_enocean_commission(&srv->addr, key, 0);

		if (err && err != -EBUSY) {
			status_pub(model, ctx, BT_MESH_SILVAIR_ENOCEAN_STATUS_UNSPECIFIED_ERROR,
				   NULL);
			bt_addr_le_copy(&srv->addr, BT_ADDR_LE_NONE);
			return err;
		}
#if CONFIG_BT_ENOCEAN_STORE
		if (err != -EBUSY) {
			(void)bt_mesh_model_data_store(srv->mod, true, NULL,
						       &srv->addr,
						       sizeof(srv->addr));
		}
#endif
	}

	status_pub(model, ctx, BT_MESH_SILVAIR_ENOCEAN_STATUS_SET, srv->addr.a.val);

	return 0;
}

static int handle_delete(struct bt_mesh_model *model, struct bt_mesh_msg_ctx *ctx,
			 struct net_buf_simple *buf)
{
	struct bt_mesh_silvair_enocean_srv *srv = model->user_data;

	if (buf->len != 0) {
		return -EMSGSIZE;
	}

	if (bt_addr_le_cmp(&srv->addr, BT_ADDR_LE_NONE)) {
		decommission_device(srv);
	}

	status_pub(model, ctx, BT_MESH_SILVAIR_ENOCEAN_STATUS_NOT_SET, NULL);

	if (IS_ENABLED(CONFIG_BT_MESH_SILVAIR_ENOCEAN_AUTO_COMMISSION)) {
		bt_enocean_commissioning_enable();
	}

	return 0;
}

static int handle_message(struct bt_mesh_model *model, struct bt_mesh_msg_ctx *ctx,
			  struct net_buf_simple *buf)
{
	uint8_t sub_opcode;

	sub_opcode = net_buf_simple_pull_u8(buf);

	switch (sub_opcode) {
	case BT_MESH_SILVAIR_ENOCEAN_PROXY_SUB_OP_GET:
		return handle_get(model, ctx, buf);
	case BT_MESH_SILVAIR_ENOCEAN_PROXY_SUB_OP_SET:
		return handle_set(model, ctx, buf);
	case BT_MESH_SILVAIR_ENOCEAN_PROXY_SUB_OP_DELETE:
		return handle_delete(model, ctx, buf);
	}

	return -EOPNOTSUPP;
}

const struct bt_mesh_model_op _bt_mesh_silvair_enocean_srv_op[] = {
	{
		BT_MESH_SILVAIR_ENOCEAN_PROXY_OP,
		BT_MESH_LEN_MIN(BT_MESH_SILVAIR_ENOCEAN_PROXY_MSG_MINLEN),
		handle_message,
	},
	BT_MESH_MODEL_OP_END,
};

static void delay_update(struct bt_mesh_model *model, uint32_t offset)
{
	uint32_t delay;

	delay = delay_get(model->pub, BT_MESH_PUB_MSG_NUM(model->pub));
	model->pub->msg->data[offset] = model_delay_encode(delay);
}

static int onoff_update_handler(struct bt_mesh_model *model)
{
	struct bt_mesh_onoff_cli *cli = model->user_data;

	if (cli->pub_buf.len != 6 ||
	    !bt_mesh_model_pub_is_retransmission(model)) {
		return -EINVAL;
	}

	delay_update(model, 5);

	return 0;
}

static int lvl_update_handler(struct bt_mesh_model *model)
{
	struct bt_mesh_lvl_cli *cli = model->user_data;
	struct bt_mesh_silvair_enocean_button *button =
		CONTAINER_OF(cli, struct bt_mesh_silvair_enocean_button, lvl);

	if (cli->pub_buf.len != 9 ||
	    !bt_mesh_model_pub_is_retransmission(model)) {
		return -EINVAL;
	}

	/* In Phase C, retransmissions are disabled and the update callback won't be called. */
	if (button->state != BT_MESH_SILVAIR_ENOCEAN_STATE_DELTA) {
		return 0;
	}

	delay_update(model, 8);

	/** Check if this is the last publication in Phase B. */
	if (BT_MESH_PUB_MSG_TOTAL(model->pub) == BT_MESH_PUB_MSG_NUM(model->pub)) {
		button->tick_count++;
		k_work_reschedule(&button->timer,
				  K_MSEC(CONFIG_BT_MESH_SILVAIR_ENOCEAN_TICK_TIME));
	}

	return 0;
}

static int bt_mesh_silvair_enocean_srv_init(struct bt_mesh_model *model)
{
	if (!initialized) {
		bt_enocean_init(&enocean_cbs);
		initialized = true;
	}

	struct bt_mesh_silvair_enocean_srv *srv = model->user_data;

	srv->mod = model;
	srv->pub.msg = &srv->pub_buf;
	net_buf_simple_init_with_data(&srv->pub_buf, srv->pub_data,
				      sizeof(srv->pub_data));
	bt_addr_le_copy(&srv->addr, BT_ADDR_LE_NONE);

	sys_slist_append(&models_list, &srv->entry);

	for (int i = 0; i < BT_MESH_SILVAIR_ENOCEAN_PROXY_BUTTONS; i++) {
		k_work_init_delayable(&srv->buttons[i].timer, timeout);
	}

	for (int i = 0; i < BT_MESH_SILVAIR_ENOCEAN_PROXY_BUTTONS; i++) {
		srv->buttons[i].onoff.pub.retr_update = true;
		srv->buttons[i].onoff.pub.update = onoff_update_handler;
		srv->buttons[i].lvl.pub.retr_update = true;
		srv->buttons[i].lvl.pub.update = lvl_update_handler;
	}

	return 0;
}

static int bt_mesh_silvair_enocean_srv_start(struct bt_mesh_model *model)
{
	struct bt_mesh_silvair_enocean_srv *srv = model->user_data;

	if (IS_ENABLED(CONFIG_BT_MESH_SILVAIR_ENOCEAN_AUTO_COMMISSION) &&
	    !bt_addr_le_cmp(&srv->addr, BT_ADDR_LE_NONE)) {
		bt_enocean_commissioning_enable();
	}

	return 0;
}

static void bt_mesh_silvair_enocean_srv_reset(struct bt_mesh_model *model)
{
	struct bt_mesh_silvair_enocean_srv *srv = model->user_data;

	if (IS_ENABLED(CONFIG_BT_MESH_SILVAIR_ENOCEAN_AUTO_COMMISSION)) {
		bt_enocean_commissioning_disable();
	}

	if (bt_addr_le_cmp(&srv->addr, BT_ADDR_LE_NONE)) {
		decommission_device(srv);
	}

#if defined(CONFIG_ZTEST)
	sys_slist_find_and_remove(&models_list, &srv->entry);
#endif

	for (int i = 0; i < BT_MESH_SILVAIR_ENOCEAN_PROXY_BUTTONS; i++) {
		/* If cancel fails, the handler will do nothing because the
		 * state is IDLE
		 */
		k_work_cancel_delayable(&srv->buttons[i].timer);
		srv->buttons[i].state =
			BT_MESH_SILVAIR_ENOCEAN_STATE_IDLE;
	}
}

static int bt_mesh_silvair_enocean_srv_settings_set(
		struct bt_mesh_model *model, const char *name, size_t len_rd,
		settings_read_cb read_cb, void *cb_arg)
{
	struct bt_mesh_silvair_enocean_srv *srv = model->user_data;
	ssize_t result;

	if (name) {
		return -ENOENT;
	}

	result = read_cb(cb_arg, &srv->addr, sizeof(srv->addr));

	if (result < sizeof(srv->addr)) {
		return -EINVAL;
	}

	return 0;
}

const struct bt_mesh_model_cb _bt_mesh_silvair_enocean_srv_cb = {
	.init = bt_mesh_silvair_enocean_srv_init,
	.reset = bt_mesh_silvair_enocean_srv_reset,
	.start = bt_mesh_silvair_enocean_srv_start,
	.settings_set = bt_mesh_silvair_enocean_srv_settings_set
};
