/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2011 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <sys/poll.h>
#include <string.h>

#include "sd-login.h"
#include "util.h"
#include "strv.h"

int main(int argc, char* argv[]) {
        int r, k;
        uid_t u, u2;
        char *seat;
        char *session;
        char *state;
        char *session2;
        char *t;
        char **seats, **sessions;
        uid_t *uids;
        unsigned n;
        struct pollfd pollfd;
        sd_login_monitor *m;

        assert_se(sd_pid_get_session(0, &session) == 0);
        printf("session = %s\n", session);

        assert_se(sd_pid_get_owner_uid(0, &u2) == 0);
        printf("user = %lu\n", (unsigned long) u2);

        r = sd_uid_get_sessions(u2, false, &sessions);
        assert_se(r >= 0);
        assert_se(t = strv_join(sessions, ", "));
        strv_free(sessions);
        printf("sessions = %s\n", t);
        free(t);

        r = sd_uid_get_seats(u2, false, &seats);
        assert_se(r >= 0);
        assert_se(t = strv_join(seats, ", "));
        strv_free(seats);
        printf("seats = %s\n", t);
        free(t);

        r = sd_session_is_active(session);
        assert_se(r >= 0);
        printf("active = %s\n", yes_no(r));

        assert_se(sd_session_get_uid(session, &u) >= 0);
        printf("uid = %lu\n", (unsigned long) u);
        assert_se(u == u2);

        assert_se(sd_session_get_seat(session, &seat) >= 0);
        printf("seat = %s\n", seat);

        assert_se(sd_uid_get_state(u, &state) >= 0);
        printf("state = %s\n", state);

        assert_se(sd_uid_is_on_seat(u, 0, seat) > 0);

        k = sd_uid_is_on_seat(u, 1, seat);
        assert_se(k >= 0);
        assert_se(!!r == !!r);

        assert_se(sd_seat_get_active(seat, &session2, &u2) >= 0);
        printf("session2 = %s\n", session2);
        printf("uid2 = %lu\n", (unsigned long) u2);

        assert_se(sd_seat_get_sessions(seat, &sessions, &uids, &n) >= 0);
        assert_se(t = strv_join(sessions, ", "));
        strv_free(sessions);
        printf("sessions = %s\n", t);
        free(t);
        printf("uids =");
        for (k = 0; k < (int) n; k++)
                printf(" %lu", (unsigned long) uids[k]);
        printf("\n");
        free(uids);

        free(session);
        free(state);
        free(session2);
        free(seat);

        assert_se(sd_get_seats(&seats) >= 0);
        assert_se(t = strv_join(seats, ", "));
        strv_free(seats);
        printf("seats = %s\n", t);
        free(t);

        assert_se(sd_get_sessions(&sessions) >= 0);
        assert_se(t = strv_join(sessions, ", "));
        strv_free(sessions);
        printf("sessions = %s\n", t);
        free(t);

        r = sd_get_uids(&uids);
        assert_se(r >= 0);

        printf("uids =");
        for (k = 0; k < r; k++)
                printf(" %lu", (unsigned long) uids[k]);
        printf("\n");

        free(uids);

        r = sd_login_monitor_new("session", &m);
        assert_se(r >= 0);

        zero(pollfd);
        pollfd.fd = sd_login_monitor_get_fd(m);
        pollfd.events = POLLIN;

        for (n = 0; n < 5; n++) {
                r = poll(&pollfd, 1, -1);
                assert_se(r >= 0);

                sd_login_monitor_flush(m);
                printf("Wake!\n");
        }

        sd_login_monitor_unref(m);

        return 0;
}