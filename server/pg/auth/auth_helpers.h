#pragma once

#include "server/pg/wire.h"

typedef enum
{
    PSQL_UnknownAuthCode = -1,
    PSQL_AuthenticationOk = 0,
    PSQL_AuthenticationCleartextPassword = 3,
    PSQL_AuthenticationMD5Password = 5,
    PSQL_AuthenticationSASL = 10,
    PSQL_AuthenticationSASLContinue = 11,
    PSQL_AuthenticationSASLFinal = 12
} AuthCode;

AuthCode parse_auth_response(DBResponse *db_response);
ByteBuffer *create_startup_message();