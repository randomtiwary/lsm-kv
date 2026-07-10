#pragma once

#include "lsmkv/status.h"

// Early-return helper for Status-returning functions.
//
//   RELDB_RETURN_NOT_OK(schema.Validate());
//   RELDB_RETURN_NOT_OK(st);
//
// Evaluates the expression once; on non-OK, returns that Status from the
// enclosing function.
#define RELDB_RETURN_NOT_OK(expr)                          \
    do {                                                   \
        const ::lsmkv::Status reldb_status_tmp = (expr);   \
        if (!reldb_status_tmp.ok()) return reldb_status_tmp; \
    } while (0)
