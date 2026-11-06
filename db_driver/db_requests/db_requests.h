#pragma once

#include "db_driver/db_driver.h"

// Sends Terminate ('X'), closes the socket and frees the connection.
void close_connection(DBServerDetails *conn);
