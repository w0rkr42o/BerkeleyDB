/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2002,2006 Oracle.  All rights reserved.
 *
 * $Id: EventType.java,v 1.6 2006/12/18 19:08:55 alanb Exp $
 */

package com.sleepycat.db;

import com.sleepycat.db.internal.DbConstants;

public final class EventType {

    public static final EventType PANIC =
        new EventType("PANIC", DbConstants.DB_EVENT_PANIC);

    public static final EventType REP_CLIENT =
        new EventType("REP_CLIENT", DbConstants.DB_EVENT_REP_CLIENT);

    public static final EventType REP_MASTER =
        new EventType("REP_MASTER", DbConstants.DB_EVENT_REP_MASTER);

    public static final EventType REP_NEW_MASTER =
        new EventType("REP_NEW_MASTER", DbConstants.DB_EVENT_REP_NEWMASTER);

    public static final EventType REP_PERM_FAILED =
        new EventType("REP_NEW_MASTER", DbConstants.DB_EVENT_REP_PERM_FAILED);

    public static final EventType REP_STARTUPDONE =
        new EventType("REP_STARTUPDONE", DbConstants.DB_EVENT_REP_STARTUPDONE);

    public static final EventType WRITE_FAILED =
        new EventType("WRITE_FAILED", DbConstants.DB_EVENT_WRITE_FAILED);

    /* package */
    public static EventType fromInt(int type) {
        switch(type) {
        case DbConstants.DB_EVENT_PANIC:
            return PANIC;
        case DbConstants.DB_EVENT_REP_CLIENT:
            return REP_CLIENT;
        case DbConstants.DB_EVENT_REP_MASTER:
            return REP_MASTER;
        case DbConstants.DB_EVENT_REP_NEWMASTER:
            return REP_NEW_MASTER;
        case DbConstants.DB_EVENT_REP_PERM_FAILED:
            return REP_PERM_FAILED;
        case DbConstants.DB_EVENT_REP_STARTUPDONE:
            return REP_STARTUPDONE;
        case DbConstants.DB_EVENT_WRITE_FAILED:
            return WRITE_FAILED;
        default:
            throw new IllegalArgumentException(
                "Unknown event type: " + type);
        }
    }

    private String statusName;
    private int id;

    private EventType(final String statusName, final int id) {
        this.statusName = statusName;
        this.id = id;
    }

    /* package */
    int getId() {
        return id;
    }

    public String toString() {
        return "EventType." + statusName;
    }
}

