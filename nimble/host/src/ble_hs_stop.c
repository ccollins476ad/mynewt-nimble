/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <assert.h>
#include "os/mynewt.h"
#include "ble_hs_priv.h"

static ble_npl_event_fn ble_hs_stop_term_event_cb;
static struct ble_npl_event ble_hs_stop_term_ev;

static struct ble_gap_event_listener ble_hs_stop_gap_listener;

/**
 * List of stop listeners.  These are notified when a stop procedure completes.
 */
static SLIST_HEAD(, ble_hs_stop_listener) ble_hs_stop_listener_list;

/**
 * Called when a stop procedure has completed.
 */
static void
ble_hs_stop_done(int status)
{
    struct ble_hs_stop_listener *listener;

    ble_hs_lock();

    ble_gap_event_listener_unregister(&ble_hs_stop_gap_listener);

    SLIST_FOREACH(listener, &ble_hs_stop_listener_list, link) {
        listener->fn(status, listener->arg);
    }

    ble_hs_enabled_state = BLE_HS_ENABLED_STATE_OFF;

    ble_hs_unlock();
}

/**
 * Terminates the first open connection.
 *
 * If there are no open connections, signals completion of the close procedure.
 */
static void
ble_hs_stop_terminate_next_conn(void)
{
    uint16_t handle;
    int rc;

    handle = ble_hs_atomic_first_conn_handle();
    if (handle == BLE_HS_CONN_HANDLE_NONE) {
        /* No open connections.  Signal completion of the stop procedure. */
        ble_hs_stop_done(0);
        return;
    }

    rc = ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc == 0) {
        /* Terminate procedure successfully initiated.  Let the GAP event
         * handler deal with the result.
         */
    } else {
        BLE_HS_LOG(ERROR,
            "ble_hs_stop: failed to terminate connection; rc=%d\n", rc);
        ble_hs_stop_done(rc);
    }
}

/**
 * Event handler.  Attempts to terminate the first open connection if there is
 * one.  All additional connections are terminated elsewhere in the GAP event
 * handler.
 *
 * If there are no connections, signals completion of the stop procedure.
 */
static void
ble_hs_stop_term_event_cb(struct ble_npl_event *ev)
{
    ble_hs_stop_terminate_next_conn();
}

/**
 * GAP event callback.  Listens for connection termination and then terminates
 * the next one.
 *
 * If there are no connections, signals completion of the stop procedure.
 */
static int
ble_hs_stop_gap_event(struct ble_gap_event *event, void *arg)
{
    /* Only process connection termination events. */
    if (event->type == BLE_GAP_EVENT_DISCONNECT ||
        event->type == BLE_GAP_EVENT_TERM_FAILURE) {

        ble_hs_stop_terminate_next_conn();
    }

    return 0;
}

/**
 * Registers a listener to listen for completion of the current stop procedure.
 */
static void
ble_hs_stop_register_listener(struct ble_hs_stop_listener *listener,
                              ble_hs_stop_fn *fn, void *arg)
{
    BLE_HS_DBG_ASSERT(fn != NULL);

    listener->fn = fn;
    listener->arg = arg;
    SLIST_INSERT_HEAD(&ble_hs_stop_listener_list, listener, link);
}

static int
ble_hs_stop_nolock(struct ble_hs_stop_listener *listener,
                   ble_hs_stop_fn *fn, void *arg)
{
    int rc;

    switch (ble_hs_enabled_state) {
    case BLE_HS_ENABLED_STATE_ON:
        /* Host is enabled; proceed with the stop procedure. */
        break;

    case BLE_HS_ENABLED_STATE_STOPPING:
        /* A stop procedure is already in progress.  Just listen for the
         * procedure's completion.
         */
        ble_hs_stop_register_listener(listener, fn, arg);
        return BLE_HS_EBUSY;

    case BLE_HS_ENABLED_STATE_OFF:
        /* Host already stopped. */
        return BLE_HS_EALREADY;

    default:
        assert(0);
        return BLE_HS_EUNKNOWN;
    }

    /* Abort all active GAP procedures. */
    ble_gap_abort_all(false);

    /* Register a GAP event listener to listen for connection terminations. */
    rc = ble_gap_event_listener_register(&ble_hs_stop_gap_listener,
                                         ble_hs_stop_gap_event, NULL);
    if (rc != 0) {
        return rc;
    }

    /* Register the provided listener to listen for completion of the stop
     * procedure.
     */
    ble_hs_stop_register_listener(listener, fn, arg);

    ble_hs_enabled_state = BLE_HS_ENABLED_STATE_STOPPING;

    /* Schedule termination of all open connections in the host task.  This is
     * done even if there are no open connections so that the result of the
     * stop procedure is signaled in a consistent manner (asynchronously).
     */
    ble_npl_eventq_put(ble_hs_evq_get(), &ble_hs_stop_term_ev);

    return 0;
}

int
ble_hs_stop(struct ble_hs_stop_listener *listener, 
            ble_hs_stop_fn *fn, void *arg)
{
    int rc;

    ble_hs_lock();
    rc = ble_hs_stop_nolock(listener, fn, arg);
    ble_hs_unlock();

    return rc;
}

void
ble_hs_stop_init(void)
{
    ble_npl_event_init(&ble_hs_stop_term_ev, ble_hs_stop_term_event_cb, NULL);
}
