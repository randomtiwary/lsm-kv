#pragma once

#include "lsmkv/status.h"

// Early-return helper for Status-returning functions.
//
//   RELDB_RETURN_NOT_OK(schema.Validate());
//   RELDB_RETURN_NOT_OK(st);
//
// Evaluates the expression once; on non-OK, returns that Status from the
// enclosing function.
#define RELDB_RETURN_NOT_OK(expr)                            \
    do {                                                     \
        const ::lsmkv::Status reldb_status_tmp = (expr);     \
        if (!reldb_status_tmp.ok()) return reldb_status_tmp; \
    } while (0)

// Return status_expr when cond is true.
//
//   RELDB_RETURN_IF(idx < 0, STATUS(InvalidArgument, "bad index"));
//   RELDB_RETURN_IF(empty, auto_txn.Complete(STATUS(InvalidArgument, "empty")));
#define RELDB_RETURN_IF(cond, status_expr) \
    do {                                   \
        if (cond) return (status_expr);    \
    } while (0)

// Common case: return STATUS(Code, msg) when cond is true.
//
//   RELDB_FAIL_IF(name.empty(), InvalidArgument, "empty name");
#define RELDB_FAIL_IF(cond, Code, msg) RELDB_RETURN_IF((cond), STATUS(Code, msg))

// Fail when the session is not inside a transaction (COMMIT / ABORT).
#define RELDB_FAIL_IF_NOT_IN_TRANSACTION(txn) \
    RELDB_FAIL_IF((txn) == nullptr, InvalidArgument, "not in a transaction")
