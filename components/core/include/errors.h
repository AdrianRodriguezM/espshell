/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ESPSHELL_ERRORS_H
#define ESPSHELL_ERRORS_H

/* Wire-level error codes — these go out on the line as `ERR <code> <msg>`.
 * Numbers are frozen: do not renumber, only append. */
enum espshell_err {
    ESPSHELL_E_UNKNOWN_CMD = 1,
    ESPSHELL_E_BAD_ARGS    = 2,
    ESPSHELL_E_NOT_AUTH    = 3,
    ESPSHELL_E_HW_FAIL     = 4,
    ESPSHELL_E_NO_MEM      = 5,
    ESPSHELL_E_TIMEOUT     = 6,
    ESPSHELL_E_NOT_FOUND   = 7,
    ESPSHELL_E_BUSY        = 8,
    ESPSHELL_E_INTERNAL    = 9,
    ESPSHELL_E_NOT_IMPL    = 10,
    ESPSHELL_E_DISABLED    = 11,
};

#endif /* ESPSHELL_ERRORS_H */
