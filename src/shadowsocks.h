/*
 * shadowsocks.h - Header files of library interfaces
 *
 * Copyright (C) 2013 - 2015, Max Lv <max.c.lv@gmail.com>
 *
 * This file is part of the shadowsocks-libev.
 * shadowsocks-libev is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * shadowsocks-libev is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with shadowsocks-libev; see the file COPYING. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef _SHADOWSOCKS_H
#define _SHADOWSOCKS_H

typedef struct {
    /*  Required  */
    char *remote_host;    // hostname or ip of remote server
    char *local_addr;     // local ip to bind
    char *method;         // encryption method
    char *password;       // password of remote server
    int remote_port;      // port number of remote server
    int local_port;       // port number of local server
    int timeout;          // connection timeout

    /*  Optional, set NULL if not valid   */
    char *acl;            // file path to acl
    char *log;            // file path to log
    int fast_open;        // enable tcp fast open
    int mode;        // enable udp relay
    int verbose;          // verbose mode

    // added by andy, server only
    char *dns_server_str; // for example "8.8.8.8"
    long tx_bytes;
    long rx_bytes;
} profile_t;

/* An example profile

   const profile_t EXAMPLE_PROFILE = {
    .remote_host = "example.com",
    .local_addr = "127.0.0.1",
    .method = "bf-cfb",
    .password = "barfoo!",
    .remote_port = 8338,
    .local_port = 1080,
    .timeout = 600;
    .acl = NULL,
    .log = NULL,
    .fast_open = 0,
    .mode = 0,
    .verbose = 0
   };
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Create and start a shadowsocks local server.
 *
 * Calling this function will block the current thread forever if the server
 * starts successfully.
 *
 * Make sure start the server in a seperate process to avoid any potential
 * memory and socket leak.
 *
 * If failed, -1 is returned. Errors will output to the log file.
 */
// int start_ss_local_server(profile_t profile);
int start_ss_remote_server(profile_t *profile);

#ifdef __cplusplus
}
#endif

// To stop the service on posix system, just kill the daemon process
// kill(pid, SIGKILL);

#endif // _SHADOWSOCKS_H
