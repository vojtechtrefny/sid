/*
 * This file is part of SID.
 *
 * Copyright (C) 2017-2019 Red Hat, Inc. All rights reserved.
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

#ifndef _SID_CONTEXT_H
#define _SID_CONTEXT_H

#include "service-link-iface.h"
#include "types.h"

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sid_resource sid_resource_t;

typedef struct sid_resource_type {
	const char *name;
	int (*init) (sid_resource_t *res, const void *kickstart_data, void **data);
	int (*destroy) (sid_resource_t *res);
	unsigned int with_event_loop : 1;
	unsigned int with_watchdog   : 1;
} sid_resource_type_t;

#include "resource-type-regs.h"

/*
 * create/destroy functions and related types
 */
typedef enum {
	SID_RESOURCE_RESTRICT_WALK_UP   = UINT64_C(0x0000000000000001),	/* restrict walk from child to parent */
	SID_RESOURCE_RESTRICT_WALK_DOWN = UINT64_C(0x0000000000000002),	/* restrict walk from parent to child */
	SID_RESOURCE_RESTRICT_MASK      = UINT64_C(0x0000000000000003),
	SID_RESOURCE_DISALLOW_ISOLATION = UINT64_C(0x0000000000000004),
} sid_resource_flags_t;

typedef struct sid_resource_service_link_def {
	const char *name;
	service_link_type_t type;
	service_link_notification_t notification;
} sid_resource_service_link_def_t;

#define NULL_SERVICE_LINK ((sid_resource_service_link_def_t) {.name = NULL, \
							      .type = SERVICE_TYPE_NONE, \
							      .notification = SERVICE_NOTIFICATION_NONE})

/* Note: service_link_defs[] array must always be terminated by NULL_SERVICE_LINK */
sid_resource_t *sid_resource_create(sid_resource_t *parent_res, const sid_resource_type_t *type,
				    sid_resource_flags_t flags, const char *id, const void *kickstart_data,
				    sid_resource_service_link_def_t service_link_defs[]);

int sid_resource_destroy(sid_resource_t *res);

/*
 * reference counting support
 */
sid_resource_t *sid_resource_ref(sid_resource_t *res);
int sid_resource_unref(sid_resource_t *res);

/*
 * basic property retrieval functions
 */
bool sid_resource_is_type_of(sid_resource_t *res, const sid_resource_type_t *type);
void *sid_resource_get_data(sid_resource_t *res);
const char *sid_resource_get_full_id(sid_resource_t *res);
const char *sid_resource_get_id(sid_resource_t *res);

/*
 * structure/tree ancestry functions
 */
bool sid_resource_is_ancestor_of_type(sid_resource_t *res, const sid_resource_type_t *type);

#define ID(res) sid_resource_get_full_id(res)

/*
 * structure/tree iterator and 'get' functions and types
 */
typedef struct sid_resource_iter sid_resource_iter_t;

sid_resource_iter_t *sid_resource_iter_create(sid_resource_t *res);
sid_resource_t *sid_resource_iter_current(sid_resource_iter_t *iter);
sid_resource_t *sid_resource_iter_next(sid_resource_iter_t *iter);
sid_resource_t *sid_resource_iter_previous(sid_resource_iter_t *iter);
void sid_resource_iter_reset(sid_resource_iter_t *iter);
void sid_resource_iter_destroy(sid_resource_iter_t *iter);

sid_resource_t *sid_resource_get_parent(sid_resource_t *res);
sid_resource_t *sid_resource_get_top_level(sid_resource_t *res);

typedef enum {
	SID_RESOURCE_SEARCH_IMMEDIATE,     /* only immediate children */
	SID_RESOURCE_SEARCH_BREADTH_FIRST, /* immediate children first, then all further ancenstors */
	SID_RESOURCE_SEARCH_DEPTH_FIRST,   /* child and all its ancestors first, then next child */
} sid_resource_search_method_t;

sid_resource_t *sid_resource_search(sid_resource_t *root_res, sid_resource_search_method_t search_method,
				    const sid_resource_type_t *res_type, const char *id);

/*
 * structure/tree modification functions
 */
int sid_resource_add_child(sid_resource_t *res, sid_resource_t *child);
int sid_resource_isolate(sid_resource_t *res);
int sid_resource_isolate_with_children(sid_resource_t *res);

/*
 * event loop and event handling functions and types
 */
typedef struct sid_resource_event_source sid_resource_event_source_t;

typedef int (*sid_resource_io_event_handler_t)(sid_resource_event_source_t *es, int fd, uint32_t revents, void *data);
typedef int (*sid_resource_signal_event_handler_t)(sid_resource_event_source_t *es, const struct signalfd_siginfo *si, void *data);
typedef int (*sid_resource_child_event_handler_t)(sid_resource_event_source_t *es, const siginfo_t *si, void *data);
typedef int (*sid_resource_time_event_handler_t)(sid_resource_event_source_t *es, uint64_t usec, void *data);
typedef int (*sid_resource_generic_event_handler_t)(sid_resource_event_source_t *es, void *data);

int sid_resource_create_io_event_source(sid_resource_t *res, sid_resource_event_source_t **es, int fd,
					sid_resource_io_event_handler_t handler, const char *name, void *data);

int sid_resource_create_signal_event_source(sid_resource_t *res, sid_resource_event_source_t **es, int signal,
					    sid_resource_signal_event_handler_t handler, const char *name, void *data);

int sid_resource_create_child_event_source(sid_resource_t *res, sid_resource_event_source_t **es, pid_t pid, int options,
					   sid_resource_child_event_handler_t handler, const char *name, void *data);

int sid_resource_create_time_event_source(sid_resource_t *res, sid_resource_event_source_t **es, clockid_t clock,
				   uint64_t usec, uint64_t accuracy, sid_resource_time_event_handler_t handler,
				   const char *name, void *data);

int sid_resource_create_deferred_event_source(sid_resource_t *res, sid_resource_event_source_t **es,
					      sid_resource_generic_event_handler_t handler, const char *name, void *data);

int sid_resource_create_post_event_source(sid_resource_t *res, sid_resource_event_source_t **es,
					  sid_resource_generic_event_handler_t handler, const char *name, void *data);

int sid_resource_create_exit_event_source(sid_resource_t *res, sid_resource_event_source_t **es,
					  sid_resource_generic_event_handler_t handler, const char *name, void *data);

int sid_resource_destroy_event_source(sid_resource_t *res, sid_resource_event_source_t **es);

int sid_resource_run_event_loop(sid_resource_t *res);
int sid_resource_exit_event_loop(sid_resource_t *res);

/*
 * miscellanous functions
 */
void sid_resource_dump_all_in_dot(sid_resource_t *res);

#ifdef __cplusplus
}
#endif

#endif
