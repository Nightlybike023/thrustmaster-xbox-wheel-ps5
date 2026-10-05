#pragma once
#include <stdint.h>

/* Bind/listen happens before success is returned; -1 means no server started. */
int web_server_start(int port);
