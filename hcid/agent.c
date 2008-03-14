/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2008  Nokia Corporation
 *  Copyright (C) 2004-2008  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/sdp.h>

#include <glib.h>

#include <dbus/dbus.h>

#include "dbus.h"
#include "dbus-helper.h"
#include "hcid.h"
#include "dbus-common.h"
#include "dbus-service.h"
#include "dbus-error.h"
#include "error.h"
#include "adapter.h"
#include "dbus-hci.h"
#include "agent.h"

#define REQUEST_TIMEOUT (60 * 1000)		/* 60 seconds */
#define AGENT_TIMEOUT (10 * 60 * 1000)		/* 10 minutes */

typedef enum {
	AGENT_REQUEST_PASSKEY,
	AGENT_REQUEST_AUTHORIZE,
	AGENT_REQUEST_CONFIRM_MODE
} agent_request_type_t;

struct agent {
	struct adapter *adapter;
	char *addr;
	char *name;
	char *path;
	struct agent_request *request;
	int exited;
	guint timeout;
	agent_remove_cb remove_cb;
	void *remove_cb_data;
};

struct agent_request {
	agent_request_type_t type;
	struct agent *agent;
	char *device;
	char *uuid;
	char remote_address[18];
	DBusPendingCall *call;
	void *cb;
	void *user_data;
};

static DBusConnection *connection = NULL;

static void send_cancel_request(struct agent_request *req);

static void agent_free(struct agent *agent)
{
	if (!agent)
		return;

	if (agent->request) {
		DBusError err;

		if (agent->request->type == AGENT_REQUEST_PASSKEY) {
			agent_passkey_cb cb = agent->request->cb;
			cb(agent, &err, NULL, agent->request->user_data);
		} else  {
			agent_cb cb = agent->request->cb;
			cb(agent, &err, agent->request->user_data);
		}

		send_cancel_request(agent->request);
	}

	if (agent->timeout)
		g_source_remove(agent->timeout);

	if (!agent->exited)
		agent_release(agent);

	g_free(agent->name);
	g_free(agent->path);
	g_free(agent->addr);

	g_free(agent);
}

static gboolean agent_timeout(struct agent *agent)
{
	debug("Agent at %s, %s timed out", agent->name, agent->path);

	agent->timeout = 0;

	if (agent->remove_cb)
		agent->remove_cb(agent, agent->remove_cb_data);

	agent_free(agent);

	return FALSE;
}

struct agent *agent_create(const char *name, const char *path,
				const char *address,
				agent_remove_cb cb, void *remove_cb_data)
{
	struct agent *agent;

	agent = g_new0(struct agent, 1);

	agent->name = g_strdup(name);
	agent->path = g_strdup(path);
	agent->remove_cb = cb;
	agent->remove_cb_data = remove_cb_data;

	if (address) {
		agent->addr = g_strdup(address);
		agent->timeout = g_timeout_add(AGENT_TIMEOUT,
						(GSourceFunc) agent_timeout, agent);
	}

	return agent;
}

int agent_destroy(struct agent *agent, gboolean exited)
{
	agent->exited = exited;
	agent_free(agent);
	return 0;
}

static struct agent_request *agent_request_new(struct agent *agent,
						const char *device,
						void *cb,
						void *user_data)
{
	struct agent_request *req;

	req = g_new0(struct agent_request, 1);

	req->agent = agent;
	req->device = g_strdup(device);
	req->cb = cb;
	req->user_data = user_data;

	return req;
}

static void agent_request_free(struct agent_request *req)
{
	g_free(req->device);
	g_free(req->uuid);
	if (req->call)
		dbus_pending_call_unref(req->call);
	g_free(req);
}

static void agent_call_cancel(struct agent_request *req)
{
	struct agent *agent = req->agent;
	DBusMessage *message;

	message = dbus_message_new_method_call(agent->name, agent->path,
						"org.bluez.Agent", "Cancel");
	if (!message) {
		error("Couldn't allocate D-Bus message");
		return;
	}

	dbus_message_set_no_reply(message, TRUE);
	send_message_and_unref(connection, message);
}

static void authorize_reply(DBusPendingCall *call, void *data)
{
	struct agent_request *req = data;
	struct agent *agent = req->agent;
	agent_cb cb = req->cb;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusError err;

	debug("authorize reply");

	dbus_error_init(&err);
	if (dbus_set_error_from_message(&err, reply)) {
		if (dbus_error_has_name(&err, DBUS_ERROR_NO_REPLY))
			agent_call_cancel(req);
		error("Authorization agent replied with an error: %s, %s",
				err.name, err.message);
		cb(agent, &err, req->user_data);
		dbus_error_free(&err);
		goto done;
	}

	dbus_error_init(&err);
	if (!dbus_message_get_args(reply, &err,	DBUS_TYPE_INVALID)) {
		error("Wrong authorization agent reply signature: %s",
			err.message);
		cb(agent, &err, req->user_data);
		dbus_error_free(&err);
		goto done;
	}

	debug("successfull reply was sent");

	cb(agent, NULL, req->user_data);

done:
	dbus_message_unref(reply);

	agent_request_free(req);
	agent->request = NULL;

	debug("auth_agent_reply: returning");
}

static DBusPendingCall *agent_call_authorize(struct agent *agent,
						const char *device_path,
						const char *uuid)
{
	DBusMessage *message;
	DBusPendingCall *call;

	message = dbus_message_new_method_call(agent->name, agent->path,
				"org.bluez.Agent", "Authorize");
	if (!message) {
		error("Couldn't allocate D-Bus message");
		return NULL;
	}

	dbus_message_append_args(message,
				DBUS_TYPE_OBJECT_PATH, &device_path,
				DBUS_TYPE_OBJECT_PATH, &uuid,
				DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(connection, message,
					&call, REQUEST_TIMEOUT) == FALSE) {
		error("D-Bus send failed");
		dbus_message_unref(message);
		return NULL;
	}

	dbus_message_unref(message);
	return call;
}

int agent_authorize(struct agent *agent,
			const char *device,
			const char *uuid,
			agent_cb cb,
			void *user_data)
{
	struct agent_request *req;

	debug("agent_authorize");

	req = agent_request_new(agent, device, cb, user_data);

	req->call = agent_call_authorize(agent, device, uuid);
	if (!req->call) {
		agent_request_free(req);
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}

	dbus_pending_call_set_notify(req->call, authorize_reply, req,
					NULL);
	agent->request = req;

	debug("authorize request was sent for %s", device);

	return 0;
}

static DBusPendingCall *passkey_request_new(const char *device,
						struct agent *agent,
						dbus_bool_t numeric)
{
	DBusMessage *message;
	DBusPendingCall *call;

	message = dbus_message_new_method_call(agent->name, agent->path,
					"org.bluez.Agent", "PasskeyRequest");
	if (message == NULL) {
		error("Couldn't allocate D-Bus message");
		return NULL;
	}

	dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &device,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(connection, message,
					&call, REQUEST_TIMEOUT) == FALSE) {
		error("D-Bus send failed");
		dbus_message_unref(message);
		return NULL;
	}

	dbus_message_unref(message);
	return call;
}

static void passkey_reply(DBusPendingCall *call, void *user_data)
{
	struct agent_request *req = user_data;
	struct agent *agent = req->agent;
	struct adapter *adapter = agent->adapter;
	agent_passkey_cb cb = req->cb;
	DBusMessage *message;
	DBusError err;
	bdaddr_t sba;
	size_t len;
	char *pin;

	/* steal_reply will always return non-NULL since the callback
	 * is only called after a reply has been received */
	message = dbus_pending_call_steal_reply(call);

	dbus_error_init(&err);
	if (dbus_set_error_from_message(&err, message)) {
		error("Agent replied with an error: %s, %s",
				err.name, err.message);

		cb(agent, &err, NULL, req->user_data);
		dbus_error_free(&err);
		goto done;
	}

	dbus_error_init(&err);
	if (!dbus_message_get_args(message, &err,
				DBUS_TYPE_STRING, &pin,
				DBUS_TYPE_INVALID)) {
		error("Wrong passkey reply signature: %s", err.message);
		cb(agent, &err, NULL, req->user_data);
		dbus_error_free(&err);
		goto done;
	}

	len = strlen(pin);

	dbus_error_init(&err);
	if (len > 16 || len < 1) {
		error("Invalid passkey length from handler");
		dbus_set_error_const(&err, "org.bluez.Error.InvalidArgs",
					"Invalid passkey length");
		cb(agent, &err, NULL, req->user_data);
		dbus_error_free(&err);
		goto done;
	}

	str2ba(adapter->address, &sba);

	set_pin_length(&sba, len);

	cb(agent, NULL, pin, req->user_data);

done:
	if (message)
		dbus_message_unref(message);

	dbus_pending_call_cancel(req->call);
	if (req->call)
		dbus_pending_call_unref(req->call);
	g_free(req);

	if (agent->addr) {
		if (agent->remove_cb)
			agent->remove_cb(agent, agent->remove_cb_data);
		agent_free(agent);
	}
}

int agent_request_passkey(struct agent *agent, const char *device,
				agent_passkey_cb cb, void *user_data)
{
	struct agent_request *req;

	req = agent_request_new(agent, device, cb, user_data);

	req->call = passkey_request_new(device, agent, FALSE);
	if (!req->call)
		goto failed;

	dbus_pending_call_set_notify(req->call, passkey_reply, req, NULL);

	agent->request = req;

	return 0;

failed:
	g_free(req);
	return -1;
}

static DBusPendingCall *confirm_request_new(struct agent *agent,
						const char *device,
						const char *value)
{
	DBusMessage *message;
	DBusPendingCall *call;

	message = dbus_message_new_method_call(agent->name, agent->path,
					"org.bluez.Agent", "Confirm");
	if (message == NULL) {
		error("Couldn't allocate D-Bus message");
		return NULL;
	}

	dbus_message_append_args(message,
				DBUS_TYPE_OBJECT_PATH, &device,
				DBUS_TYPE_STRING, &value,
				DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(connection, message,
					&call, REQUEST_TIMEOUT) == FALSE) {
		error("D-Bus send failed");
		dbus_message_unref(message);
		return NULL;
	}

	dbus_message_unref(message);

	return call;
}

static void confirm_reply(DBusPendingCall *call, void *user_data)
{
	struct agent_request *req = user_data;
	struct agent *agent = req->agent;
	DBusMessage *message;
	DBusError err;
	agent_cb cb = req->cb;

	/* steal_reply will always return non-NULL since the callback
	 * is only called after a reply has been received */
	message = dbus_pending_call_steal_reply(call);

	dbus_error_init(&err);
	if (dbus_set_error_from_message(&err, message)) {

		error("Agent replied with an error: %s, %s",
				err.name, err.message);

		cb(agent, &err, req->user_data);
		dbus_error_free(&err);
		goto done;
	}

	dbus_error_init(&err);
	if (!dbus_message_get_args(message, &err, DBUS_TYPE_INVALID)) {
		error("Wrong confirm reply signature: %s", err.message);
		cb(agent, &err, req->user_data);
		dbus_error_free(&err);
		goto done;
	}

	cb(agent, NULL, req->user_data);
done:
	if (message)
		dbus_message_unref(message);

	agent->request = NULL;
	dbus_pending_call_cancel(req->call);
	if (req->call)
		dbus_pending_call_unref(req->call);
	g_free(req);

	if (agent->addr) {
		if (agent->remove_cb)
			agent->remove_cb(agent, agent->remove_cb_data);
		agent_free(agent);
	}
}

int agent_confirm(struct agent *agent, const char *device, const char *pin,
			agent_cb cb, void *user_data)
{
	struct agent_request *req;

	debug("Calling Agent.Confirm: name=%s, path=%s",
						agent->name, agent->path);

	req = agent_request_new(agent, device, cb, user_data);

	req->call = confirm_request_new(agent, device, pin);
	if (!req->call)
		goto failed;

	dbus_pending_call_set_notify(req->call, confirm_reply, req, NULL);

	agent->request = req;

	return 0;

failed:
	agent_request_free(req);
	return -1;
}

static void send_cancel_request(struct agent_request *req)
{
	DBusMessage *message;

	message = dbus_message_new_method_call(req->agent->name, req->agent->path,
						"org.bluez.Agent", "Cancel");
	if (message == NULL) {
		error("Couldn't allocate D-Bus message");
		return;
	}

	dbus_message_set_no_reply(message, TRUE);

	send_message_and_unref(connection, message);

	dbus_pending_call_cancel(req->call);
	agent_request_free(req);
}

void agent_release(struct agent *agent)
{
	DBusMessage *message;

	debug("Releasing agent %s, %s", agent->name, agent->path);

	message = dbus_message_new_method_call(agent->name, agent->path,
			"org.bluez.Agent", "Release");
	if (message == NULL) {
		error("Couldn't allocate D-Bus message");
		return;
	}

	dbus_message_set_no_reply(message, TRUE);

	send_message_and_unref(connection, message);
}

gboolean agent_matches(struct agent *agent, const char *name, const char *path)
{
	if (g_str_equal(agent->name, name) && g_str_equal(agent->path, path))
		return TRUE;

	return FALSE;
}

void agent_exit(void)
{
	dbus_connection_unref(connection);
}

void agent_init(void)
{
	connection = dbus_bus_get(DBUS_BUS_SYSTEM, NULL);
}
