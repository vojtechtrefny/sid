/*
 * This file is part of SID.
 *
 * Copyright (C) 2017-2020 Red Hat, Inc. All rights reserved.
 *
 * SID is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * SID is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SID.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _SID_UCMD_MODULE_H
#define _SID_UCMD_MODULE_H

#include "base/macros.h"
#include "base/types.h"
#include "resource/module.h"

#include <inttypes.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sid_ucmd_mod_ctx;
struct sid_ucmd_ctx;

typedef int sid_ucmd_fn_t(struct sid_module *module, struct sid_ucmd_ctx *cmd);
typedef int sid_ucmd_mod_fn_t(struct sid_module *module, struct sid_ucmd_mod_ctx *cmd_mod);

/*
 * Macros to register module's management functions.
 */
#define SID_UCMD_MOD_FN(name, fn)           sid_ucmd_mod_fn_t *sid_ucmd_mod_ ## name = fn;

#ifdef __GNUC__

#define _SID_UCMD_MOD_FN_TO_SID_MODULE_FN_SAFE_CAST(fn) \
	(__builtin_choose_expr(__builtin_types_compatible_p(typeof(fn), sid_ucmd_mod_fn_t), (sid_module_fn_t *) fn, (void) 0))

#define SID_UCMD_MOD_INIT(fn)               SID_MODULE_INIT(_SID_UCMD_MOD_FN_TO_SID_MODULE_FN_SAFE_CAST(fn))
#define SID_UCMD_MOD_RELOAD(fn)             SID_MODULE_RELOAD(_SID_UCMD_MOD_FN_TO_SID_MODULE_FN_SAFE_CAST(fn))
#define SID_UCMD_MOD_EXIT(fn)               SID_MODULE_EXIT(_SID_UCMD_MOD_FN_TO_SID_MODULE_FN_SAFE_CAST(fn))

#else /* __GNUC__ */

#define SID_UCMD_MOD_INIT(fn)               SID_UCMD_MOD_FN(mod_init, fn)   SID_MODULE_INIT((sid_module_fn_t *) fn)
#define SID_UCMD_MOD_RELOAD(fn)             SID_UCMD_MOD_FN(mod_reload, fn) SID_MODULE_RELOAD((sid_module_fn_t *) fn)
#define SID_UCMD_MOD_EXIT(fn)               SID_UCMD_MOD_FN(mod_exit, fn)   SID_MODULE_EXIT((sid_module_fn_t *) fn)

#endif /* __GNUC__ */

/*
 * Macros to register module's phase functions.
 */
#define SID_UCMD_FN(name, fn)               sid_ucmd_fn_t *sid_ucmd_ ## name = fn;

#ifdef __GNUC__

#define _SID_UCMD_FN_CHECK_TYPE(fn) \
	(__builtin_choose_expr(__builtin_types_compatible_p(typeof(fn), sid_ucmd_fn_t), fn, (void) 0))

#else /* __GNUC__ */

#define _SID_UCMD_FN_CHECK_TYPE(fn) fn

#endif /* __GNUC__ */

#define SID_UCMD_IDENT(fn)                  SID_UCMD_FN(ident, _SID_UCMD_FN_CHECK_TYPE(fn))
#define SID_UCMD_SCAN_PRE(fn)               SID_UCMD_FN(scan_pre, _SID_UCMD_FN_CHECK_TYPE(fn))
#define SID_UCMD_SCAN_CURRENT(fn)           SID_UCMD_FN(scan_current, _SID_UCMD_FN_CHECK_TYPE(fn))
#define SID_UCMD_SCAN_NEXT(fn)              SID_UCMD_FN(scan_next, _SID_UCMD_FN_CHECK_TYPE(fn))
#define SID_UCMD_SCAN_POST_CURRENT(fn)      SID_UCMD_FN(scan_post_current, _SID_UCMD_FN_CHECK_TYPE(fn))
#define SID_UCMD_SCAN_POST_NEXT(fn)         SID_UCMD_FN(scan_post_next, _SID_UCMD_FN_CHECK_TYPE(fn))
#define SID_UCMD_TRIGGER_ACTION_CURRENT(fn) SID_UCMD_FN(trigger_action_current, _SID_UCMD_FN_CHECK_TYPE(fn))
#define SID_UCMD_TRIGGER_ACTION_NEXT(fn)    SID_UCMD_FN(trigger_action_next, _SID_UCMD_FN_CHECK_TYPE(fn))
#define SID_UCMD_ERROR(fn)                  SID_UCMD_FN(error, _SID_UCMD_FN_CHECK_TYPE(fn))

/*
 * Functions to retrieve device properties associated with given command ctx.
 */
udev_action_t sid_ucmd_dev_get_action(struct sid_ucmd_ctx *cmd);
udev_devtype_t sid_ucmd_dev_get_type(struct sid_ucmd_ctx *cmd);
int sid_ucmd_dev_get_major(struct sid_ucmd_ctx *cmd);
int sid_ucmd_dev_get_minor(struct sid_ucmd_ctx *cmd);
const char *sid_ucmd_dev_get_name(struct sid_ucmd_ctx *cmd);
uint64_t sid_ucmd_dev_get_seqnum(struct sid_ucmd_ctx *cmd);
const char *sid_ucmd_dev_get_synth_uuid(struct sid_ucmd_ctx *cmd);

typedef enum {
	KV_NS_UNDEFINED, /* namespace not defined */
	KV_NS_UDEV,      /* per-device namespace, it contains records imported from udev, all changed/new records  are exported back to udev */
	KV_NS_GLOBAL,    /* global namespace, it contains records in global scope - visible for all modules and when processing all devices */
	KV_NS_MODULE,    /* per-module namespace, it contains records in the scope of the module that set the record */
	KV_NS_DEVICE,    /* per-device namespace, it contains records in the scope of the device that was being processed when the record was set */
} sid_ucmd_kv_namespace_t;

typedef enum {
	KV_FLAGS_UNSET   = UINT64_C(0x0000000000000000),  /* no flags set */
	KV_PERSISTENT    = UINT64_C(0x0000000000000001),  /* persist record */
	KV_MOD_PROTECTED = UINT64_C(0x0000000000000002),  /* protect record - other modules can read, but can't write */
	KV_MOD_PRIVATE   = UINT64_C(0x0000000000000004),  /* make record private - other modules can't read and can't write */
	KV_MOD_RESERVED  = UINT64_C(0x0000000000000008),  /* reserve key - other modules can't take over the key until the flag is dropped */
} sid_ucmd_kv_flags_t;

#define SID_UCMD_KEY_DEVICE_NEXT_MOD "SID_NEXT_MOD"

void *sid_ucmd_set_kv(struct sid_ucmd_ctx *cmd, sid_ucmd_kv_namespace_t ns,
		      const char *key, const void *value, size_t value_size, sid_ucmd_kv_flags_t flags);
const void *sid_ucmd_get_kv(struct sid_ucmd_ctx *cmd, sid_ucmd_kv_namespace_t ns,
			    const char *key, size_t *value_size, sid_ucmd_kv_flags_t *flags);
const void *sid_ucmd_part_get_disk_kv(struct sid_ucmd_ctx *cmd, const char *key, size_t *value_size, sid_ucmd_kv_flags_t *flags);

int sid_ucmd_mod_reserve_kv(struct sid_module *mod, struct sid_ucmd_mod_ctx *cmd_mod,
			    sid_ucmd_kv_namespace_t ns, const char *key);
int sid_ucmd_mod_unreserve_kv(struct sid_module *mod, struct sid_ucmd_mod_ctx *cmd_mod,
			      sid_ucmd_kv_namespace_t ns, const char *key);

typedef enum {
	DEV_NOT_RDY_UNPROCESSED,  /* not ready and not yet processed by SID */
	DEV_NOT_RDY_INACCESSIBLE, /* not ready and not able to perform IO */
	DEV_NOT_RDY_ACCESSIBLE,   /* not ready and able to perform IO */
	DEV_RDY_PRIVATE,          /* ready and for private use of the module/subsystem */
	DEV_RDY_PUBLIC,           /* ready and publicly available for use */
	DEV_RDY_UNAVAILABLE,      /* ready but temporarily unavailable at the moment, e.g. suspended device */
} dev_ready_t;

typedef enum {
	DEV_RES_UNPROCESSED,	  /* not yet processed by SID */
	DEV_RES_FREE,             /* not yet reserved by a layer above */
	DEV_RES_RESERVED,         /* reserved by a layer above */
} dev_reserved_t;

int sid_ucmd_dev_set_ready(struct sid_ucmd_ctx *cmd, dev_ready_t ready);
dev_ready_t sid_ucmd_dev_get_ready(struct sid_ucmd_ctx *cmd);
int sid_ucmd_dev_set_reserved(struct sid_ucmd_ctx *cmd, dev_reserved_t reserved);
dev_reserved_t sid_ucmd_dev_get_reserved(struct sid_ucmd_ctx *cmd);

int sid_ucmd_group_create(struct sid_ucmd_ctx *cmd, sid_ucmd_kv_namespace_t group_ns,
			  const char *group_id, sid_ucmd_kv_flags_t group_flags);

int sid_ucmd_group_add_current_dev(struct sid_ucmd_ctx *cmd, sid_ucmd_kv_namespace_t group_ns,
				   const char *group_id);

int sid_ucmd_group_remove_current_dev(struct sid_ucmd_ctx *cmd, sid_ucmd_kv_namespace_t group_ns,
				      const char *group_id);

int sid_ucmd_group_destroy(struct sid_ucmd_ctx *cmd, sid_ucmd_kv_namespace_t group_ns,
			   const char *group_id, int force);

#ifdef __cplusplus
}
#endif

#endif