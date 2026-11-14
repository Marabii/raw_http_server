#pragma once

#include "server/pg/pg.h"

DBServerDetails *read_server_details(int db_fd);
