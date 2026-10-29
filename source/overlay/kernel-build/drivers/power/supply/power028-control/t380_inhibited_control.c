/* SPDX-License-Identifier: GPL-2.0-only */
#include "t380_inhibited_control.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

static int ic_read(const struct t380_ic_io *io, int address,
                   struct t380_ic_result *r)
{
    int ret = io->guard(io->context);
    if (ret)
        return ret;
    ++r->reads;
    ret = io->read(io->context, address);
    return ret > 255 ? -EIO : ret;
}

int t380_ic_apply(const struct t380_ic_io *io, const struct t380_cs_plan *plan,
                  struct t380_ic_result *result)
{
    struct t380_ic_result r = { 0 };
    int merged[T380_CS_COUNT], dummy[T380_CS_COUNT] = { 0 };
    int i, ret;

    if (!result)
        return -EINVAL;
    r.status_before = r.status_after = -ENODATA;
    for (i = 0; i < T380_CS_COUNT; ++i)
        r.before[i] = r.after[i] = -ENODATA;
    *result = r;
    if (!io || !io->off || !io->guard || !io->read || !io->write)
        return result->error = -EINVAL;
    /* Even an invalid plan must take the independent physical-off path. */
    ret = io->off(io->context);
    if (ret) {
        r.off_error = ret;
        goto done;
    }
    /* Merge validates the exact encoder provenance/address/mask/value. */
    ret = t380_cs_merge(plan, dummy, merged);
    if (ret)
        goto done;
    r.status_before = ic_read(io, 0x0a, &r);
    if (r.status_before < 0 || (r.status_before & 1)) {
        ret = r.status_before < 0 ? r.status_before : -EIO;
        goto done;
    }
    for (i = 0; i < T380_CS_COUNT; ++i) {
        r.before[i] = ic_read(io, plan->fields[i].address, &r);
        if (r.before[i] < 0) {
            ret = r.before[i];
            goto done;
        }
    }
    ret = t380_cs_merge(plan, r.before, merged);
    if (ret)
        goto done;
    /* Exact logical field order while HIGH, never CHGCNTL1 or CNTL mode.
     * The mutex excludes software readers/writers, not silicon AUTOSET/AICL. */
    for (i = 0; i < T380_CS_COUNT; ++i) {
        ret = io->guard(io->context);
        if (ret)
            goto done;
        ++r.writes;
        ret = io->write(io->context, plan->fields[i].address, merged[i]);
        if (ret)
            goto done;
    }
    for (i = 0; i < T380_CS_COUNT; ++i) {
        r.after[i] = ic_read(io, plan->fields[i].address, &r);
        if (r.after[i] < 0) {
            ret = r.after[i];
            goto done;
        }
    }
    ret = t380_cs_verify(plan, r.after, &r.verification);
    if (ret)
        goto done;
    /* A full-byte readback also detects unexpected unmasked changes. */
    for (i = 0; i < T380_CS_COUNT; ++i)
        if (r.after[i] != merged[i]) {
            ret = -EIO;
            goto done;
        }
    r.status_after = ic_read(io, 0x0a, &r);
    if (r.status_after < 0 || (r.status_after & 1)) {
        ret = r.status_after < 0 ? r.status_after : -EIO;
        goto done;
    }
    ret = io->guard(io->context);
done:
    r.error = ret;
    r.off_error = io->off(io->context);
    if (!r.error && r.off_error)
        r.error = r.off_error;
    r.verified = !r.error && r.verification.valid;
    /* Numeric/readback validity never authorizes GPIO LOW. */
    r.permission = 0;
    *result = r;
    return r.error;
}
