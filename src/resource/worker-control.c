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

#include "resource/worker-control.h"

#include "base/buffer.h"
#include "base/comms.h"
#include "internal/mem.h"
#include "internal/util.h"
#include "log/log.h"
#include "resource/resource.h"

#include <dirent.h>
#include <limits.h>
#include <sys/prctl.h>
#include <unistd.h>

#define WORKER_EXT_NAME                  "ext-worker"

#define DEFAULT_WORKER_IDLE_TIMEOUT_USEC 5000000

typedef enum {
	WORKER_CHANNEL_CMD_NOOP,
	WORKER_CHANNEL_CMD_YIELD,
	WORKER_CHANNEL_CMD_DATA,
	WORKER_CHANNEL_CMD_DATA_EXT,
} worker_channel_cmd_t;

#define WORKER_INT_CHANNEL_MIN_BUF_SIZE sizeof(worker_channel_cmd_t)
#define WORKER_EXT_CHANNEL_MIN_BUF_SIZE 4096

static const char *worker_channel_cmd_str[] = {
	[WORKER_CHANNEL_CMD_NOOP]     = "NOOP",
	[WORKER_CHANNEL_CMD_YIELD]    = "YIELD",
	[WORKER_CHANNEL_CMD_DATA]     = "DATA",
	[WORKER_CHANNEL_CMD_DATA_EXT] = "DATA+EXT",
};

static const char *worker_state_str[] = {[WORKER_STATE_NEW]       = "WORKER_NEW",
                                         [WORKER_STATE_IDLE]      = "WORKER_IDLE",
                                         [WORKER_STATE_ASSIGNED]  = "WORKER_ASSIGNED",
                                         [WORKER_STATE_EXITING]   = "WORKER_EXITING",
                                         [WORKER_STATE_TIMED_OUT] = "WORKER_TIMED_OUT",
                                         [WORKER_STATE_EXITED]    = "WORKER_EXITED"};

const sid_resource_type_t sid_resource_type_worker_proxy;
const sid_resource_type_t sid_resource_type_worker_proxy_with_ev_loop;
const sid_resource_type_t sid_resource_type_worker;

struct worker_init {
	bool                   prepared;
	char                  *id;
	struct worker_channel *channels;
	char                 **ext_argv;
	char                 **ext_envp;
	void                  *arg;
};

struct worker_control {
	worker_type_t               worker_type;
	struct worker_init_cb_spec  init_cb_spec;
	unsigned                    channel_spec_count;
	struct worker_channel_spec *channel_specs;
	struct worker_init          worker_init;
	struct worker_timeout_spec  timeout_spec;
};

struct worker_channel {
	sid_resource_t                   *owner; /* either worker_proxy or worker instance */
	const struct worker_channel_spec *spec;
	struct sid_buffer                *in_buf;
	struct sid_buffer                *out_buf;
	int                               fd;
};

struct worker_kickstart {
	worker_type_t               type;
	pid_t                       pid;
	struct worker_channel_spec *channel_specs;
	struct worker_channel      *channels;
	unsigned                    channel_count;
	struct worker_timeout_spec  timeout_spec;
	void                       *arg;
};

struct worker_proxy {
	pid_t                        pid;             /* worker PID */
	worker_type_t                type;            /* worker type */
	worker_state_t               state;           /* current worker state */
	sid_resource_event_source_t *idle_timeout_es; /* event source to catch idle timeout for worker */
	sid_resource_event_source_t *exec_timeout_es; /* event source to catch execution timeout for worker */
	struct worker_channel       *channels;        /* NULL-terminated array of worker_proxy --> worker channels */
	unsigned                     channel_count;
	struct worker_timeout_spec   timeout_spec;
	void                        *arg;
};

struct worker {
	struct worker_channel_spec *channel_specs;
	struct worker_channel      *channels; /* NULL-terminated array of worker --> worker_proxy channels */
	unsigned                    channel_count;
	unsigned                    parent_exited;
	void                       *arg;
};

static void _change_worker_proxy_state(sid_resource_t *worker_proxy_res, worker_state_t state)
{
	struct worker_proxy *worker_proxy = sid_resource_get_data(worker_proxy_res);

	worker_proxy->state               = state;
	log_debug(ID(worker_proxy_res), "Worker state changed to %s.", worker_state_str[state]);
}

static int _create_channel(sid_resource_t                   *worker_control_res,
                           const struct worker_channel_spec *spec,
                           struct worker_channel            *proxy_chan,
                           struct worker_channel            *chan)
{
	int comms_fds[2];

	proxy_chan->spec = chan->spec = spec;
	proxy_chan->owner = chan->owner = NULL;     /* will be set later with _setup_channel */
	proxy_chan->in_buf = chan->in_buf = NULL;   /* will be set later with _setup_channel */
	proxy_chan->out_buf = chan->out_buf = NULL; /* will be set later with _setup_channel */

	switch (spec->wire.type) {
		case WORKER_WIRE_NONE:
			proxy_chan->fd = chan->fd = -1;
			break;

		case WORKER_WIRE_PIPE_TO_WORKER:
			if (pipe(comms_fds) < 0) {
				proxy_chan->fd = chan->fd = -1;
				log_sys_error(ID(worker_control_res), "pipe", "Failed to create pipe to worker.");
				return -1;
			}

			proxy_chan->fd = comms_fds[1];
			chan->fd       = comms_fds[0];
			break;

		case WORKER_WIRE_PIPE_TO_PROXY:
			if (pipe(comms_fds) < 0) {
				proxy_chan->fd = chan->fd = -1;
				log_sys_error(ID(worker_control_res), "pipe", "Failed to create pipe to worker proxy.");
				return -1;
			}

			proxy_chan->fd = comms_fds[0];
			chan->fd       = comms_fds[1];
			break;

		case WORKER_WIRE_SOCKET:
			// FIXME: See if SOCK_SEQPACKET would be more appropriate here but looks buffers would
			//        also need to be enhanced to use sendmsg/recvmsg instead of pure read/write somehow.
			if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, comms_fds) < 0) {
				proxy_chan->fd = chan->fd = -1;
				log_sys_error(ID(worker_control_res), "socketpair", "Failed to create socket.");
				return -1;
			}

			proxy_chan->fd = comms_fds[0];
			chan->fd       = comms_fds[1];
			break;
	}

	return 0;
}

static int _create_channels(sid_resource_t         *worker_control_res,
                            struct worker_channel **worker_proxy_channels,
                            struct worker_channel **worker_channels)
{
	struct worker_control *worker_control = sid_resource_get_data(worker_control_res);
	struct worker_channel *proxy_chans = NULL, *chans = NULL;
	unsigned               i = 0;

	if (!(proxy_chans = malloc((worker_control->channel_spec_count) * sizeof(struct worker_channel)))) {
		log_error(ID(worker_control_res), "Failed to allocate worker proxy channel array.");
		goto fail;
	}

	if (!(chans = malloc((worker_control->channel_spec_count) * sizeof(struct worker_channel)))) {
		log_error(ID(worker_control_res), "Failed to allocate worker channel array.");
		goto fail;
	}

	while (i < worker_control->channel_spec_count) {
		if (_create_channel(worker_control_res, &worker_control->channel_specs[i], &proxy_chans[i], &chans[i]) < 0)
			goto fail;
		i++;
	}

	*worker_proxy_channels = proxy_chans;
	*worker_channels       = chans;

	return 0;
fail:
	while (i > 0) {
		i--;
		if (proxy_chans[i].fd >= 0)
			close(proxy_chans[i].fd);
		if (chans[i].fd >= 0)
			close(chans[i].fd);
	}

	if (proxy_chans)
		free(proxy_chans);
	if (chans)
		free(chans);

	return -1;
}

#define CHAN_BUF_RECV_MSG 0x1
#define CHAN_BUF_RECV_EOF 0x2

/*
 * Returns:
 *   < 0 error
 *     0 expecting more data
 *     1 MSG
 *     2 EOF
 *     3 MSG + EOF
 */
static int _chan_buf_recv(const struct worker_channel *chan,
                          uint32_t                     revents,
                          worker_channel_cmd_t        *chan_cmd,
                          struct worker_data_spec     *data_spec)
{
	// TODO: Double check we're really interested in EPOLLRDHUP.
	bool          hup = (revents & (EPOLLHUP | EPOLLRDHUP)) && !(revents & EPOLLIN);
	ssize_t       n;
	void         *buf_data;
	size_t        buf_data_size;
	unsigned char byte;

	if (revents & EPOLLERR) {
		if (hup)
			log_error(ID(chan->owner), "Peer closed channel %s prematurely.", chan->spec->id);
		else
			log_error(ID(chan->owner), "Error detected on channel %s.", chan->spec->id);

		return -EPIPE;
	}

	n = sid_buffer_read(chan->in_buf, chan->fd);

	if (n > 0) {
		/* For plain buffers, we are waiting for EOF to complete the message. */
		if (sid_buffer_stat(chan->in_buf).spec.mode == SID_BUFFER_MODE_PLAIN)
			return 0;

		if (!sid_buffer_is_complete(chan->in_buf, NULL))
			return 0;

		(void) sid_buffer_get_data(chan->in_buf, (const void **) &buf_data, &buf_data_size);

		/*
		 * Internal workers and associated proxies use SID_BUFFER_MODE_SIZE_PREFIX buffers and
		 * they always transmit worker_channel_cmd_t as header before actual data.
		 */
		memcpy(chan_cmd, buf_data, sizeof(*chan_cmd));
		data_spec->data_size = buf_data_size - sizeof(*chan_cmd);
		data_spec->data      = data_spec->data_size > 0 ? buf_data + sizeof(*chan_cmd) : NULL;

		if (*chan_cmd == WORKER_CHANNEL_CMD_DATA_EXT) {
			if (chan->spec->wire.type == WORKER_WIRE_SOCKET) {
				/* Also read ancillary data in a channel with socket wire - an FD might be passed through this way.
				 */
				/*
				 * FIXME: Buffer is using 'read', but we need to use 'recvmsg' for ancillary data.
				 *        This is why we need to receive the ancillary data separately from usual data here.
				 *        Maybe extend the buffer so it can use 'recvmsg' somehow - a custom callback
				 *        for reading the data? Then we could receive data and anc. data at once in one
				 *        buffer_read call.
				 *
				 * TODO:  Make this a part of event loop instead of looping here!
				 */
				for (;;) {
					n = sid_comms_unix_recv(chan->fd, &byte, sizeof(byte), &data_spec->ext.socket.fd_pass);

					if (n < 0) {
						if (n == -EAGAIN || n == -EINTR)
							continue;

						data_spec->ext.socket.fd_pass = -1;
						log_error_errno(ID(chan->owner),
						                n,
						                "Failed to read ancillary data on channel %s",
						                chan->spec->id);

						return n;
					}

					data_spec->ext.used = true;
					break;
				}
			}
		}

		return CHAN_BUF_RECV_MSG;
	} else if (n < 0) {
		if (n == -EAGAIN || n == -EINTR)
			return 0;

		log_error_errno(ID(chan->owner), n, "Failed to read data on channel %s", chan->spec->id);
		return n;
	} else {
		if (sid_buffer_stat(chan->in_buf).spec.mode == SID_BUFFER_MODE_PLAIN) {
			(void) sid_buffer_get_data(chan->in_buf, (const void **) &buf_data, &buf_data_size);

			*chan_cmd            = WORKER_CHANNEL_CMD_DATA;
			data_spec->data_size = buf_data_size;
			data_spec->data      = buf_data;

			return CHAN_BUF_RECV_EOF | CHAN_BUF_RECV_MSG;
		}

		return CHAN_BUF_RECV_EOF;
	}
}

static int _make_worker_exit(sid_resource_t *worker_proxy_res)
{
	struct worker_proxy *worker_proxy = sid_resource_get_data(worker_proxy_res);
	int                  r;

	if (!(r = kill(worker_proxy->pid, SIGTERM)))
		_change_worker_proxy_state(worker_proxy_res, WORKER_STATE_EXITING);

	return r;
}

static const char _unexpected_internal_command_msg[]    = "unexpected internal command received.";
static const char _custom_message_handling_failed_msg[] = "Custom message handling failed.";

static int _on_worker_proxy_channel_event(sid_resource_event_source_t *es, int fd, uint32_t revents, void *data)
{
	struct worker_channel  *chan = data;
	worker_channel_cmd_t    chan_cmd;
	struct worker_data_spec data_spec = {0};
	/*uint64_t timeout_usec;*/
	int r;

	r = _chan_buf_recv(chan, revents, &chan_cmd, &data_spec);

	if (r == 0)
		return 0;

	if (r < 0) {
		if (chan->in_buf)
			sid_buffer_reset(chan->in_buf);
		return r;
	}

	if (r & CHAN_BUF_RECV_MSG) {
		switch (chan_cmd) {
			case WORKER_CHANNEL_CMD_YIELD:
				/* FIXME: Make timeout configurable. If timeout is set to zero, exit worker right away - call
				_make_worker_exit.
				 *
				timeout_usec = util_get_now_usec(CLOCK_MONOTONIC) + DEFAULT_WORKER_IDLE_TIMEOUT_USEC;
				sid_resource_create_time_event_source(chan->owner, &worker_proxy->idle_timeout_es, CLOCK_MONOTONIC,
				                                      timeout_usec, 0, _on_worker_proxy_idle_timeout_event, "idle
				timeout", chan->owner); _change_worker_proxy_state(chan->owner, WORKER_STATE_IDLE);
				*/
				_make_worker_exit(chan->owner);
				break;
			case WORKER_CHANNEL_CMD_DATA:
			case WORKER_CHANNEL_CMD_DATA_EXT:
				if (chan->spec->proxy_rx_cb.cb) {
					if (chan->spec->proxy_rx_cb.cb(chan->owner, chan, &data_spec, chan->spec->proxy_rx_cb.arg) <
					    0)
						log_warning(ID(chan->owner), "%s", _custom_message_handling_failed_msg);
				}
				break;
			default:
				log_error(ID(chan->owner),
				          INTERNAL_ERROR "%s %s",
				          worker_channel_cmd_str[chan_cmd],
				          _unexpected_internal_command_msg);
		}

		sid_buffer_reset(chan->in_buf);
	}

	if (r & CHAN_BUF_RECV_EOF)
		sid_resource_destroy_event_source(&es);

	return 0;
}

static int _on_worker_channel_event(sid_resource_event_source_t *es, int fd, uint32_t revents, void *data)
{
	struct worker_channel  *chan = data;
	worker_channel_cmd_t    chan_cmd;
	struct worker_data_spec data_spec = {0};
	int                     r;

	r = _chan_buf_recv(chan, revents, &chan_cmd, &data_spec);

	if (r == 0)
		return 0;

	if (r < 0) {
		if (chan->in_buf)
			sid_buffer_reset(chan->in_buf);
		return r;
	}

	if (r & CHAN_BUF_RECV_MSG) {
		switch (chan_cmd) {
			case WORKER_CHANNEL_CMD_DATA:
			case WORKER_CHANNEL_CMD_DATA_EXT:
				if (chan->spec->worker_rx_cb.cb) {
					if (chan->spec->worker_rx_cb.cb(chan->owner,
					                                chan,
					                                &data_spec,
					                                chan->spec->worker_rx_cb.arg) < 0)
						log_warning(ID(chan->owner), "%s", _custom_message_handling_failed_msg);
				}
				break;
			default:
				log_error(ID(chan->owner),
				          INTERNAL_ERROR "%s %s",
				          worker_channel_cmd_str[chan_cmd],
				          _unexpected_internal_command_msg);
		}

		sid_buffer_reset(chan->in_buf);
	}

	if (r & CHAN_BUF_RECV_EOF)
		sid_resource_destroy_event_source(&es);

	return 0;
}

static int
	_setup_channel(sid_resource_t *owner, const char *alt_id, bool is_worker, worker_type_t type, struct worker_channel *chan)
{
	struct sid_buffer    **buf1, **buf2;
	struct sid_buffer_spec buf_spec = {.backend = SID_BUFFER_BACKEND_MALLOC, .type = SID_BUFFER_TYPE_LINEAR, .mode = 0};
	struct sid_buffer_init buf_init = {0};
	const char            *id       = owner ? ID(owner) : alt_id;
	int                    r;

	if (chan->in_buf || chan->out_buf) {
		log_error(id, INTERNAL_ERROR "%s: Buffers already set.", __func__);
		r = -EINVAL;
		goto fail;
	}

	/*
	 * Buffer wiring scheme:
	 *
	 * WORKER:
	 *   WORKER_TYPE_INTERNAL:
	 *     (A)
	 *     buf1: [ worker/out_buf ]  -> proxy/in_buf
	 *     buf2: [ worker/in_buf  ] <-  proxy/out_buf
	 *
	 *   WORKER_TYPE_EXTERNAL:
	 *     (B)
	 *     external worker - we have no control over buffers here
	 *
	 *
	 * PROXY:
	 *   WORKER_TYPE_INTERNAL:
	 *     (C)
	 *     buf1:   worker/out_buf    -> [ proxy/in_buf  ]
	 *     buf2:   worker/in_buf    <-  [ proxy/out_buf ]
	 *
	 *   WORKER_TYPE_EXTERNAL:
	 *     (D)
	 *     buf1:   worker/out_buf    -> [ proxy/in_buf  ]
	 *     buf2:   worker/in_buf    <-  [ proxy/out_buf ]
	 */

	if (is_worker) {
		/* WORKER */
		if (type == WORKER_TYPE_INTERNAL) {
			/* (A) */
			buf1 = &chan->out_buf;
			buf2 = &chan->in_buf;
		} else {
			/* (B) */
			buf1 = NULL;
			buf2 = NULL;
		}
	} else {
		/* PROXY */
		/* (C) (D) */
		buf1 = &chan->in_buf;
		buf2 = &chan->out_buf;
	}

	if (buf1 || buf2) {
		switch (type) {
			case WORKER_TYPE_INTERNAL:
				/*
				 * For internal workers, we have complete control on how data are sent and so we mandate
				 * use of data size prefixes on both sides of the channel so we always know how much data
				 * to receive on the other side and we can preallocate proper buffer size for it.
				 */
				buf_spec.mode       = SID_BUFFER_MODE_SIZE_PREFIX;
				buf_init.size       = WORKER_INT_CHANNEL_MIN_BUF_SIZE;
				buf_init.alloc_step = 1;
				break;
			case WORKER_TYPE_EXTERNAL:
				/*
				 * We can't use data size prefix for external workers - they simply send us plain data.
				 * Since we don't know in advance how much data to accept, we use WORKER_EXT_CHANNEL_MIN_BUF_SIZE
				 * and then we extend the buffer with WORKER_EXT_CHANNEL_MIN_BUF_SIZE each time it's filled up
				 * and data are still incoming.
				 */
				buf_spec.mode       = SID_BUFFER_MODE_PLAIN;
				buf_init.size       = WORKER_EXT_CHANNEL_MIN_BUF_SIZE;
				buf_init.alloc_step = WORKER_EXT_CHANNEL_MIN_BUF_SIZE;
				break;
		}
	}

	switch (chan->spec->wire.type) {
		case WORKER_WIRE_NONE:
			break;

		case WORKER_WIRE_PIPE_TO_WORKER:
			if (buf2 && !(*buf2 = sid_buffer_create(&buf_spec, &buf_init, &r))) {
				log_error_errno(id, r, "Failed to create buffer for channel with ID %s.", chan->spec->id);
				goto fail;
			}

			if (is_worker && chan->spec->wire.ext.used && chan->spec->wire.ext.pipe.fd_redir >= 0) {
				if (dup2(chan->fd, chan->spec->wire.ext.pipe.fd_redir) < 0) {
					log_error_errno(id,
					                errno,
					                "Failed to redirect FD %d through channel %s",
					                chan->spec->wire.ext.pipe.fd_redir,
					                chan->spec->id);
					r = -errno;
					goto fail;
				}
				close(chan->fd);
				chan->fd = -1;
			}

			break;

		case WORKER_WIRE_PIPE_TO_PROXY:
			if (buf1 && !(*buf1 = sid_buffer_create(&buf_spec, &buf_init, &r)))
				goto fail;

			if (is_worker && chan->spec->wire.ext.used && chan->spec->wire.ext.pipe.fd_redir >= 0) {
				if (dup2(chan->fd, chan->spec->wire.ext.pipe.fd_redir) < 0) {
					log_error_errno(id,
					                errno,
					                "Failed to redirect FD %d through channel %s",
					                chan->spec->wire.ext.pipe.fd_redir,
					                chan->spec->id);
					r = -errno;
					goto fail;
				}
				close(chan->fd);
				chan->fd = -1;
			}

			break;

		case WORKER_WIRE_SOCKET:
			if ((buf1 && !(*buf1 = sid_buffer_create(&buf_spec, &buf_init, &r))) ||
			    (buf2 && !(*buf2 = sid_buffer_create(&buf_spec, &buf_init, &r)))) {
				log_error_errno(id, r, "Failed to create buffer for channel with ID %s.", chan->spec->id);
				goto fail;
			}

			if (is_worker && chan->spec->wire.ext.used && chan->spec->wire.ext.socket.fd_redir >= 0) {
				if (dup2(chan->fd, chan->spec->wire.ext.socket.fd_redir) < 0) {
					log_error_errno(id,
					                errno,
					                "Failed to redirect FD %d through channel %s : WORKER_WIRE_SOCKET",
					                chan->spec->wire.ext.pipe.fd_redir,
					                chan->spec->id);
				}

				close(chan->fd);
				chan->fd = -1;
			}
			break;
	}

	if (owner) {
		if (sid_resource_create_io_event_source(owner,
		                                        NULL,
		                                        chan->fd,
		                                        is_worker ? _on_worker_channel_event : _on_worker_proxy_channel_event,
		                                        0,
		                                        chan->spec->id,
		                                        chan) < 0) {
			log_error(id, "Failed to register communication channel with ID %s.", chan->spec->id);
			r = -1;
			goto fail;
		}

		chan->owner = owner;
	}

	return 0;
fail:
	if (chan->in_buf) {
		sid_buffer_destroy(chan->in_buf);
		chan->in_buf = NULL;
	}

	if (chan->out_buf) {
		sid_buffer_destroy(chan->out_buf);
		chan->out_buf = NULL;
	}

	return r;
}

static int _setup_channels(sid_resource_t        *owner,
                           const char            *alt_id,
                           worker_type_t          type,
                           struct worker_channel *chans,
                           unsigned               chan_count)
{
	bool     is_worker;
	unsigned i = 0;

	is_worker  = owner ? worker_control_is_worker(owner) : true;

	while (i < chan_count) {
		if (_setup_channel(owner, alt_id, is_worker, type, &chans[i]) < 0)
			goto fail;
		i++;
	}

	return 0;
fail:
	/*
	 * If _setup_channel failed, it already cleaned up channel buffers for the exact channel
	 * it was processing. Here, we clean up all the buffers for channels already setup right
	 * before this failure.
	 */
	while (i > 0) {
		i--;

		if (chans[i].in_buf) {
			sid_buffer_destroy(chans[i].in_buf);
			chans[i].in_buf = NULL;
		}
		if (chans[i].out_buf) {
			sid_buffer_destroy(chans[i].out_buf);
			chans[i].out_buf = NULL;
		}
	}

	return -1;
}

void _destroy_channels(struct worker_channel *channels, unsigned channel_count)
{
	unsigned               i;
	struct worker_channel *chan;

	for (i = 0; i < channel_count; i++) {
		chan = &channels[i];

		switch (chan->spec->wire.type) {
			case WORKER_WIRE_NONE:
				break;
			case WORKER_WIRE_SOCKET:
			case WORKER_WIRE_PIPE_TO_WORKER:
			case WORKER_WIRE_PIPE_TO_PROXY:
				if (chan->fd >= 0)
					close(chan->fd);
				break;
		}

		if (chan->in_buf)
			sid_buffer_destroy(chan->in_buf);

		if (chan->out_buf)
			sid_buffer_destroy(chan->out_buf);
	}

	free(channels);
}

int _close_non_channel_fds(sid_resource_t *res, struct worker_channel *channels, unsigned channel_count)
{
	static const char *proc_self_fd_dir = SYSTEM_PROC_PATH "/self/fd";
	DIR               *d;
	struct dirent     *dirent;
	char              *p;
	int                fd, i, r = -1;

	if (!(d = opendir(proc_self_fd_dir))) {
		log_sys_error(ID(res), "opendir", proc_self_fd_dir);
		goto out;
	}

	while ((dirent = readdir(d))) {
		errno = 0;
		fd    = strtol(dirent->d_name, &p, 10);
		if (errno || !p || *p || fd >= INT_MAX)
			continue;

		if (fd == dirfd(d))
			continue;

		for (i = 0; i < channel_count; i++) {
			if (channels[i].fd == fd)
				break;
		}

		if (i < channel_count)
			continue;

		if (close(fd) < 0) {
			log_sys_error(ID(res), "close non-channel fd", dirent->d_name);
			goto out;
		}

		log_debug(ID(res), "Closed non-channel fd %d.", fd);
	}

	r = 0;
out:
	if (d) {
		if (closedir(d) < 0) {
			log_sys_error(ID(res), "closedir", proc_self_fd_dir);
			r = -1;
		}
	}
	return r;
}

static int _do_worker_control_get_new_worker(sid_resource_t       *worker_control_res,
                                             struct worker_params *params,
                                             sid_resource_t      **res_p,
                                             bool                  with_event_loop)
{
	struct worker_control  *worker_control        = sid_resource_get_data(worker_control_res);
	struct worker_channel  *worker_proxy_channels = NULL, *worker_channels = NULL;
	struct worker_kickstart kickstart;
	sigset_t                original_sigmask, new_sigmask;
	pid_t                   original_pid, curr_ppid;
	sid_resource_t         *res             = NULL;
	int                     signals_blocked = 0;
	pid_t                   pid             = -1;
	const char             *id;
	char                    gen_id[32];

	*res_p = NULL;

	if (_create_channels(worker_control_res, &worker_proxy_channels, &worker_channels) < 0) {
		log_error(ID(worker_control_res), "Failed to create worker channels.");
		goto out;
	}

	if (sigfillset(&new_sigmask) < 0) {
		log_sys_error(ID(worker_control_res), "sigfillset", "");
		goto out;
	}

	if (sigprocmask(SIG_SETMASK, &new_sigmask, &original_sigmask) < 0) {
		log_sys_error(ID(worker_control_res), "sigprocmask", "blocking signals before fork");
		goto out;
	}
	signals_blocked = 1;
	original_pid    = getpid();

	if ((pid = fork()) < 0) {
		log_sys_error(ID(worker_control_res), "fork", "");
		goto out;
	}

	if (pid == 0) {
		/*
		 *  WORKER HERE
		 */

		if (worker_control->worker_type == WORKER_TYPE_INTERNAL) {
			/* WORKER_TYPE_INTERNAL - request a SIGUSR1 signal if parent dies */
			if (prctl(PR_SET_PDEATHSIG, SIGUSR1) < 0)
				log_sys_error(ID(worker_control_res),
				              "prctl",
				              "failed to set parent-death signal for internal worker");
		} else {
			/* WORKER_TYPE_EXTERNAL - request a SIGTERM signal if parent dies */
			if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0)
				log_sys_error(ID(worker_control_res),
				              "prctl",
				              "failed to set parent-death signal for external worker");
		}

		/* Check to make sure the parent didn't die right after the fork() */
		curr_ppid = getppid();
		if (curr_ppid != original_pid) {
			log_debug(ID(worker_control_res), "parent died before prctl() call completed - exiting SID worker process");
			raise(SIGTERM);
			exit(EXIT_FAILURE);
		}

		_destroy_channels(worker_proxy_channels, worker_control->channel_spec_count);

		worker_proxy_channels                = NULL;
		worker_control->worker_init.channels = worker_channels;

		if (worker_control->worker_type == WORKER_TYPE_INTERNAL)
			worker_control->worker_init.id = params->id ? strdup(params->id) : NULL;
		else {
			/*
			 * WORKER_TYPE_EXTERNAL
			 */
			if (_close_non_channel_fds(worker_control_res, worker_channels, worker_control->channel_spec_count) < 0)
				exit(EXIT_FAILURE);

			if (!params->id || asprintf(&worker_control->worker_init.id, "%s/%s", WORKER_EXT_NAME, params->id) < 0)
				worker_control->worker_init.id = NULL;

			if (!(worker_control->worker_init.ext_argv = util_str_comb_to_strv(NULL,
			                                                                   params->external.exec_file,
			                                                                   params->external.args,
			                                                                   NULL,
			                                                                   UTIL_STR_DEFAULT_DELIMS,
			                                                                   UTIL_STR_DEFAULT_QUOTES)) ||
			    !(worker_control->worker_init.ext_envp = util_str_comb_to_strv(NULL,
			                                                                   NULL,
			                                                                   params->external.env,
			                                                                   NULL,
			                                                                   UTIL_STR_DEFAULT_DELIMS,
			                                                                   UTIL_STR_DEFAULT_QUOTES))) {
				log_error(worker_control->worker_init.id ?: WORKER_EXT_NAME,
				          "Failed to convert argument and environment strings to vectors.");
				exit(EXIT_FAILURE);
			}
		}

		worker_control->worker_init.arg      = params->worker_arg;
		worker_control->worker_init.prepared = true;

		/* worker processes return with *res_p == NULL, to differentiate them from the proxy process*/
		return 0;
	}
	/*
	 * WORKER PROXY HERE
	 */

	log_debug(ID(worker_control_res), "Created new worker process with PID %d.", pid);

	_destroy_channels(worker_channels, worker_control->channel_spec_count);
	worker_channels         = NULL;

	kickstart.type          = worker_control->worker_type;
	kickstart.pid           = pid;
	kickstart.channels      = worker_proxy_channels;
	kickstart.channel_count = worker_control->channel_spec_count;
	kickstart.arg           = params->worker_proxy_arg;

	if (params->timeout_spec.usec)
		/* override default timeout for this single worker */
		kickstart.timeout_spec = params->timeout_spec;
	else
		/* use default timeout */
		kickstart.timeout_spec = worker_control->timeout_spec;

	if (!(id = params->id)) {
		(void) util_process_pid_to_str(kickstart.pid, gen_id, sizeof(gen_id));
		id = gen_id;
	}

	res = sid_resource_create(worker_control_res,
	                          with_event_loop ? &sid_resource_type_worker_proxy_with_ev_loop : &sid_resource_type_worker_proxy,
	                          SID_RESOURCE_DISALLOW_ISOLATION,
	                          id,
	                          &kickstart,
	                          SID_RESOURCE_PRIO_NORMAL,
	                          SID_RESOURCE_NO_SERVICE_LINKS);
out:
	if (!res) {
		if (worker_proxy_channels)
			_destroy_channels(worker_proxy_channels, worker_control->channel_spec_count);

		if (worker_channels)
			_destroy_channels(worker_channels, worker_control->channel_spec_count);

		// FIXME: also terminate worker if worker_proxy creation fails
	}

	if (signals_blocked && sigprocmask(SIG_SETMASK, &original_sigmask, NULL) < 0)
		log_sys_error(ID(res), "sigprocmask", "after forking process");

	/* return worker proxy resource */
	*res_p = res;
	return res ? 0 : -1;
}

int worker_control_get_new_worker(sid_resource_t *worker_control_res, struct worker_params *params, sid_resource_t **res_p)
{
	return _do_worker_control_get_new_worker(worker_control_res, params, res_p, false);
}

int _run_internal_worker(sid_resource_t *worker_control_res)
{
	struct worker_control  *worker_control = sid_resource_get_data(worker_control_res);
	struct worker_kickstart kickstart;
	sid_resource_t         *res;
	const char             *id;
	char                    gen_id[32];

	kickstart.type          = WORKER_TYPE_INTERNAL;
	kickstart.pid           = getpid();
	kickstart.channels      = worker_control->worker_init.channels;
	kickstart.channel_count = worker_control->channel_spec_count;
	kickstart.channel_specs = worker_control->channel_specs;
	kickstart.arg           = worker_control->worker_init.arg;

	if (!(id = worker_control->worker_init.id)) {
		(void) util_process_pid_to_str(kickstart.pid, gen_id, sizeof(gen_id));
		id = gen_id;
	}

	res = sid_resource_create(SID_RESOURCE_NO_PARENT,
	                          &sid_resource_type_worker,
	                          SID_RESOURCE_NO_FLAGS,
	                          id,
	                          &kickstart,
	                          SID_RESOURCE_PRIO_NORMAL,
	                          SID_RESOURCE_NO_SERVICE_LINKS);
	if (!res) {
		(void) sid_resource_unref(sid_resource_search(worker_control_res, SID_RESOURCE_SEARCH_TOP, NULL, NULL));
		return -1;
	}
	/*
	 * We are going to destroy the worker_control and the worker already
	 * has a reference to the channels and the channel_specs. Set them
	 * to NULL, so they aren't freed when the worker_control is destroyed
	 */
	worker_control->worker_init.channels = NULL;
	worker_control->channel_specs        = NULL;

	if (worker_control->init_cb_spec.cb)
		(void) worker_control->init_cb_spec.cb(res, worker_control->init_cb_spec.arg);

	return sid_resource_run_event_loop(res);
}

int _run_external_worker(sid_resource_t *worker_control_res)
{
	struct worker_control      *worker_control = sid_resource_get_data(worker_control_res);
	struct worker_channel_spec *channel_specs;
	struct worker_channel      *channels;
	unsigned                    channel_count;
	char                      **argv, **envp;
	char                       *id;
	char                        gen_id[32];
	int                         r;

	/*
	 * We may destroy the worker_control. Save references to the values we
	 * need to keep, and set them to NULL, so they aren't freed if the
	 * worker_control is destroyed
	 */
	if (!(id = worker_control->worker_init.id)) {
		snprintf(gen_id, sizeof(gen_id), "%s/%d", WORKER_EXT_NAME, getpid());
		id = gen_id;
	}
	worker_control->worker_init.id       = NULL;

	channels                             = worker_control->worker_init.channels;
	worker_control->worker_init.channels = NULL;
	channel_count                        = worker_control->channel_spec_count;

	channel_specs                        = worker_control->channel_specs;
	worker_control->channel_specs        = NULL;

	argv                                 = worker_control->worker_init.ext_argv;
	worker_control->worker_init.ext_argv = NULL;

	envp                                 = worker_control->worker_init.ext_envp;
	worker_control->worker_init.ext_envp = NULL;

	if ((r = _setup_channels(NULL, id, WORKER_TYPE_EXTERNAL, channels, channel_count)) < 0)
		goto fail;

	if (worker_control->init_cb_spec.cb && (r = worker_control->init_cb_spec.cb(NULL, worker_control->init_cb_spec.arg)) < 0)
		goto fail;

	r = execve(argv[0], argv, envp);
	/* On success, execve never returns */
	log_sys_error(id, "execve", "");
fail:
	if (id != gen_id)
		free(id);
	_destroy_channels(channels, channel_count);
	free(channel_specs);
	free(argv);
	free(envp);
	return r;
}

int worker_control_run_worker(sid_resource_t *worker_control_res)
{
	struct worker_control *worker_control = sid_resource_get_data(worker_control_res);

	if (!worker_control->worker_init.prepared)
		return -ESRCH;

	worker_control->worker_init.prepared = false;

	if (worker_control->worker_type == WORKER_TYPE_INTERNAL)
		return _run_internal_worker(worker_control_res);

	return _run_external_worker(worker_control_res);
}

/*
 * FIXME: Cleanup resources before running the external worker or do
 *        something to make valgrind happy, otherwise it will report memleaks.
 */
int worker_control_run_new_worker(sid_resource_t *worker_control_res, struct worker_params *params)
{
	struct worker_control *worker_control = sid_resource_get_data(worker_control_res);
	sid_resource_t        *proxy_res;
	int                    r;

	if (worker_control->worker_type != WORKER_TYPE_EXTERNAL)
		return -ENOTSUP;

	if (worker_control->worker_init.prepared)
		return -EBUSY;

	if ((r = _do_worker_control_get_new_worker(worker_control_res, params, &proxy_res, true)) < 0)
		return r;

	if (proxy_res)
		/*
		 * WORKER PROXY HERE
		 */
		return sid_resource_run_event_loop(proxy_res);
	else
		/*
		 * WORKER HERE
		 */
		return worker_control_run_worker(worker_control_res);
}

sid_resource_t *worker_control_get_idle_worker(sid_resource_t *worker_control_res)
{
	sid_resource_iter_t *iter;
	sid_resource_t      *res;

	if (!(iter = sid_resource_iter_create(worker_control_res)))
		return NULL;

	while ((res = sid_resource_iter_next(iter))) {
		if (((struct worker_proxy *) sid_resource_get_data(res))->state == WORKER_STATE_IDLE)
			break;
	}

	sid_resource_iter_destroy(iter);
	return res;
}

sid_resource_t *worker_control_find_worker(sid_resource_t *worker_control_res, const char *id)
{
	return sid_resource_search(worker_control_res, SID_RESOURCE_SEARCH_IMM_DESC, &sid_resource_type_worker_proxy, id);
}

bool worker_control_is_worker(sid_resource_t *res)
{
	// TODO: detect external worker
	if (sid_resource_match(res, &sid_resource_type_worker, NULL))
		return true;
	else if (sid_resource_match(res, &sid_resource_type_worker_proxy, NULL))
		return false;
	else
		return sid_resource_search(res, SID_RESOURCE_SEARCH_ANC, &sid_resource_type_worker, NULL) != NULL;
}

worker_state_t worker_control_get_worker_state(sid_resource_t *res)
{
	struct worker_proxy *worker_proxy;

	do {
		if (sid_resource_match(res, &sid_resource_type_worker_proxy, NULL) ||
		    sid_resource_match(res, &sid_resource_type_worker_proxy_with_ev_loop, NULL)) {
			worker_proxy = sid_resource_get_data(res);
			return worker_proxy->state;
		}
	} while ((res = sid_resource_search(res, SID_RESOURCE_SEARCH_IMM_ANC, NULL, NULL)));

	return WORKER_STATE_UNKNOWN;
}

const char *worker_control_get_worker_id(sid_resource_t *res)
{
	do {
		if (sid_resource_match(res, &sid_resource_type_worker, NULL) ||
		    sid_resource_match(res, &sid_resource_type_worker_proxy, NULL) ||
		    sid_resource_match(res, &sid_resource_type_worker_proxy_with_ev_loop, NULL))
			return sid_resource_get_id(res);
	} while ((res = sid_resource_search(res, SID_RESOURCE_SEARCH_IMM_ANC, NULL, NULL)));

	return NULL;
}

void *worker_control_get_worker_arg(sid_resource_t *res)
{
	do {
		if (sid_resource_match(res, &sid_resource_type_worker, NULL))
			return (((struct worker *) sid_resource_get_data(res))->arg);
		else if (sid_resource_match(res, &sid_resource_type_worker_proxy, NULL) ||
		         sid_resource_match(res, &sid_resource_type_worker_proxy_with_ev_loop, NULL))
			return (((struct worker_proxy *) sid_resource_get_data(res))->arg);
	} while ((res = sid_resource_search(res, SID_RESOURCE_SEARCH_IMM_ANC, NULL, NULL)));

	return NULL;
}

/* FIXME: Consider making this a part of event loop. */
static int _chan_buf_send(const struct worker_channel *chan, worker_channel_cmd_t chan_cmd, struct worker_data_spec *data_spec)
{
	static unsigned char byte     = 0xFF;
	int                  has_data = data_spec && data_spec->data && data_spec->data_size;
	ssize_t              n;
	int                  r = 0;

	/*
	 * Internal workers and associated proxies use SID_BUFFER_MODE_SIZE_PREFIX buffers and
	 * they always transmit worker_channel_cmd_t as header before actual data.
	 * FIXME: avoid using SID_BUFFER_MODE_SIZE_PREFIX for this detection
	 */
	if (sid_buffer_stat(chan->out_buf).spec.mode == SID_BUFFER_MODE_SIZE_PREFIX &&
	    (sid_buffer_add(chan->out_buf, &chan_cmd, sizeof(chan_cmd), NULL, NULL)) < 0) {
		r = -ENOMEM;
		goto out;
	}

	if (has_data && (sid_buffer_add(chan->out_buf, data_spec->data, data_spec->data_size, NULL, NULL) < 0)) {
		r = -ENOMEM;
		goto out;
	}

	if ((r = sid_buffer_write_all(chan->out_buf, chan->fd)) < 0) {
		log_error_errno(ID(chan->owner), r, "Failed to write data on channel %s", chan->spec->id);
		goto out;
	}

	if (data_spec && data_spec->ext.used) {
		if (chan->spec->wire.type == WORKER_WIRE_SOCKET) {
			/* Also send ancillary data in a channel with socket wire - an FD might be passed through this way. */
			/*
			 * FIXME: Buffer is using 'write', but we need to use 'sendmsg' (wrapped by sid_comms_unix_send) for
			 * ancillary data. This is why we need to send the ancillary data separately from usual data here. Maybe
			 * extend the buffer so it can use 'sendmsg' somehow - a custom callback for writing the data? Then we could
			 * send data and anc. data at once in one buffer_write call.
			 */
			for (;;) {
				n = sid_comms_unix_send(chan->fd, &byte, sizeof(byte), data_spec->ext.socket.fd_pass);

				if (n < 0) {
					if (n == -EAGAIN || n == -EINTR)
						continue;

					log_error_errno(ID(chan->owner),
					                n,
					                "Failed to send ancillary data on channel %s",
					                chan->spec->id);
					r = n;
					goto out;
				}

				break;
			}
		}
	}
out:
	(void) sid_buffer_reset(chan->out_buf);
	return r;
}

static struct worker_channel *_get_channel(struct worker_channel *channels, unsigned channel_count, const char *channel_id)
{
	struct worker_channel *chan;
	unsigned               i;

	for (i = 0; i < channel_count; i++) {
		chan = &channels[i];
		if (!strcmp(chan->spec->id, channel_id))
			return chan;
	}

	return NULL;
}

int worker_control_channel_send(sid_resource_t *current_res, const char *channel_id, struct worker_data_spec *data_spec)
{
	sid_resource_t        *res = current_res;
	struct worker_proxy   *worker_proxy;
	struct worker         *worker;
	struct worker_channel *chan;

	if (!channel_id || !*channel_id)
		return -ECHRNG;

	if (sid_resource_match(res, &sid_resource_type_worker_proxy, NULL) ||
	    (res = sid_resource_search(current_res, SID_RESOURCE_SEARCH_ANC, &sid_resource_type_worker_proxy, NULL))) {
		/* sending from worker proxy to worker */
		worker_proxy = sid_resource_get_data(res);

		if (!(chan = _get_channel(worker_proxy->channels, worker_proxy->channel_count, channel_id)))
			return -ECHRNG;

		if (worker_proxy->idle_timeout_es)
			sid_resource_destroy_event_source(&worker_proxy->idle_timeout_es);
		if (worker_proxy->state != WORKER_STATE_ASSIGNED)
			_change_worker_proxy_state(res, WORKER_STATE_ASSIGNED);

		if (chan->spec->proxy_tx_cb.cb)
			if (chan->spec->proxy_tx_cb.cb(res, chan, data_spec, chan->spec->proxy_tx_cb.arg) < 0)
				log_warning(ID(current_res), "%s", _custom_message_handling_failed_msg);

	} else if ((res = sid_resource_search(current_res, SID_RESOURCE_SEARCH_TOP, &sid_resource_type_worker, NULL))) {
		/* sending from worker to worker proxy */
		worker = sid_resource_get_data(res);

		if (!(chan = _get_channel(worker->channels, worker->channel_count, channel_id)))
			return -ECHRNG;

		if (chan->spec->worker_tx_cb.cb)
			if (chan->spec->worker_tx_cb.cb(res, chan, data_spec, chan->spec->worker_tx_cb.arg) < 0)
				log_warning(ID(current_res), "%s", _custom_message_handling_failed_msg);

	} else
		return -ENOMEDIUM;

	return _chan_buf_send(chan, data_spec->ext.used ? WORKER_CHANNEL_CMD_DATA_EXT : WORKER_CHANNEL_CMD_DATA, data_spec);
}

int worker_control_worker_yield(sid_resource_t *res)
{
	sid_resource_t        *worker_res;
	struct worker         *worker;
	struct worker_channel *chan;
	unsigned               i;

	if (sid_resource_match(res, &sid_resource_type_worker, NULL))
		worker_res = res;
	else if (!(worker_res = sid_resource_search(res, SID_RESOURCE_SEARCH_ANC, &sid_resource_type_worker, NULL)))
		return -ENOMEDIUM;

	worker = sid_resource_get_data(worker_res);

	for (i = 0; i < worker->channel_count; i++) {
		chan = &worker->channels[i];
		if (chan->spec->wire.type == WORKER_WIRE_PIPE_TO_PROXY || chan->spec->wire.type == WORKER_WIRE_SOCKET) {
			if (worker->parent_exited == 0)
				return _chan_buf_send(chan, WORKER_CHANNEL_CMD_YIELD, NULL);
			else
				raise(SIGTERM);
		}
	}

	return -ENOTCONN;
}

static int _on_worker_proxy_child_event(sid_resource_event_source_t *es, const siginfo_t *si, void *data)
{
	sid_resource_t *worker_proxy_res = data;

	switch (si->si_code) {
		case CLD_EXITED:
			log_debug(ID(worker_proxy_res), "Worker exited with exit code %d.", si->si_status);
			break;
		case CLD_KILLED:
		case CLD_DUMPED:
			log_debug(ID(worker_proxy_res),
			          "Worker terminated by signal %d (%s).",
			          si->si_status,
			          strsignal(si->si_status));
			break;
		default:
			log_debug(ID(worker_proxy_res), "Worker failed unexpectedly.");
	}

	_change_worker_proxy_state(worker_proxy_res, WORKER_STATE_EXITED);

	/*
	 * NOTE: We have set lower priority for this _on_worker_proxy_child_event handler
	 * for us to be able to process all remaining events (e.g. data reception on channels)
	 * before we destroy worker_proxy_res here.
	 *
	 * The approach with setting lower priority for this handler has a downside. Since it
	 * also sets the worker proxy state, then if there are other remaining events with higher
	 * priority, the worke proxy state setting is delayed.
	 *
	 * If this appears as an issue in the future, setting the state and destroying the
	 * worker proxy needs to be separated.
	 */

	if (sid_resource_match(worker_proxy_res, &sid_resource_type_worker_proxy_with_ev_loop, NULL))
		sid_resource_exit_event_loop(worker_proxy_res);

	(void) sid_resource_unref(worker_proxy_res);
	return 0;
}

/*
static int _on_worker_proxy_idle_timeout_event(sid_resource_event_source_t *es, uint64_t usec, void *data)
{
        sid_resource_t *worker_proxy_res = data;

        log_debug(ID(worker_proxy_res), "Idle timeout expired.");
        return _make_worker_exit(worker_proxy_res);
}
*/

static int _on_worker_signal_event(sid_resource_event_source_t *es, const struct signalfd_siginfo *si, void *arg)
{
	sid_resource_t *res = arg;
	struct worker  *worker;

	log_debug(ID(res), "Received signal %d from %d.", si->ssi_signo, si->ssi_pid);

	switch (si->ssi_signo) {
		case SIGTERM:
		case SIGINT:
			sid_resource_exit_event_loop(res);
			break;
		case SIGUSR1:
			worker = sid_resource_get_data(res);
			if (worker != NULL)
				worker->parent_exited = 1;
			break;
		default:
			break;
	};

	return 0;
}

static int _on_worker_proxy_timeout_event(sid_resource_event_source_t *es, uint64_t usec, void *data)
{
	sid_resource_t      *worker_proxy_res = data;
	struct worker_proxy *worker_proxy     = sid_resource_get_data(worker_proxy_res);
	int                  signum;
	int                  r = 0;

	_change_worker_proxy_state(worker_proxy_res, WORKER_STATE_TIMED_OUT);

	if ((signum = worker_proxy->timeout_spec.signum)) {
		log_debug(ID(worker_proxy_res), "Sending signal %d (SIG%s)) to worker process.", signum, sigabbrev_np(signum));

		if ((r = kill(worker_proxy->pid, signum)) < 0)
			log_error_errno(ID(worker_proxy_res),
			                errno,
			                "Failed to send signal %d (SIG%s) to worker process.",
			                signum,
			                sigabbrev_np(signum));
	}

	return r;
}

static int _init_worker_proxy(sid_resource_t *worker_proxy_res, const void *kickstart_data, void **data)
{
	const struct worker_kickstart *kickstart    = kickstart_data;
	struct worker_proxy           *worker_proxy = NULL;

	if (!(worker_proxy = mem_zalloc(sizeof(*worker_proxy)))) {
		log_error(ID(worker_proxy_res), "Failed to allocate worker_proxy structure.");
		goto fail;
	}

	worker_proxy->pid           = kickstart->pid;
	worker_proxy->type          = kickstart->type;
	worker_proxy->state         = WORKER_STATE_NEW;
	worker_proxy->channels      = kickstart->channels;
	worker_proxy->channel_count = kickstart->channel_count;
	worker_proxy->timeout_spec  = kickstart->timeout_spec;
	worker_proxy->arg           = kickstart->arg;

	if (sid_resource_create_child_event_source(worker_proxy_res,
	                                           NULL,
	                                           worker_proxy->pid,
	                                           WEXITED,
	                                           _on_worker_proxy_child_event,
	                                           1,
	                                           "worker process monitor",
	                                           worker_proxy_res) < 0) {
		log_error(ID(worker_proxy_res), "Failed to register worker process monitoring in worker proxy.");
		goto fail;
	}

	if (_setup_channels(worker_proxy_res, NULL, kickstart->type, kickstart->channels, kickstart->channel_count) < 0)
		goto fail;

	if (kickstart->timeout_spec.usec && sid_resource_create_time_event_source(worker_proxy_res,
	                                                                          &worker_proxy->exec_timeout_es,
	                                                                          CLOCK_MONOTONIC,
	                                                                          SID_RESOURCE_POS_REL,
	                                                                          kickstart->timeout_spec.usec,
	                                                                          0,
	                                                                          _on_worker_proxy_timeout_event,
	                                                                          0,
	                                                                          "timeout",
	                                                                          worker_proxy_res) < 0) {
		log_error(ID(worker_proxy_res), "Failed to create timeout event.");
		goto fail;
	}

	*data = worker_proxy;
	return 0;
fail:
	free(worker_proxy);
	return -1;
}

static int _destroy_worker_proxy(sid_resource_t *worker_proxy_res)
{
	struct worker_proxy *worker_proxy = sid_resource_get_data(worker_proxy_res);

	_destroy_channels(worker_proxy->channels, worker_proxy->channel_count);
	free(worker_proxy);

	return 0;
}

static int _init_worker(sid_resource_t *worker_res, const void *kickstart_data, void **data)
{
	const struct worker_kickstart *kickstart = kickstart_data;
	struct worker                 *worker    = NULL;
	sigset_t                       mask;

	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGUSR1);

	if (!(worker = mem_zalloc(sizeof(*worker)))) {
		log_error(ID(worker_res), "Failed to allocate new worker structure.");
		goto fail;
	}

	worker->channel_specs = kickstart->channel_specs;
	worker->channels      = kickstart->channels;
	worker->channel_count = kickstart->channel_count;
	worker->parent_exited = 0;
	worker->arg           = kickstart->arg;

	if (sid_resource_create_signal_event_source(worker_res,
	                                            NULL,
	                                            mask,
	                                            _on_worker_signal_event,
	                                            0,
	                                            "worker_signal_handler",
	                                            worker_res) < 0) {
		log_error(ID(worker_res), "Failed to create signal handlers.");
		goto fail;
	}

	if (_setup_channels(worker_res, NULL, kickstart->type, kickstart->channels, kickstart->channel_count) < 0)
		goto fail;

	*data = worker;
	return 0;
fail:
	free(worker);
	return -1;
}

static int _destroy_worker(sid_resource_t *worker_res)
{
	struct worker *worker = sid_resource_get_data(worker_res);

	_destroy_channels(worker->channels, worker->channel_count);
	free(worker->channel_specs);
	free(worker);

	return 0;
}

static int _set_channel_specs(struct worker_control *worker_control, const struct worker_channel_spec *channel_specs)
{
	const struct worker_channel_spec *spec;
	unsigned                          spec_count;
	size_t                            ids_size;
	char                             *p;
	unsigned                          i;
	int                               r;

	for (spec = channel_specs, ids_size = 0, spec_count = 0; spec->wire.type != WORKER_WIRE_NONE; spec++) {
		if (!spec->id || !*spec->id) {
			r = -EINVAL;
			goto fail;
		}

		ids_size += strlen(spec->id) + 1; /* +1 to include '\0' at the end of each string! */
		spec_count++;
	}

	/* Allocate overall memory to keep deep copy of all specs, including each spec.id. */
	if (!(worker_control->channel_specs = malloc(spec_count * sizeof(struct worker_channel_spec) + ids_size))) {
		r = -ENOMEM;
		goto fail;
	}

	for (i = 0, p = (char *) worker_control->channel_specs + spec_count * sizeof(struct worker_channel_spec); i < spec_count;
	     i++) {
		worker_control->channel_specs[i]    = channel_specs[i];
		worker_control->channel_specs[i].id = p;
		p                                   = stpcpy(p, channel_specs[i].id) + 1;
	}

	worker_control->channel_spec_count = spec_count;
	return 0;
fail:
	worker_control->channel_specs = mem_freen(worker_control->channel_specs);
	return r;
}

static int _init_worker_control(sid_resource_t *worker_control_res, const void *kickstart_data, void **data)
{
	const struct worker_control_resource_params *params = kickstart_data;
	struct worker_control                       *worker_control;
	int                                          r;

	if (!(worker_control = mem_zalloc(sizeof(*worker_control)))) {
		log_error(ID(worker_control_res), "Failed to allocate memory for worker control structure.");
		goto fail;
	}

	if ((r = _set_channel_specs(worker_control, params->channel_specs)) < 0) {
		log_error_errno(ID(worker_control_res), r, "Failed to set channel specs while initializing worker control.");
		goto fail;
	}

	worker_control->worker_type  = params->worker_type;
	worker_control->init_cb_spec = params->init_cb_spec;
	worker_control->timeout_spec = params->timeout_spec;

	*data                        = worker_control;
	return 0;
fail:
	free(worker_control);
	return -1;
}

static int _destroy_worker_control(sid_resource_t *worker_control_res)
{
	struct worker_control *worker_control = sid_resource_get_data(worker_control_res);

	if (worker_control->worker_init.channels)
		_destroy_channels(worker_control->worker_init.channels, worker_control->channel_spec_count);
	free(worker_control->worker_init.id);
	free(worker_control->worker_init.ext_argv);
	free(worker_control->worker_init.ext_envp);
	free(worker_control->channel_specs);
	free(worker_control);
	return 0;
}

#define WORKER_PROXY_NAME       "worker-proxy"
#define WORKER_PROXY_SHORT_NAME "wrp"
#define WORKER_PROXY_DESCRIPTION                                                                                                   \
	"Resource under worker-control management providing worker representation "                                                \
	"on parent process side ('proxy') and containting communication endpoints "                                                \
	"for worker-proxy <--> worker channels."

const sid_resource_type_t sid_resource_type_worker_proxy = {
	.name        = WORKER_PROXY_NAME,
	.short_name  = WORKER_PROXY_SHORT_NAME,
	.description = WORKER_PROXY_DESCRIPTION,
	.init        = _init_worker_proxy,
	.destroy     = _destroy_worker_proxy,
};

const sid_resource_type_t sid_resource_type_worker_proxy_with_ev_loop = {
	.name            = WORKER_PROXY_NAME,
	.short_name      = WORKER_PROXY_SHORT_NAME,
	.description     = WORKER_PROXY_DESCRIPTION,
	.init            = _init_worker_proxy,
	.destroy         = _destroy_worker_proxy,
	.with_event_loop = 1,
};

const sid_resource_type_t sid_resource_type_worker = {
	.name            = "worker",
	.short_name      = "wrk",
	.description     = "Top-level resource in a worker process spawned by worker-control "
			   "resource and containting worker communication endpoints for "
			   "worker <--> worker-proxy channels.",
	.init            = _init_worker,
	.destroy         = _destroy_worker,
	.with_event_loop = 1,
};

const sid_resource_type_t sid_resource_type_worker_control = {
	.name        = "worker-control",
	.short_name  = "wcl",
	.description = "Resource providing capabilities to spawn worker processes "
		       "and setting up communication channels with workers.",
	.init        = _init_worker_control,
	.destroy     = _destroy_worker_control,
};
