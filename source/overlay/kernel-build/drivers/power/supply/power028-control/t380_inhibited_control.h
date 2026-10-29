/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_INHIBITED_CONTROL_H
#define T380_INHIBITED_CONTROL_H
#include "t380_charger_settings.h"

/* Caller holds the charger's sleeping mutex for the complete transaction.
 * This interface has only a physical HIGH/off operation. No enable API. */
struct t380_ic_io {
    void *context;
    int (*off)(void *context);
    int (*guard)(void *context);
    int (*read)(void *context, int address);
    int (*write)(void *context, int address, int value);
};
struct t380_ic_result {
    int error, off_error, verified, permission;
    int reads, writes, status_before, status_after;
    int before[T380_CS_COUNT], after[T380_CS_COUNT];
    struct t380_cs_verification verification;
};
/* Apply only the accepted encoder's exact fields, while inhibited. Failure
 * can leave a partial configuration; no rollback or automatic retry. */
int t380_ic_apply(const struct t380_ic_io *io, const struct t380_cs_plan *plan,
                  struct t380_ic_result *result);
#endif
