// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Force feedback support for Logitech Gaming Wheels
 *
 *  Including G27, G25, DFP, DFGT, FFEX, Momo, Momo2 &
 *  Speed Force Wireless (WiiWheel)
 *
 *  Copyright (c) 2010 Simon Wood <simon@mungewell.org>
 *  Copyright (c) 2019 Bernat Arlandis <berarma@hotmail.com>
 */

#include <linux/module.h>
#include <linux/input.h>
#include <linux/usb.h>
#include <linux/hid.h>
#include <linux/fixp-arith.h>

#include "usbhid/usbhid.h"
#include "hid-lg.h"
#include "hid-lg4ff.h"
#include "hid-ids.h"

#define LG4FF_MMODE_IS_MULTIMODE 0
#define LG4FF_MMODE_SWITCHED 1
#define LG4FF_MMODE_NOT_MULTIMODE 2

#define LG4FF_MODE_NATIVE_IDX 0
#define LG4FF_MODE_DFEX_IDX 1
#define LG4FF_MODE_DFP_IDX 2
#define LG4FF_MODE_G25_IDX 3
#define LG4FF_MODE_DFGT_IDX 4
#define LG4FF_MODE_G27_IDX 5
#define LG4FF_MODE_G29_IDX 6
#define LG4FF_MODE_MAX_IDX 7

#define LG4FF_MODE_NATIVE BIT(LG4FF_MODE_NATIVE_IDX)
#define LG4FF_MODE_DFEX BIT(LG4FF_MODE_DFEX_IDX)
#define LG4FF_MODE_DFP BIT(LG4FF_MODE_DFP_IDX)
#define LG4FF_MODE_G25 BIT(LG4FF_MODE_G25_IDX)
#define LG4FF_MODE_DFGT BIT(LG4FF_MODE_DFGT_IDX)
#define LG4FF_MODE_G27 BIT(LG4FF_MODE_G27_IDX)
#define LG4FF_MODE_G29 BIT(LG4FF_MODE_G29_IDX)

#define LG4FF_DFEX_TAG "DF-EX"
#define LG4FF_DFEX_NAME "Driving Force / Formula EX"
#define LG4FF_DFP_TAG "DFP"
#define LG4FF_DFP_NAME "Driving Force Pro"
#define LG4FF_G25_TAG "G25"
#define LG4FF_G25_NAME "G25 Racing Wheel"
#define LG4FF_G27_TAG "G27"
#define LG4FF_G27_NAME "G27 Racing Wheel"
#define LG4FF_G29_TAG "G29"
#define LG4FF_G29_NAME "G29 Racing Wheel"
#define LG4FF_DFGT_TAG "DFGT"
#define LG4FF_DFGT_NAME "Driving Force GT"

#define LG4FF_FFEX_REV_MAJ 0x21
#define LG4FF_FFEX_REV_MIN 0x00

#define DEBUG(...) pr_debug("lg4ff: " __VA_ARGS__)
#define time_diff(a,b) ({ \
		typecheck(unsigned long, a); \
		typecheck(unsigned long, b); \
		((a) - (long)(b)); })
#define CLAMP_VALUE_U16(x) ((unsigned short)((x) > 0xffff ? 0xffff : (x)))
#define CLAMP_VALUE_S16(x) ((unsigned short)((x) <= -0x8000 ? -0x8000 : ((x) > 0x7fff ? 0x7fff : (x))))
#define SCALE_VALUE_U16(x, bits) (CLAMP_VALUE_U16(x) >> (16 - bits))
#define TRANSLATE_FORCE(x) ((CLAMP_VALUE_S16(x) + 0x8000) >> 8)
#define STOP_EFFECT(state) ((state)->flags = 0)

#define DEFAULT_TIMER_PERIOD 4
#define LG4FF_MAX_EFFECTS 16

#define FF_EFFECT_STARTED 0
#define FF_EFFECT_ALLSET 1
#define FF_EFFECT_PLAYING 2
#define FF_EFFECT_UPDATING 3

static int timer_msecs = DEFAULT_TIMER_PERIOD;
module_param(timer_msecs, int, 0660);
MODULE_PARM_DESC(timer_msecs, "Timer resolution in msecs (it will be rounded up to jiffies).");

static int fixed_loop = 0;
module_param(fixed_loop, int, 0);
MODULE_PARM_DESC(fixed_loop, "Put the device into fixed loop mode.");

static void lg4ff_set_range_dfp(struct hid_device *hid, u16 range);
static void lg4ff_set_range_g25(struct hid_device *hid, u16 range);

struct lg4ff_effect_state {
	struct ff_effect effect;
	struct ff_envelope *envelope;
	unsigned long start_at;
	unsigned long play_at;
	unsigned long stop_at;
	unsigned long flags;
	unsigned long time_playing;
	unsigned long updated_at;
	unsigned int phase;
	unsigned int phase_adj;
	unsigned int count;
	unsigned int cmd;
	unsigned int cmd_start_time;
	unsigned int cmd_start_count;
	int direction_gain;
	int slope;
};

struct lg4ff_effect_parameters {
	int level;
	unsigned int d1;
	unsigned int d2;
	int k1;
	int k2;
	unsigned int clip;
};

struct lg4ff_slot {
	int id;
	struct lg4ff_effect_parameters parameters;
	__u8 current_cmd[7];
	int cmd_op;
	int is_updated;
	int effect_type;
};

struct lg4ff_wheel_data {
	const u32 product_id;
	u16 combine;
	u16 range;
	const u16 min_range;
	const u16 max_range;
#ifdef CONFIG_LEDS_CLASS
	u8  led_state;
	struct led_classdev *led[5];
#endif
	const u32 alternate_modes;
	const char * const real_tag;
	const char * const real_name;
	const u16 real_product_id;

	void (*set_range)(struct hid_device *hid, u16 range);
};

struct lg4ff_device_entry {
	spinlock_t report_lock; /* Protect output HID report */
	spinlock_t timer_lock;
	struct hid_report *report;
	struct lg4ff_wheel_data wdata;

	struct hid_device *hid;
	struct timer_list timer;
	struct lg4ff_slot slots[4];
	struct lg4ff_effect_state states[LG4FF_MAX_EFFECTS];
	int effects_used;
	u16 gain;
};

static const signed short lg4ff_wheel_effects[] = {
	FF_CONSTANT,
	FF_SPRING,
	FF_DAMPER,
	FF_AUTOCENTER,
	FF_PERIODIC,
	FF_SINE,
	FF_SQUARE,
	FF_TRIANGLE,
	FF_SAW_UP,
	FF_SAW_DOWN,
	FF_RAMP,
	FF_FRICTION,
	-1
};

static const signed short no_wheel_effects[] = {
	-1
};

struct lg4ff_wheel {
	const u32 product_id;
	const signed short *ff_effects;
	const u16 min_range;
	const u16 max_range;
	void (*set_range)(struct hid_device *hid, u16 range);
};

struct lg4ff_compat_mode_switch {
	const u8 cmd_count;	/* Number of commands to send */
	const u8 cmd[];
};

struct lg4ff_wheel_ident_info {
	const u32 modes;
	const u16 mask;
	const u16 result;
	const u16 real_product_id;
};

struct lg4ff_multimode_wheel {
	const u16 product_id;
	const u32 alternate_modes;
	const char *real_tag;
	const char *real_name;
};

struct lg4ff_alternate_mode {
	const u16 product_id;
	const char *tag;
	const char *name;
};

static const struct lg4ff_wheel lg4ff_devices[] = {
	{USB_DEVICE_ID_LOGITECH_WINGMAN_FG,  no_wheel_effects,    40, 180, NULL},
	{USB_DEVICE_ID_LOGITECH_WINGMAN_FFG, lg4ff_wheel_effects, 40, 180, NULL},
	{USB_DEVICE_ID_LOGITECH_WHEEL,       lg4ff_wheel_effects, 40, 270, NULL},
	{USB_DEVICE_ID_LOGITECH_MOMO_WHEEL,  lg4ff_wheel_effects, 40, 270, NULL},
	{USB_DEVICE_ID_LOGITECH_DFP_WHEEL,   lg4ff_wheel_effects, 40, 900, lg4ff_set_range_dfp},
	{USB_DEVICE_ID_LOGITECH_G25_WHEEL,   lg4ff_wheel_effects, 40, 900, lg4ff_set_range_g25},
	{USB_DEVICE_ID_LOGITECH_DFGT_WHEEL,  lg4ff_wheel_effects, 40, 900, lg4ff_set_range_g25},
	{USB_DEVICE_ID_LOGITECH_G27_WHEEL,   lg4ff_wheel_effects, 40, 900, lg4ff_set_range_g25},
	{USB_DEVICE_ID_LOGITECH_G29_WHEEL,   lg4ff_wheel_effects, 40, 900, lg4ff_set_range_g25},
	{USB_DEVICE_ID_LOGITECH_MOMO_WHEEL2, lg4ff_wheel_effects, 40, 270, NULL},
	{USB_DEVICE_ID_LOGITECH_WII_WHEEL,   lg4ff_wheel_effects, 40, 270, NULL}
};

static const struct lg4ff_multimode_wheel lg4ff_multimode_wheels[] = {
	{USB_DEVICE_ID_LOGITECH_DFP_WHEEL,
	 LG4FF_MODE_NATIVE | LG4FF_MODE_DFP | LG4FF_MODE_DFEX,
	 LG4FF_DFP_TAG, LG4FF_DFP_NAME},
	{USB_DEVICE_ID_LOGITECH_G25_WHEEL,
	 LG4FF_MODE_NATIVE | LG4FF_MODE_G25 | LG4FF_MODE_DFP | LG4FF_MODE_DFEX,
	 LG4FF_G25_TAG, LG4FF_G25_NAME},
	{USB_DEVICE_ID_LOGITECH_DFGT_WHEEL,
	 LG4FF_MODE_NATIVE | LG4FF_MODE_DFGT | LG4FF_MODE_DFP | LG4FF_MODE_DFEX,
	 LG4FF_DFGT_TAG, LG4FF_DFGT_NAME},
	{USB_DEVICE_ID_LOGITECH_G27_WHEEL,
	 LG4FF_MODE_NATIVE | LG4FF_MODE_G27 | LG4FF_MODE_G25 | LG4FF_MODE_DFP | LG4FF_MODE_DFEX,
	 LG4FF_G27_TAG, LG4FF_G27_NAME},
	{USB_DEVICE_ID_LOGITECH_G29_WHEEL,
	 LG4FF_MODE_NATIVE | LG4FF_MODE_G29 | LG4FF_MODE_G27 | LG4FF_MODE_G25 | LG4FF_MODE_DFGT | LG4FF_MODE_DFP | LG4FF_MODE_DFEX,
	 LG4FF_G29_TAG, LG4FF_G29_NAME},
};

static const struct lg4ff_alternate_mode lg4ff_alternate_modes[] = {
	[LG4FF_MODE_NATIVE_IDX] = {0, "native", ""},
	[LG4FF_MODE_DFEX_IDX] = {USB_DEVICE_ID_LOGITECH_WHEEL, LG4FF_DFEX_TAG, LG4FF_DFEX_NAME},
	[LG4FF_MODE_DFP_IDX] = {USB_DEVICE_ID_LOGITECH_DFP_WHEEL, LG4FF_DFP_TAG, LG4FF_DFP_NAME},
	[LG4FF_MODE_G25_IDX] = {USB_DEVICE_ID_LOGITECH_G25_WHEEL, LG4FF_G25_TAG, LG4FF_G25_NAME},
	[LG4FF_MODE_DFGT_IDX] = {USB_DEVICE_ID_LOGITECH_DFGT_WHEEL, LG4FF_DFGT_TAG, LG4FF_DFGT_NAME},
	[LG4FF_MODE_G27_IDX] = {USB_DEVICE_ID_LOGITECH_G27_WHEEL, LG4FF_G27_TAG, LG4FF_G27_NAME},
	[LG4FF_MODE_G29_IDX] = {USB_DEVICE_ID_LOGITECH_G29_WHEEL, LG4FF_G29_TAG, LG4FF_G29_NAME},
};

/* Multimode wheel identificators */
static const struct lg4ff_wheel_ident_info lg4ff_dfp_ident_info = {
	LG4FF_MODE_DFP | LG4FF_MODE_DFEX,
	0xf000,
	0x1000,
	USB_DEVICE_ID_LOGITECH_DFP_WHEEL
};

static const struct lg4ff_wheel_ident_info lg4ff_g25_ident_info = {
	LG4FF_MODE_G25 | LG4FF_MODE_DFP | LG4FF_MODE_DFEX,
	0xff00,
	0x1200,
	USB_DEVICE_ID_LOGITECH_G25_WHEEL
};

static const struct lg4ff_wheel_ident_info lg4ff_g27_ident_info = {
	LG4FF_MODE_G27 | LG4FF_MODE_G25 | LG4FF_MODE_DFP | LG4FF_MODE_DFEX,
	0xfff0,
	0x1230,
	USB_DEVICE_ID_LOGITECH_G27_WHEEL
};

static const struct lg4ff_wheel_ident_info lg4ff_dfgt_ident_info = {
	LG4FF_MODE_DFGT | LG4FF_MODE_DFP | LG4FF_MODE_DFEX,
	0xff00,
	0x1300,
	USB_DEVICE_ID_LOGITECH_DFGT_WHEEL
};

static const struct lg4ff_wheel_ident_info lg4ff_g29_ident_info = {
	LG4FF_MODE_G29 | LG4FF_MODE_G27 | LG4FF_MODE_G25 | LG4FF_MODE_DFGT | LG4FF_MODE_DFP | LG4FF_MODE_DFEX,
	0xfff8,
	0x1350,
	USB_DEVICE_ID_LOGITECH_G29_WHEEL
};

static const struct lg4ff_wheel_ident_info lg4ff_g29_ident_info2 = {
	LG4FF_MODE_G29 | LG4FF_MODE_G27 | LG4FF_MODE_G25 | LG4FF_MODE_DFGT | LG4FF_MODE_DFP | LG4FF_MODE_DFEX,
	0xff00,
	0x8900,
	USB_DEVICE_ID_LOGITECH_G29_WHEEL
};

/* Multimode wheel identification checklists */
static const struct lg4ff_wheel_ident_info *lg4ff_main_checklist[] = {
	&lg4ff_g29_ident_info,
	&lg4ff_g29_ident_info2,
	&lg4ff_dfgt_ident_info,
	&lg4ff_g27_ident_info,
	&lg4ff_g25_ident_info,
	&lg4ff_dfp_ident_info
};

/* Compatibility mode switching commands */
/* EXT_CMD9 - Understood by G27 and DFGT */
static const struct lg4ff_compat_mode_switch lg4ff_mode_switch_ext09_dfex = {
	2,
	{0xf8, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,	/* Revert mode upon USB reset */
	 0xf8, 0x09, 0x00, 0x01, 0x00, 0x00, 0x00}	/* Switch mode to DF-EX with detach */
};

static const struct lg4ff_compat_mode_switch lg4ff_mode_switch_ext09_dfp = {
	2,
	{0xf8, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,	/* Revert mode upon USB reset */
	 0xf8, 0x09, 0x01, 0x01, 0x00, 0x00, 0x00}	/* Switch mode to DFP with detach */
};

static const struct lg4ff_compat_mode_switch lg4ff_mode_switch_ext09_g25 = {
	2,
	{0xf8, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,	/* Revert mode upon USB reset */
	 0xf8, 0x09, 0x02, 0x01, 0x00, 0x00, 0x00}	/* Switch mode to G25 with detach */
};

static const struct lg4ff_compat_mode_switch lg4ff_mode_switch_ext09_dfgt = {
	2,
	{0xf8, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,	/* Revert mode upon USB reset */
	 0xf8, 0x09, 0x03, 0x01, 0x00, 0x00, 0x00}	/* Switch mode to DFGT with detach */
};

static const struct lg4ff_compat_mode_switch lg4ff_mode_switch_ext09_g27 = {
	2,
	{0xf8, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,	/* Revert mode upon USB reset */
	 0xf8, 0x09, 0x04, 0x01, 0x00, 0x00, 0x00}	/* Switch mode to G27 with detach */
};

static const struct lg4ff_compat_mode_switch lg4ff_mode_switch_ext09_g29 = {
	2,
	{0xf8, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,	/* Revert mode upon USB reset */
	 0xf8, 0x09, 0x05, 0x01, 0x01, 0x00, 0x00}	/* Switch mode to G29 with detach */
};

/* EXT_CMD1 - Understood by DFP, G25, G27 and DFGT */
static const struct lg4ff_compat_mode_switch lg4ff_mode_switch_ext01_dfp = {
	1,
	{0xf8, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}
};

/* EXT_CMD16 - Understood by G25 and G27 */
static const struct lg4ff_compat_mode_switch lg4ff_mode_switch_ext16_g25 = {
	1,
	{0xf8, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00}
};

static struct lg4ff_device_entry *lg4ff_get_device_entry(struct hid_device *hid)
{
	struct lg_drv_data *drv_data;
	struct lg4ff_device_entry *entry;

	drv_data = hid_get_drvdata(hid);
	if (!drv_data) {
		hid_err(hid, "Private driver data not found!\n");
		return NULL;
	}

	entry = drv_data->device_props;
	if (!entry) {
		hid_err(hid, "Device properties not found!\n");
		return NULL;
	}

	return entry;
}

void lg4ff_send_cmd(struct lg4ff_device_entry *entry, __u8 *cmd)
{
	unsigned long flags;
	s32 *value = entry->report->field[0]->value;

	spin_lock_irqsave(&entry->report_lock, flags);
	value[0] = cmd[0];
	value[1] = cmd[1];
	value[2] = cmd[2];
	value[3] = cmd[3];
	value[4] = cmd[4];
	value[5] = cmd[5];
	value[6] = cmd[6];
	hid_hw_request(entry->hid, entry->report, HID_REQ_SET_REPORT);
	spin_unlock_irqrestore(&entry->report_lock, flags);
	//DEBUG("send_cmd: %02X %02X %02X %02X %02X %02X %02X", cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], cmd[5], cmd[6]);
}

void lg4ff_update_slot(struct lg4ff_slot *slot, struct lg4ff_effect_parameters *parameters)
{
	__u8 original_cmd[7];
	int d1;
	int d2;
	int s1;
	int s2;

	memcpy(original_cmd, slot->current_cmd, sizeof(original_cmd));

	slot->current_cmd[0] = (0x10 << slot->id) + slot->cmd_op;
	switch (slot->effect_type) {
		case FF_CONSTANT:
			slot->current_cmd[1] = 0x00;
			slot->current_cmd[2 + slot->id] = TRANSLATE_FORCE(parameters->level);
			slot->current_cmd[3] = 0;
			slot->current_cmd[4] = 0;
			slot->current_cmd[5] = 0;
			slot->current_cmd[6] = 0;
			break;
		case FF_SPRING:
			d1 = SCALE_VALUE_U16(((parameters->d1 + 0x8000) & 0xffff), 11);
			d2 = SCALE_VALUE_U16(((parameters->d2 + 0x8000) & 0xffff), 11);
			s1 = parameters->k1 < 0;
			s2 = parameters->k2 < 0;
			slot->current_cmd[1] = 0x0b;
			slot->current_cmd[2] = d1 >> 3;
			slot->current_cmd[3] = d2 >> 3;
			slot->current_cmd[4] = (SCALE_VALUE_U16(abs(parameters->k2), 4) << 4) + SCALE_VALUE_U16(abs(parameters->k1), 4);
			slot->current_cmd[5] = ((d2 & 7) << 5) + ((d1 & 7) << 1) + (s2 << 4) + s1;
			slot->current_cmd[6] = SCALE_VALUE_U16(parameters->clip, 8);
			break;
		case FF_DAMPER:
			s1 = parameters->k1 < 0;
			s2 = parameters->k2 < 0;
			slot->current_cmd[1] = 0x0c;
			slot->current_cmd[2] = SCALE_VALUE_U16(abs(parameters->k1), 4);
			slot->current_cmd[3] = s1;
			slot->current_cmd[4] = SCALE_VALUE_U16(abs(parameters->k2), 4);
			slot->current_cmd[5] = s2;
			slot->current_cmd[6] = SCALE_VALUE_U16(parameters->clip, 8);
			break;
		case FF_FRICTION:
			s1 = parameters->k1 < 0;
			s2 = parameters->k2 < 0;
			slot->current_cmd[1] = 0x0e;
			slot->current_cmd[2] = SCALE_VALUE_U16(abs(parameters->k1) * 2, 8);
			slot->current_cmd[3] = SCALE_VALUE_U16(abs(parameters->k2) * 2, 8);
			slot->current_cmd[4] = SCALE_VALUE_U16(parameters->clip, 8);
			slot->current_cmd[5] = (s2 << 4) + s1;
			slot->current_cmd[6] = 0;
			break;
	}
	
	if (memcmp(original_cmd, slot->current_cmd, sizeof(original_cmd))) {
		slot->is_updated = 1;
	}
}

static __always_inline int lg4ff_calculate_constant(struct lg4ff_effect_state *state)
{
	int level = state->effect.u.constant.level;
	int level_sign;
	long d, t;

	if (state->time_playing < state->envelope->attack_length) {
		level_sign = level < 0 ? -1 : 1;
		d = level - level_sign * state->envelope->attack_level;
		level = level_sign * state->envelope->attack_level + d * state->time_playing / state->envelope->attack_length;
	} else if (state->effect.replay.length) {
		t = state->time_playing - state->effect.replay.length + state->envelope->fade_length;
		if (t > 0) {
			level_sign = level < 0 ? -1 : 1;
			d = level - level_sign * state->envelope->fade_level;
			level = level - d * t / state->envelope->fade_length;
		}
	}

	return state->direction_gain * level / 0x7fff;
}

static __always_inline int lg4ff_calculate_ramp(struct lg4ff_effect_state *state)
{
	struct ff_ramp_effect *ramp = &state->effect.u.ramp;
	int level = INT_MAX;
	int level_sign;
	long d, t;

	if (state->time_playing < state->envelope->attack_length) {
		level = ramp->start_level;
		level_sign =  level < 0 ? -1 : 1;
		t = state->envelope->attack_length - state->time_playing;
		d = level - level_sign * state->envelope->attack_level;
		level = level_sign * state->envelope->attack_level + d * t / state->envelope->attack_length;
	} else if (state->effect.replay.length && state->time_playing >= state->effect.replay.length - state->envelope->fade_length) {
		level = ramp->end_level;
		level_sign = level < 0 ? -1 : 1;
		t = state->time_playing - state->effect.replay.length + state->envelope->fade_length;
		d = level_sign * state->envelope->fade_level - level;
		level = level - d * t / state->envelope->fade_length;
	} else {
		t = state->time_playing - state->envelope->attack_length;
		level = ramp->start_level + ((t * state->slope) >> 16);
	}

	return state->direction_gain * level / 0x7fff;
}

static __always_inline int lg4ff_calculate_periodic(struct lg4ff_effect_state *state)
{
	struct ff_periodic_effect *periodic = &state->effect.u.periodic;
	int level = periodic->offset;
	int magnitude = periodic->magnitude;
	int magnitude_sign = magnitude < 0 ? -1 : 1;
	long d, t;

	if (state->time_playing < state->envelope->attack_length) {
		d = magnitude - magnitude_sign * state->envelope->attack_level;
		magnitude = magnitude_sign * state->envelope->attack_level + d * state->time_playing / state->envelope->attack_length;
	} else if (state->effect.replay.length) {
		t = state->time_playing - state->effect.replay.length + state->envelope->fade_length;
		if (t > 0) {
			d = magnitude - magnitude_sign * state->envelope->fade_level;
			magnitude = magnitude - d * t / state->envelope->fade_length;
		}
	}

	switch (periodic->waveform) {
		case FF_SINE:
			level += fixp_sin16(state->phase) * magnitude / 0x7fff;
			break;
		case FF_SQUARE:
			level += (state->phase < 180 ? 1 : -1) * magnitude;
			break;
		case FF_TRIANGLE:
			level += abs(state->phase * magnitude * 2 / 360 - magnitude) * 2 - magnitude;
			break;
		case FF_SAW_UP:
			level += state->phase * magnitude * 2 / 360 - magnitude;
			break;
		case FF_SAW_DOWN:
			level += magnitude - state->phase * magnitude * 2 / 360;
			break;
	}

	return state->direction_gain * level / 0x7fff;
}

static __always_inline void lg4ff_calculate_spring(struct lg4ff_effect_state *state, struct lg4ff_effect_parameters *parameters)
{
	struct ff_condition_effect *condition = &state->effect.u.condition[0];
	int d1;
	int d2;

	d1 = condition->center - condition->deadband / 2;
	d2 = condition->center + condition->deadband / 2;
	if (d1 < parameters->d1) {
		parameters->d1 = d1;
	}
	if (d2 > parameters->d2) {
		parameters->d2 = d2;
	}
	parameters->k1 = condition->left_coeff;
	parameters->k2 = condition->right_coeff;
	parameters->clip = condition->left_saturation;
}

static __always_inline void lg4ff_calculate_damper(struct lg4ff_effect_state *state, struct lg4ff_effect_parameters *parameters)
{
	struct ff_condition_effect *condition = &state->effect.u.condition[0];

	parameters->k1 = condition->left_coeff;
	parameters->k2 = condition->right_coeff;
	parameters->clip = condition->left_saturation;
}

static __always_inline void lg4ff_calculate_friction(struct lg4ff_effect_state *state, struct lg4ff_effect_parameters *parameters)
{
	struct ff_condition_effect *condition = &state->effect.u.condition[0];

	parameters->k1 = condition->left_coeff;
	parameters->k2 = condition->right_coeff;
	parameters->clip = condition->left_saturation;
}

static __always_inline struct ff_envelope *lg4ff_effect_envelope(struct ff_effect *effect)
{
	switch (effect->type) {
		case FF_CONSTANT:
			return &effect->u.constant.envelope;
		case FF_RAMP:
			return &effect->u.ramp.envelope;
		case FF_PERIODIC:
			return &effect->u.periodic.envelope;
	}

	return NULL;
}

static __always_inline void lg4ff_update_state(struct lg4ff_effect_state *state, const unsigned long now)
{
	struct ff_effect *effect = &state->effect;
	unsigned long phase_time;

	if (!__test_and_set_bit(FF_EFFECT_ALLSET, &state->flags)) {
		state->play_at = state->start_at + effect->replay.delay;
		if (!test_bit(FF_EFFECT_UPDATING, &state->flags)) {
			state->updated_at = state->play_at;
		}
		state->direction_gain = fixp_sin16(effect->direction * 360 / 0xffff);
		if (effect->type == FF_PERIODIC) {
			state->phase_adj = effect->u.periodic.phase * 360 / effect->u.periodic.period;
		}
		if (effect->replay.length) {
			state->stop_at = state->play_at + effect->replay.length;
		}
	}

	if (__test_and_clear_bit(FF_EFFECT_UPDATING, &state->flags)) {
		__clear_bit(FF_EFFECT_PLAYING, &state->flags);
		state->play_at = state->start_at + effect->replay.delay;
		state->direction_gain = fixp_sin16(effect->direction * 360 / 0xffff);
		if (effect->replay.length) {
			state->stop_at = state->play_at + effect->replay.length;
		}
		if (effect->type == FF_PERIODIC) {
			state->phase_adj = state->phase;
		}
	}

	state->envelope = lg4ff_effect_envelope(effect);

	state->slope = 0;
	if (effect->type == FF_RAMP && effect->replay.length) {
		state->slope = ((effect->u.ramp.end_level - effect->u.ramp.start_level) << 16) / (effect->replay.length - state->envelope->attack_length - state->envelope->fade_length);
	}

	if (!test_bit(FF_EFFECT_PLAYING, &state->flags) && time_after_eq(now,
				state->play_at) && (effect->replay.length == 0 ||
					time_before(now, state->stop_at))) {
		__set_bit(FF_EFFECT_PLAYING, &state->flags);
	}

	if (test_bit(FF_EFFECT_PLAYING, &state->flags)) {
		state->time_playing = time_diff(now, state->play_at);
		if (effect->type == FF_PERIODIC) {
			phase_time = time_diff(now, state->updated_at);
			state->phase = (phase_time % effect->u.periodic.period) * 360 / effect->u.periodic.period;
			state->phase += state->phase_adj % 360;
		}
	}
}

static void lg4ff_timer(struct timer_list *t)
{
	struct lg4ff_device_entry *entry = from_timer(entry, t, timer);
	struct lg4ff_slot *slot;
	struct lg4ff_effect_state *state;
	struct lg4ff_effect_parameters parameters[4];
	struct timespec t0, t1;
	unsigned long handler_time;
	unsigned long now = jiffies_to_msecs(jiffies);
	unsigned long flags;
	int count;
	int effect_id;
	int i;

	getrawmonotonic(&t0);

	memset(parameters, 0, sizeof(parameters));

	spin_lock_irqsave(&entry->timer_lock, flags);

	count = entry->effects_used;

	for (effect_id = 0; effect_id < LG4FF_MAX_EFFECTS; effect_id++) {

		if (!count) {
			break;
		}

		state = &entry->states[effect_id];

		if (!test_bit(FF_EFFECT_STARTED, &state->flags)) {
			continue;
		}

		count--;

		if (test_bit(FF_EFFECT_ALLSET, &state->flags)) {
			if (state->effect.replay.length && time_after_eq(now, state->stop_at)) {
				STOP_EFFECT(state);
				if (!--state->count) {
					entry->effects_used--;
					continue;
				}
				__set_bit(FF_EFFECT_STARTED, &state->flags);
				state->start_at = state->stop_at;
			}
		}

		lg4ff_update_state(state, now);

		if (!test_bit(FF_EFFECT_PLAYING, &state->flags)) {
			continue;
		}

		switch (state->effect.type) {
			case FF_CONSTANT:
				parameters[0].level += lg4ff_calculate_constant(state);
				break;
			case FF_RAMP:
				parameters[0].level += lg4ff_calculate_ramp(state);
				break;
			case FF_PERIODIC:
				parameters[0].level += lg4ff_calculate_periodic(state);
				break;
			case FF_SPRING:
				lg4ff_calculate_spring(state, &parameters[1]);
				break;
			case FF_DAMPER:
				lg4ff_calculate_damper(state, &parameters[2]);
				break;
			case FF_FRICTION:
				lg4ff_calculate_friction(state, &parameters[3]);
				break;
		}
	}

	parameters[0].level = (long)parameters[0].level * entry->gain / 0xffff;

	spin_unlock_irqrestore(&entry->timer_lock, flags);

	for (i = 0; i < 4; i++) {
		slot = &entry->slots[i];
		lg4ff_update_slot(slot, &parameters[i]);
		if (slot->is_updated) {
			lg4ff_send_cmd(entry, slot->current_cmd);
			slot->is_updated = 0;
		}
	}

	getrawmonotonic(&t1);
	handler_time = (t1.tv_nsec > t0.tv_nsec ? 0 : 1000000000) + t1.tv_nsec - t0.tv_nsec;
	if (handler_time > timer_msecs * 1000000 / 2) {
		DEBUG("Timer function slow: %lu", handler_time);
	}

	if (entry->effects_used < 0) {
		DEBUG("Error: effects_used = %d", entry->effects_used);
	}

	if (entry->effects_used) {
		mod_timer(&entry->timer, msecs_to_jiffies(now + timer_msecs));
	} else {
		DEBUG("Stop timer.");
		del_timer(&entry->timer);
	}
}

static void lg4ff_init_slots(struct lg4ff_device_entry *entry, struct ff_device *ff)
{
	struct lg4ff_effect_parameters parameters;
	__u8 cmd[8] = {0};
	int i;

	// Set/unset fixed loop mode
	cmd[0] = 0x0d;
	cmd[1] = fixed_loop ? 1 : 0;
	lg4ff_send_cmd(entry, cmd);

	memset(&entry->states, 0, sizeof(entry->states));
	memset(&entry->slots, 0, sizeof(entry->slots));
	memset(&parameters, 0, sizeof(parameters));

	entry->slots[0].effect_type = FF_CONSTANT;
	entry->slots[1].effect_type = FF_SPRING;
	entry->slots[2].effect_type = FF_DAMPER;
	entry->slots[3].effect_type = FF_FRICTION;

	for (i = 0; i < 4; i++) {
		entry->slots[i].id = i;
		entry->slots[i].cmd_op = 0x01;
		lg4ff_update_slot(&entry->slots[i], &parameters);
		lg4ff_send_cmd(entry, entry->slots[i].current_cmd);
		entry->slots[i].cmd_op = 0x0c;
		entry->slots[i].is_updated = 0;
	}

	entry->effects_used = 0;

	spin_lock_init(&entry->timer_lock);

	timer_setup(&entry->timer, lg4ff_timer, 0);
}

static int lg4ff_upload_effect(struct input_dev *dev, struct ff_effect *effect, struct ff_effect *old)
{
	struct hid_device *hid = input_get_drvdata(dev);
	struct lg4ff_device_entry *entry;
	struct lg4ff_effect_state *state;
	unsigned long now = jiffies_to_msecs(jiffies);
	unsigned long flags;

	entry = lg4ff_get_device_entry(hid);
	if (entry == NULL) {
		return -EINVAL;
	}

	if (effect->type == FF_PERIODIC && effect->u.periodic.period == 0) {
		return -EINVAL;
	}

	state = &entry->states[effect->id];

	if (test_bit(FF_EFFECT_STARTED, &state->flags) && effect->type != state->effect.type) {
		return -EINVAL;
	}

	spin_lock_irqsave(&entry->timer_lock, flags);

	state->effect = *effect;

	if (test_bit(FF_EFFECT_STARTED, &state->flags)) {
		__set_bit(FF_EFFECT_UPDATING, &state->flags);
		state->updated_at = now;
	}

	spin_unlock_irqrestore(&entry->timer_lock, flags);

	return 0;
}

static int lg4ff_play_effect(struct input_dev *dev, int effect_id, int value)
{
	struct hid_device *hid = input_get_drvdata(dev);
	struct lg4ff_device_entry *entry;
	struct lg4ff_effect_state *state;
	unsigned long now = jiffies_to_msecs(jiffies);
	unsigned long flags;

	entry = lg4ff_get_device_entry(hid);
	if (entry == NULL) {
		return -EINVAL;
	}

	state = &entry->states[effect_id];

	spin_lock_irqsave(&entry->timer_lock, flags);

	if (value > 0) {
		if (test_bit(FF_EFFECT_STARTED, &state->flags)) {
			STOP_EFFECT(state);
		} else {
			entry->effects_used++;
			if (!timer_pending(&entry->timer)) {
				DEBUG("Start timer.");
				mod_timer(&entry->timer, jiffies + msecs_to_jiffies(timer_msecs));
			}
		}
		__set_bit(FF_EFFECT_STARTED, &state->flags);
		state->start_at = now;
		state->count = value;
	} else {
		if (test_bit(FF_EFFECT_STARTED, &state->flags)) {
			STOP_EFFECT(state);
			entry->effects_used--;
		}
	}

	spin_unlock_irqrestore(&entry->timer_lock, flags);

	return 0;
}

/* Recalculates X axis value accordingly to currently selected range */
static s32 lg4ff_adjust_dfp_x_axis(s32 value, u16 range)
{
	u16 max_range;
	s32 new_value;

	if (range == 900)
		return value;
	else if (range == 200)
		return value;
	else if (range < 200)
		max_range = 200;
	else
		max_range = 900;

	new_value = 8192 + mult_frac(value - 8192, max_range, range);
	if (new_value < 0)
		return 0;
	else if (new_value > 16383)
		return 16383;
	else
		return new_value;
}

int lg4ff_adjust_input_event(struct hid_device *hid, struct hid_field *field,
			     struct hid_usage *usage, s32 value, struct lg_drv_data *drv_data)
{
	struct lg4ff_device_entry *entry = drv_data->device_props;
	s32 new_value = 0;

	if (!entry) {
		hid_err(hid, "Device properties not found");
		return 0;
	}

	switch (entry->wdata.product_id) {
	case USB_DEVICE_ID_LOGITECH_DFP_WHEEL:
		switch (usage->code) {
		case ABS_X:
			new_value = lg4ff_adjust_dfp_x_axis(value, entry->wdata.range);
			input_event(field->hidinput->input, usage->type, usage->code, new_value);
			return 1;
		default:
			return 0;
		}
	default:
		return 0;
	}
}

int lg4ff_raw_event(struct hid_device *hdev, struct hid_report *report,
		u8 *rd, int size, struct lg_drv_data *drv_data)
{
	int offset;
	struct lg4ff_device_entry *entry = drv_data->device_props;

	if (!entry)
		return 0;

	/* adjust HID report present combined pedals data */
	if (entry->wdata.combine == 1) {
		switch (entry->wdata.product_id) {
		case USB_DEVICE_ID_LOGITECH_WHEEL:
			rd[5] = rd[3];
			rd[6] = 0x7F;
			return 1;
		case USB_DEVICE_ID_LOGITECH_WINGMAN_FG:
		case USB_DEVICE_ID_LOGITECH_WINGMAN_FFG:
		case USB_DEVICE_ID_LOGITECH_MOMO_WHEEL:
		case USB_DEVICE_ID_LOGITECH_MOMO_WHEEL2:
			rd[4] = rd[3];
			rd[5] = 0x7F;
			return 1;
		case USB_DEVICE_ID_LOGITECH_DFP_WHEEL:
			rd[5] = rd[4];
			rd[6] = 0x7F;
			return 1;
		case USB_DEVICE_ID_LOGITECH_G25_WHEEL:
		case USB_DEVICE_ID_LOGITECH_G27_WHEEL:
			offset = 5;
			break;
		case USB_DEVICE_ID_LOGITECH_DFGT_WHEEL:
		case USB_DEVICE_ID_LOGITECH_G29_WHEEL:
			offset = 6;
			break;
		case USB_DEVICE_ID_LOGITECH_WII_WHEEL:
			offset = 3;
			break;
		default:
			return 0;
		}

		/* Compute a combined axis when wheel does not supply it */
		rd[offset] = (0xFF + rd[offset] - rd[offset+1]) >> 1;
		rd[offset+1] = 0x7F;
		return 1;
	}

	if (entry->wdata.combine == 2) {
		switch (entry->wdata.product_id) {
			case USB_DEVICE_ID_LOGITECH_G25_WHEEL:
			case USB_DEVICE_ID_LOGITECH_G27_WHEEL:
				offset = 5;
				break;
			case USB_DEVICE_ID_LOGITECH_G29_WHEEL:
				offset = 6;
				break;
			default:
				return 0;
		}

		/* Compute a combined axis when wheel does not supply it */
		rd[offset] = (0xFF + rd[offset] - rd[offset+2]) >> 1;
		rd[offset+2] = 0x7F;
		return 1;
	}

	return 0;
}

static void lg4ff_init_wheel_data(struct lg4ff_wheel_data * const wdata, const struct lg4ff_wheel *wheel,
				  const struct lg4ff_multimode_wheel *mmode_wheel,
				  const u16 real_product_id)
{
	u32 alternate_modes = 0;
	const char *real_tag = NULL;
	const char *real_name = NULL;

	if (mmode_wheel) {
		alternate_modes = mmode_wheel->alternate_modes;
		real_tag = mmode_wheel->real_tag;
		real_name = mmode_wheel->real_name;
	}

	{
		struct lg4ff_wheel_data t_wdata =  { .product_id = wheel->product_id,
						     .real_product_id = real_product_id,
						     .combine = 0,
						     .min_range = wheel->min_range,
						     .max_range = wheel->max_range,
						     .set_range = wheel->set_range,
						     .alternate_modes = alternate_modes,
						     .real_tag = real_tag,
						     .real_name = real_name };

		memcpy(wdata, &t_wdata, sizeof(t_wdata));
	}
}

/* Sends default autocentering command compatible with
 * all wheels except Formula Force EX */
static void lg4ff_set_autocenter_default(struct input_dev *dev, u16 magnitude)
{
	struct hid_device *hid = input_get_drvdata(dev);
	s32 *value;
	u32 expand_a, expand_b;
	struct lg4ff_device_entry *entry;
	unsigned long flags;

	entry = lg4ff_get_device_entry(hid);
	if (entry == NULL) {
		return;
	}

	value = entry->report->field[0]->value;

	/* De-activate Auto-Center */
	spin_lock_irqsave(&entry->report_lock, flags);
	if (magnitude == 0) {
		value[0] = 0xf5;
		value[1] = 0x00;
		value[2] = 0x00;
		value[3] = 0x00;
		value[4] = 0x00;
		value[5] = 0x00;
		value[6] = 0x00;

		hid_hw_request(hid, entry->report, HID_REQ_SET_REPORT);
		spin_unlock_irqrestore(&entry->report_lock, flags);
		return;
	}

	if (magnitude <= 0xaaaa) {
		expand_a = 0x0c * magnitude;
		expand_b = 0x80 * magnitude;
	} else {
		expand_a = (0x0c * 0xaaaa) + 0x06 * (magnitude - 0xaaaa);
		expand_b = (0x80 * 0xaaaa) + 0xff * (magnitude - 0xaaaa);
	}

	/* Adjust for non-MOMO wheels */
	switch (entry->wdata.product_id) {
	case USB_DEVICE_ID_LOGITECH_MOMO_WHEEL:
	case USB_DEVICE_ID_LOGITECH_MOMO_WHEEL2:
		break;
	default:
		expand_a = expand_a >> 1;
		break;
	}

	value[0] = 0xfe;
	value[1] = 0x0d;
	value[2] = expand_a / 0xaaaa;
	value[3] = expand_a / 0xaaaa;
	value[4] = expand_b / 0xaaaa;
	value[5] = 0x00;
	value[6] = 0x00;

	hid_hw_request(hid, entry->report, HID_REQ_SET_REPORT);

	/* Activate Auto-Center */
	value[0] = 0x14;
	value[1] = 0x00;
	value[2] = 0x00;
	value[3] = 0x00;
	value[4] = 0x00;
	value[5] = 0x00;
	value[6] = 0x00;

	hid_hw_request(hid, entry->report, HID_REQ_SET_REPORT);
	spin_unlock_irqrestore(&entry->report_lock, flags);
}

/* Sends autocentering command compatible with Formula Force EX */
static void lg4ff_set_autocenter_ffex(struct input_dev *dev, u16 magnitude)
{
	struct hid_device *hid = input_get_drvdata(dev);
	struct lg4ff_device_entry *entry;
	unsigned long flags;
	s32 *value;

	entry = lg4ff_get_device_entry(hid);
	if (entry == NULL) {
		return;
	}

	value = entry->report->field[0]->value;

	magnitude = magnitude * 90 / 65535;

	spin_lock_irqsave(&entry->report_lock, flags);
	value[0] = 0xfe;
	value[1] = 0x03;
	value[2] = magnitude >> 14;
	value[3] = magnitude >> 14;
	value[4] = magnitude;
	value[5] = 0x00;
	value[6] = 0x00;

	hid_hw_request(hid, entry->report, HID_REQ_SET_REPORT);
	spin_unlock_irqrestore(&entry->report_lock, flags);
}

/* Sends command to set range compatible with G25/G27/Driving Force GT */
static void lg4ff_set_range_g25(struct hid_device *hid, u16 range)
{
	struct lg4ff_device_entry *entry;
	struct lg_drv_data *drv_data;
	unsigned long flags;
	s32 *value;

	drv_data = hid_get_drvdata(hid);
	entry = drv_data->device_props;
	value = entry->report->field[0]->value;

	dbg_hid("G25/G27/DFGT: setting range to %u\n", range);

	spin_lock_irqsave(&entry->report_lock, flags);
	value[0] = 0xf8;
	value[1] = 0x81;
	value[2] = range & 0x00ff;
	value[3] = (range & 0xff00) >> 8;
	value[4] = 0x00;
	value[5] = 0x00;
	value[6] = 0x00;

	hid_hw_request(hid, entry->report, HID_REQ_SET_REPORT);
	spin_unlock_irqrestore(&entry->report_lock, flags);
}

/* Sends commands to set range compatible with Driving Force Pro wheel */
static void lg4ff_set_range_dfp(struct hid_device *hid, u16 range)
{
	struct lg4ff_device_entry *entry;
	struct lg_drv_data *drv_data;
	unsigned long flags;
	int start_left, start_right, full_range;
	s32 *value;

	drv_data = hid_get_drvdata(hid);
	entry = drv_data->device_props;
	value = entry->report->field[0]->value;

	dbg_hid("Driving Force Pro: setting range to %u\n", range);

	/* Prepare "coarse" limit command */
	spin_lock_irqsave(&entry->report_lock, flags);
	value[0] = 0xf8;
	value[1] = 0x00;	/* Set later */
	value[2] = 0x00;
	value[3] = 0x00;
	value[4] = 0x00;
	value[5] = 0x00;
	value[6] = 0x00;

	if (range > 200) {
		value[1] = 0x03;
		full_range = 900;
	} else {
		value[1] = 0x02;
		full_range = 200;
	}
	hid_hw_request(hid, entry->report, HID_REQ_SET_REPORT);

	/* Prepare "fine" limit command */
	value[0] = 0x81;
	value[1] = 0x0b;
	value[2] = 0x00;
	value[3] = 0x00;
	value[4] = 0x00;
	value[5] = 0x00;
	value[6] = 0x00;

	if (range == 200 || range == 900) {	/* Do not apply any fine limit */
		hid_hw_request(hid, entry->report, HID_REQ_SET_REPORT);
		spin_unlock_irqrestore(&entry->report_lock, flags);
		return;
	}

	/* Construct fine limit command */
	start_left = (((full_range - range + 1) * 2047) / full_range);
	start_right = 0xfff - start_left;

	value[2] = start_left >> 4;
	value[3] = start_right >> 4;
	value[4] = 0xff;
	value[5] = (start_right & 0xe) << 4 | (start_left & 0xe);
	value[6] = 0xff;

	hid_hw_request(hid, entry->report, HID_REQ_SET_REPORT);
	spin_unlock_irqrestore(&entry->report_lock, flags);
}

static void lg4ff_set_gain(struct input_dev *dev, u16 gain)
{
	struct hid_device *hid = input_get_drvdata(dev);
	struct lg4ff_device_entry *entry;
	unsigned long flags;

	entry = lg4ff_get_device_entry(hid);
	if (entry == NULL) {
		return;
	}

	spin_lock_irqsave(&entry->timer_lock, flags);

	entry->gain = gain;

	spin_unlock_irqrestore(&entry->timer_lock, flags);
}

static const struct lg4ff_compat_mode_switch *lg4ff_get_mode_switch_command(const u16 real_product_id, const u16 target_product_id)
{
	switch (real_product_id) {
	case USB_DEVICE_ID_LOGITECH_DFP_WHEEL:
		switch (target_product_id) {
		case USB_DEVICE_ID_LOGITECH_DFP_WHEEL:
			return &lg4ff_mode_switch_ext01_dfp;
		/* DFP can only be switched to its native mode */
		default:
			return NULL;
		}
		break;
	case USB_DEVICE_ID_LOGITECH_G25_WHEEL:
		switch (target_product_id) {
		case USB_DEVICE_ID_LOGITECH_DFP_WHEEL:
			return &lg4ff_mode_switch_ext01_dfp;
		case USB_DEVICE_ID_LOGITECH_G25_WHEEL:
			return &lg4ff_mode_switch_ext16_g25;
		/* G25 can only be switched to DFP mode or its native mode */
		default:
			return NULL;
		}
		break;
	case USB_DEVICE_ID_LOGITECH_G27_WHEEL:
		switch (target_product_id) {
		case USB_DEVICE_ID_LOGITECH_WHEEL:
			return &lg4ff_mode_switch_ext09_dfex;
		case USB_DEVICE_ID_LOGITECH_DFP_WHEEL:
			return &lg4ff_mode_switch_ext09_dfp;
		case USB_DEVICE_ID_LOGITECH_G25_WHEEL:
			return &lg4ff_mode_switch_ext09_g25;
		case USB_DEVICE_ID_LOGITECH_G27_WHEEL:
			return &lg4ff_mode_switch_ext09_g27;
		/* G27 can only be switched to DF-EX, DFP, G25 or its native mode */
		default:
			return NULL;
		}
		break;
	case USB_DEVICE_ID_LOGITECH_G29_WHEEL:
		switch (target_product_id) {
		case USB_DEVICE_ID_LOGITECH_DFP_WHEEL:
			return &lg4ff_mode_switch_ext09_dfp;
		case USB_DEVICE_ID_LOGITECH_DFGT_WHEEL:
			return &lg4ff_mode_switch_ext09_dfgt;
		case USB_DEVICE_ID_LOGITECH_G25_WHEEL:
			return &lg4ff_mode_switch_ext09_g25;
		case USB_DEVICE_ID_LOGITECH_G27_WHEEL:
			return &lg4ff_mode_switch_ext09_g27;
		case USB_DEVICE_ID_LOGITECH_G29_WHEEL:
			return &lg4ff_mode_switch_ext09_g29;
		/* G29 can only be switched to DF-EX, DFP, DFGT, G25, G27 or its native mode */
		default:
			return NULL;
		}
		break;
	case USB_DEVICE_ID_LOGITECH_DFGT_WHEEL:
		switch (target_product_id) {
		case USB_DEVICE_ID_LOGITECH_WHEEL:
			return &lg4ff_mode_switch_ext09_dfex;
		case USB_DEVICE_ID_LOGITECH_DFP_WHEEL:
			return &lg4ff_mode_switch_ext09_dfp;
		case USB_DEVICE_ID_LOGITECH_DFGT_WHEEL:
			return &lg4ff_mode_switch_ext09_dfgt;
		/* DFGT can only be switched to DF-EX, DFP or its native mode */
		default:
			return NULL;
		}
		break;
	/* No other wheels have multiple modes */
	default:
		return NULL;
	}
}

static int lg4ff_switch_compatibility_mode(struct hid_device *hid, const struct lg4ff_compat_mode_switch *s)
{
	struct lg4ff_device_entry *entry;
	unsigned long flags;
	s32 *value;
	u8 i;

	entry = lg4ff_get_device_entry(hid);
	if (entry == NULL) {
		return -EINVAL;
	}
	value = entry->report->field[0]->value;

	spin_lock_irqsave(&entry->report_lock, flags);
	for (i = 0; i < s->cmd_count; i++) {
		u8 j;

		for (j = 0; j < 7; j++)
			value[j] = s->cmd[j + (7*i)];

		hid_hw_request(hid, entry->report, HID_REQ_SET_REPORT);
	}
	spin_unlock_irqrestore(&entry->report_lock, flags);
	hid_hw_wait(hid);
	return 0;
}

static ssize_t lg4ff_alternate_modes_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hid_device *hid = to_hid_device(dev);
	struct lg4ff_device_entry *entry;
	ssize_t count = 0;
	int i;

	entry = lg4ff_get_device_entry(hid);
	if (entry == NULL) {
		return -EINVAL;
	}

	if (!entry->wdata.real_name) {
		hid_err(hid, "NULL pointer to string\n");
		return 0;
	}

	for (i = 0; i < LG4FF_MODE_MAX_IDX; i++) {
		if (entry->wdata.alternate_modes & BIT(i)) {
			/* Print tag and full name */
			count += scnprintf(buf + count, PAGE_SIZE - count, "%s: %s",
					   lg4ff_alternate_modes[i].tag,
					   !lg4ff_alternate_modes[i].product_id ? entry->wdata.real_name : lg4ff_alternate_modes[i].name);
			if (count >= PAGE_SIZE - 1)
				return count;

			/* Mark the currently active mode with an asterisk */
			if (lg4ff_alternate_modes[i].product_id == entry->wdata.product_id ||
			    (lg4ff_alternate_modes[i].product_id == 0 && entry->wdata.product_id == entry->wdata.real_product_id))
				count += scnprintf(buf + count, PAGE_SIZE - count, " *\n");
			else
				count += scnprintf(buf + count, PAGE_SIZE - count, "\n");

			if (count >= PAGE_SIZE - 1)
				return count;
		}
	}

	return count;
}

static ssize_t lg4ff_alternate_modes_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct hid_device *hid = to_hid_device(dev);
	struct lg4ff_device_entry *entry;
	const struct lg4ff_compat_mode_switch *s;
	u16 target_product_id = 0;
	int i, ret;
	char *lbuf;

	entry = lg4ff_get_device_entry(hid);
	if (entry == NULL) {
		return -EINVAL;
	}

	/* Allow \n at the end of the input parameter */
	lbuf = kasprintf(GFP_KERNEL, "%s", buf);
	if (!lbuf)
		return -ENOMEM;

	i = strlen(lbuf);
	if (lbuf[i-1] == '\n') {
		if (i == 1) {
			kfree(lbuf);
			return -EINVAL;
		}
		lbuf[i-1] = '\0';
	}

	for (i = 0; i < LG4FF_MODE_MAX_IDX; i++) {
		const u16 mode_product_id = lg4ff_alternate_modes[i].product_id;
		const char *tag = lg4ff_alternate_modes[i].tag;

		if (entry->wdata.alternate_modes & BIT(i)) {
			if (!strcmp(tag, lbuf)) {
				if (!mode_product_id)
					target_product_id = entry->wdata.real_product_id;
				else
					target_product_id = mode_product_id;
				break;
			}
		}
	}

	if (i == LG4FF_MODE_MAX_IDX) {
		hid_info(hid, "Requested mode \"%s\" is not supported by the device\n", lbuf);
		kfree(lbuf);
		return -EINVAL;
	}
	kfree(lbuf); /* Not needed anymore */

	if (target_product_id == entry->wdata.product_id) /* Nothing to do */
		return count;

	/* Automatic switching has to be disabled for the switch to DF-EX mode to work correctly */
	if (target_product_id == USB_DEVICE_ID_LOGITECH_WHEEL && !lg4ff_no_autoswitch) {
		hid_info(hid, "\"%s\" cannot be switched to \"DF-EX\" mode. Load the \"hid_logitech\" module with \"lg4ff_no_autoswitch=1\" parameter set and try again\n",
			 entry->wdata.real_name);
		return -EINVAL;
	}

	/* Take care of hardware limitations */
	if ((entry->wdata.real_product_id == USB_DEVICE_ID_LOGITECH_DFP_WHEEL || entry->wdata.real_product_id == USB_DEVICE_ID_LOGITECH_G25_WHEEL) &&
	    entry->wdata.product_id > target_product_id) {
		hid_info(hid, "\"%s\" cannot be switched back into \"%s\" mode\n", entry->wdata.real_name, lg4ff_alternate_modes[i].name);
		return -EINVAL;
	}

	s = lg4ff_get_mode_switch_command(entry->wdata.real_product_id, target_product_id);
	if (!s) {
		hid_err(hid, "Invalid target product ID %X\n", target_product_id);
		return -EINVAL;
	}

	ret = lg4ff_switch_compatibility_mode(hid, s);
	return (ret == 0 ? count : ret);
}
static DEVICE_ATTR(alternate_modes, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH, lg4ff_alternate_modes_show, lg4ff_alternate_modes_store);

static ssize_t lg4ff_combine_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct hid_device *hid = to_hid_device(dev);
	struct lg4ff_device_entry *entry;
	size_t count;

	entry = lg4ff_get_device_entry(hid);
	if (entry == NULL) {
		return -EINVAL;
	}

	count = scnprintf(buf, PAGE_SIZE, "%u\n", entry->wdata.combine);
	return count;
}

static ssize_t lg4ff_combine_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct hid_device *hid = to_hid_device(dev);
	struct lg4ff_device_entry *entry;
	u16 combine = simple_strtoul(buf, NULL, 10);

	entry = lg4ff_get_device_entry(hid);
	if (entry == NULL) {
		return -EINVAL;
	}

	if (combine > 2)
		combine = 2;

	entry->wdata.combine = combine;
	return count;
}
static DEVICE_ATTR(combine_pedals, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH, lg4ff_combine_show, lg4ff_combine_store);

/* Export the currently set range of the wheel */
static ssize_t lg4ff_range_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct hid_device *hid = to_hid_device(dev);
	struct lg4ff_device_entry *entry;
	size_t count;

	entry = lg4ff_get_device_entry(hid);
	if (entry == NULL) {
		return -EINVAL;
	}

	count = scnprintf(buf, PAGE_SIZE, "%u\n", entry->wdata.range);
	return count;
}

/* Set range to user specified value, call appropriate function
 * according to the type of the wheel */
static ssize_t lg4ff_range_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct hid_device *hid = to_hid_device(dev);
	struct lg4ff_device_entry *entry;
	u16 range = simple_strtoul(buf, NULL, 10);

	entry = lg4ff_get_device_entry(hid);
	if (entry == NULL) {
		return -EINVAL;
	}

	if (range == 0)
		range = entry->wdata.max_range;

	/* Check if the wheel supports range setting
	 * and that the range is within limits for the wheel */
	if (entry->wdata.set_range && range >= entry->wdata.min_range && range <= entry->wdata.max_range) {
		entry->wdata.set_range(hid, range);
		entry->wdata.range = range;
	}

	return count;
}
static DEVICE_ATTR(range, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH, lg4ff_range_show, lg4ff_range_store);

static ssize_t lg4ff_real_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hid_device *hid = to_hid_device(dev);
	struct lg4ff_device_entry *entry;
	size_t count;

	entry = lg4ff_get_device_entry(hid);
	if (entry == NULL) {
		return -EINVAL;
	}

	if (!entry->wdata.real_tag || !entry->wdata.real_name) {
		hid_err(hid, "NULL pointer to string\n");
		return 0;
	}

	count = scnprintf(buf, PAGE_SIZE, "%s: %s\n", entry->wdata.real_tag, entry->wdata.real_name);
	return count;
}

static ssize_t lg4ff_real_id_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	/* Real ID is a read-only value */
	return -EPERM;
}
static DEVICE_ATTR(real_id, S_IRUGO, lg4ff_real_id_show, lg4ff_real_id_store);

#ifdef CONFIG_LEDS_CLASS
static void lg4ff_set_leds(struct hid_device *hid, u8 leds)
{
	struct lg4ff_device_entry *entry;
	unsigned long flags;
	s32 *value;

	entry = lg4ff_get_device_entry(hid);
	if (entry == NULL) {
		return;
	}

	value = entry->report->field[0]->value;

	spin_lock_irqsave(&entry->report_lock, flags);
	value[0] = 0xf8;
	value[1] = 0x12;
	value[2] = leds;
	value[3] = 0x00;
	value[4] = 0x00;
	value[5] = 0x00;
	value[6] = 0x00;
	hid_hw_request(hid, entry->report, HID_REQ_SET_REPORT);
	spin_unlock_irqrestore(&entry->report_lock, flags);
}

static void lg4ff_led_set_brightness(struct led_classdev *led_cdev,
			enum led_brightness value)
{
	struct device *dev = led_cdev->dev->parent;
	struct hid_device *hid = to_hid_device(dev);
	struct lg4ff_device_entry *entry;
	int i, state = 0;

	entry = lg4ff_get_device_entry(hid);
	if (entry == NULL) {
		return;
	}

	for (i = 0; i < 5; i++) {
		if (led_cdev != entry->wdata.led[i])
			continue;
		state = (entry->wdata.led_state >> i) & 1;
		if (value == LED_OFF && state) {
			entry->wdata.led_state &= ~(1 << i);
			lg4ff_set_leds(hid, entry->wdata.led_state);
		} else if (value != LED_OFF && !state) {
			entry->wdata.led_state |= 1 << i;
			lg4ff_set_leds(hid, entry->wdata.led_state);
		}
		break;
	}
}

static enum led_brightness lg4ff_led_get_brightness(struct led_classdev *led_cdev)
{
	struct device *dev = led_cdev->dev->parent;
	struct hid_device *hid = to_hid_device(dev);
	struct lg4ff_device_entry *entry;
	int i, value = 0;

	entry = lg4ff_get_device_entry(hid);
	if (entry == NULL) {
		return -EINVAL;
	}

	for (i = 0; i < 5; i++)
		if (led_cdev == entry->wdata.led[i]) {
			value = (entry->wdata.led_state >> i) & 1;
			break;
		}

	return value ? LED_FULL : LED_OFF;
}

static void lg4ff_init_leds(struct hid_device *hid, struct lg4ff_device_entry *entry, int i)
{
	int error, j;

	/* register led subsystem - G27/G29 only */
	entry->wdata.led_state = 0;
	for (j = 0; j < 5; j++)
		entry->wdata.led[j] = NULL;

	if (lg4ff_devices[i].product_id == USB_DEVICE_ID_LOGITECH_G27_WHEEL ||
			lg4ff_devices[i].product_id == USB_DEVICE_ID_LOGITECH_G29_WHEEL) {
		struct led_classdev *led;
		size_t name_sz;
		char *name;

		lg4ff_set_leds(hid, 0);

		name_sz = strlen(dev_name(&hid->dev)) + 8;

		for (j = 0; j < 5; j++) {
			led = kzalloc(sizeof(struct led_classdev)+name_sz, GFP_KERNEL);
			if (!led) {
				hid_err(hid, "can't allocate memory for LED %d\n", j);
				goto err_leds;
			}

			name = (void *)(&led[1]);
			snprintf(name, name_sz, "%s::RPM%d", dev_name(&hid->dev), j+1);
			led->name = name;
			led->brightness = 0;
			led->max_brightness = 1;
			led->brightness_get = lg4ff_led_get_brightness;
			led->brightness_set = lg4ff_led_set_brightness;

			entry->wdata.led[j] = led;
			error = led_classdev_register(&hid->dev, led);

			if (error) {
				hid_err(hid, "failed to register LED %d. Aborting.\n", j);
err_leds:
				/* Deregister LEDs (if any) */
				for (j = 0; j < 5; j++) {
					led = entry->wdata.led[j];
					entry->wdata.led[j] = NULL;
					if (!led)
						continue;
					led_classdev_unregister(led);
					kfree(led);
				}
				goto out;	/* Let the driver continue without LEDs */
			}
		}
	}
out:
	return;
}
#endif

static u16 lg4ff_identify_multimode_wheel(struct hid_device *hid, const u16 reported_product_id, const u16 bcdDevice)
{
	u32 current_mode;
	int i;

	/* identify current mode from USB PID */
	for (i = 1; i < ARRAY_SIZE(lg4ff_alternate_modes); i++) {
		dbg_hid("Testing whether PID is %X\n", lg4ff_alternate_modes[i].product_id);
		if (reported_product_id == lg4ff_alternate_modes[i].product_id)
			break;
	}

	if (i == ARRAY_SIZE(lg4ff_alternate_modes))
		return 0;

	current_mode = BIT(i);

	for (i = 0; i < ARRAY_SIZE(lg4ff_main_checklist); i++) {
		const u16 mask = lg4ff_main_checklist[i]->mask;
		const u16 result = lg4ff_main_checklist[i]->result;
		const u16 real_product_id = lg4ff_main_checklist[i]->real_product_id;

		if ((current_mode & lg4ff_main_checklist[i]->modes) && \
				(bcdDevice & mask) == result) {
			dbg_hid("Found wheel with real PID %X whose reported PID is %X\n", real_product_id, reported_product_id);
			return real_product_id;
		}
	}

	/* No match found. This is either Driving Force or an unknown
	 * wheel model, do not touch it */
	dbg_hid("Wheel with bcdDevice %X was not recognized as multimode wheel, leaving in its current mode\n", bcdDevice);
	return 0;
}

static int lg4ff_handle_multimode_wheel(struct hid_device *hid, u16 *real_product_id, const u16 bcdDevice)
{
	const u16 reported_product_id = hid->product;
	int ret;

	*real_product_id = lg4ff_identify_multimode_wheel(hid, reported_product_id, bcdDevice);
	/* Probed wheel is not a multimode wheel */
	if (!*real_product_id) {
		*real_product_id = reported_product_id;
		dbg_hid("Wheel is not a multimode wheel\n");
		return LG4FF_MMODE_NOT_MULTIMODE;
	}

	/* Switch from "Driving Force" mode to native mode automatically.
	 * Otherwise keep the wheel in its current mode */
	if (reported_product_id == USB_DEVICE_ID_LOGITECH_WHEEL &&
	    reported_product_id != *real_product_id &&
	    !lg4ff_no_autoswitch) {
		const struct lg4ff_compat_mode_switch *s = lg4ff_get_mode_switch_command(*real_product_id, *real_product_id);

		if (!s) {
			hid_err(hid, "Invalid product id %X\n", *real_product_id);
			return LG4FF_MMODE_NOT_MULTIMODE;
		}

		ret = lg4ff_switch_compatibility_mode(hid, s);
		if (ret) {
			/* Wheel could not have been switched to native mode,
			 * leave it in "Driving Force" mode and continue */
			hid_err(hid, "Unable to switch wheel mode, errno %d\n", ret);
			return LG4FF_MMODE_IS_MULTIMODE;
		}
		return LG4FF_MMODE_SWITCHED;
	}

	return LG4FF_MMODE_IS_MULTIMODE;
}

static void lg4ff_destroy(struct ff_device *ff)
{
}

int lg4ff_init(struct hid_device *hid)
{
	struct hid_input *hidinput = list_entry(hid->inputs.next, struct hid_input, list);
	struct input_dev *dev = hidinput->input;
	struct list_head *report_list = &hid->report_enum[HID_OUTPUT_REPORT].report_list;
	struct hid_report *report = list_entry(report_list->next, struct hid_report, list);
	const struct usb_device_descriptor *udesc = &(hid_to_usb_dev(hid)->descriptor);
	const u16 bcdDevice = le16_to_cpu(udesc->bcdDevice);
	const struct lg4ff_multimode_wheel *mmode_wheel = NULL;
	struct lg4ff_device_entry *entry;
	struct lg_drv_data *drv_data;
	int error, i, j;
	int mmode_ret, mmode_idx = -1;
	u16 real_product_id;
	struct ff_device *ff;

	/* Check that the report looks ok */
	if (!hid_validate_values(hid, HID_OUTPUT_REPORT, 0, 0, 7))
		return -1;

	drv_data = hid_get_drvdata(hid);
	if (!drv_data) {
		hid_err(hid, "Cannot add device, private driver data not allocated\n");
		return -1;
	}
	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	spin_lock_init(&entry->report_lock);
	entry->report = report;
	drv_data->device_props = entry;

	/* Check if a multimode wheel has been connected and
	 * handle it appropriately */
	mmode_ret = lg4ff_handle_multimode_wheel(hid, &real_product_id, bcdDevice);

	/* Wheel has been told to switch to native mode. There is no point in going on
	 * with the initialization as the wheel will do a USB reset when it switches mode
	 */
	if (mmode_ret == LG4FF_MMODE_SWITCHED)
		return 0;
	else if (mmode_ret < 0) {
		hid_err(hid, "Unable to switch device mode during initialization, errno %d\n", mmode_ret);
		error = mmode_ret;
		goto err_init;
	}

	/* Check what wheel has been connected */
	for (i = 0; i < ARRAY_SIZE(lg4ff_devices); i++) {
		if (hid->product == lg4ff_devices[i].product_id) {
			dbg_hid("Found compatible device, product ID %04X\n", lg4ff_devices[i].product_id);
			break;
		}
	}

	if (i == ARRAY_SIZE(lg4ff_devices)) {
		hid_err(hid, "This device is flagged to be handled by the lg4ff module but this module does not know how to handle it. "
			     "Please report this as a bug to LKML, Simon Wood <simon@mungewell.org> or "
			     "Michal Maly <madcatxster@devoid-pointer.net>\n");
		error = -1;
		goto err_init;
	}

	if (mmode_ret == LG4FF_MMODE_IS_MULTIMODE) {
		for (mmode_idx = 0; mmode_idx < ARRAY_SIZE(lg4ff_multimode_wheels); mmode_idx++) {
			if (real_product_id == lg4ff_multimode_wheels[mmode_idx].product_id)
				break;
		}

		if (mmode_idx == ARRAY_SIZE(lg4ff_multimode_wheels)) {
			hid_err(hid, "Device product ID %X is not listed as a multimode wheel", real_product_id);
			error = -1;
			goto err_init;
		}
	}

	/* Set supported force feedback capabilities */
	for (j = 0; lg4ff_devices[i].ff_effects[j] >= 0; j++)
		set_bit(lg4ff_devices[i].ff_effects[j], dev->ffbit);

	error = input_ff_create(dev, LG4FF_MAX_EFFECTS);

	//__clear_bit(FF_RUMBLE, dev->ffbit);

	if (error)
		goto err_init;

	ff = dev->ff;
	ff->upload = lg4ff_upload_effect;
	ff->playback = lg4ff_play_effect;
	ff->set_gain = lg4ff_set_gain;
	ff->destroy = lg4ff_destroy;

	/* Initialize device properties */
	if (mmode_ret == LG4FF_MMODE_IS_MULTIMODE) {
		BUG_ON(mmode_idx == -1);
		mmode_wheel = &lg4ff_multimode_wheels[mmode_idx];
	}
	lg4ff_init_wheel_data(&entry->wdata, &lg4ff_devices[i], mmode_wheel, real_product_id);

	set_bit(FF_GAIN, dev->ffbit);

	/* Check if autocentering is available and
	 * set the centering force to zero by default */
	if (test_bit(FF_AUTOCENTER, dev->ffbit)) {
		/* Formula Force EX expects different autocentering command */
		if ((bcdDevice >> 8) == LG4FF_FFEX_REV_MAJ &&
		    (bcdDevice & 0xff) == LG4FF_FFEX_REV_MIN)
			dev->ff->set_autocenter = lg4ff_set_autocenter_ffex;
		else
			dev->ff->set_autocenter = lg4ff_set_autocenter_default;

		dev->ff->set_autocenter(dev, 0);
	}

	/* Create sysfs interface */
	error = device_create_file(&hid->dev, &dev_attr_combine_pedals);
	if (error)
		hid_warn(hid, "Unable to create sysfs interface for \"combine\", errno %d\n", error);
	error = device_create_file(&hid->dev, &dev_attr_range);
	if (error)
		hid_warn(hid, "Unable to create sysfs interface for \"range\", errno %d\n", error);
	if (mmode_ret == LG4FF_MMODE_IS_MULTIMODE) {
		error = device_create_file(&hid->dev, &dev_attr_real_id);
		if (error)
			hid_warn(hid, "Unable to create sysfs interface for \"real_id\", errno %d\n", error);
		error = device_create_file(&hid->dev, &dev_attr_alternate_modes);
		if (error)
			hid_warn(hid, "Unable to create sysfs interface for \"alternate_modes\", errno %d\n", error);
	}
	dbg_hid("sysfs interface created\n");

	/* Set the maximum range to start with */
	entry->wdata.range = entry->wdata.max_range;
	if (entry->wdata.set_range)
		entry->wdata.set_range(hid, entry->wdata.range);

	entry->hid = hid;
	entry->gain = 0xffff;

	lg4ff_init_slots(entry, dev->ff);

#ifdef CONFIG_LEDS_CLASS
	lg4ff_init_leds(hid, entry, i);
#endif

	hid_info(hid, "Force feedback support for Logitech Gaming Wheels (new)\n");

	hid_info(hid, "HZ (jiffies) = %d, timer period = %d", HZ, jiffies_to_msecs(msecs_to_jiffies(timer_msecs)));

	return 0;

err_init:
	drv_data->device_props = NULL;
	kfree(entry);
	return error;
}

int lg4ff_deinit(struct hid_device *hid)
{
	struct lg4ff_device_entry *entry;
	struct lg_drv_data *drv_data;

	drv_data = hid_get_drvdata(hid);
	if (!drv_data) {
		hid_err(hid, "Error while deinitializing device, no private driver data.\n");
		return -1;
	}
	entry = drv_data->device_props;
	if (!entry)
		goto out; /* Nothing more to do */

	del_timer(&entry->timer);

	/* Multimode devices will have at least the "MODE_NATIVE" bit set */
	if (entry->wdata.alternate_modes) {
		device_remove_file(&hid->dev, &dev_attr_real_id);
		device_remove_file(&hid->dev, &dev_attr_alternate_modes);
	}

	device_remove_file(&hid->dev, &dev_attr_combine_pedals);
	device_remove_file(&hid->dev, &dev_attr_range);
#ifdef CONFIG_LEDS_CLASS
	{
		int j;
		struct led_classdev *led;

		/* Deregister LEDs (if any) */
		for (j = 0; j < 5; j++) {

			led = entry->wdata.led[j];
			entry->wdata.led[j] = NULL;
			if (!led)
				continue;
			led_classdev_unregister(led);
			kfree(led);
		}
	}
#endif
	drv_data->device_props = NULL;

	kfree(entry);
out:
	dbg_hid("Device successfully unregistered\n");
	return 0;
}
