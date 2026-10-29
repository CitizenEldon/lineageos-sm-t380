package st.barnes.t380.displayschedule;

import java.time.Instant;
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.time.LocalTime;
import java.time.ZoneId;
import java.time.ZoneOffset;
import java.time.zone.ZoneRules;
import java.util.List;

/** Daily civil-time interval. Does not assume a fixed 24-hour duration. */
public final class DailySchedule {
    public final LocalTime on;
    public final LocalTime off;

    public DailySchedule(LocalTime on, LocalTime off) {
        if (on.equals(off)) throw new IllegalArgumentException("On and off must differ");
        if (on.getSecond() != 0 || off.getSecond() != 0
                || on.getNano() != 0 || off.getNano() != 0) {
            throw new IllegalArgumentException("Use whole minutes");
        }
        this.on = on;
        this.off = off;
    }

    public static final class Decision {
        public final boolean visible;
        public final Instant nextBoundary;
        Decision(boolean visible, Instant nextBoundary) {
            this.visible = visible;
            this.nextBoundary = nextBoundary;
        }
    }

    // Missing civil times run at the first valid instant after the clock gap.
    // Repeated civil times run once, at their first occurrence.
    private static Instant resolve(LocalDate date, LocalTime time, ZoneId zone) {
        LocalDateTime local = date.atTime(time);
        ZoneRules rules = zone.getRules();
        List<ZoneOffset> offsets = rules.getValidOffsets(local);
        if (offsets.isEmpty()) return rules.getTransition(local).getInstant();
        return local.toInstant(offsets.get(0));
    }

    public Decision decide(Instant now, ZoneId zone) {
        LocalDate today = now.atZone(zone).toLocalDate();
        boolean visible = false;
        Instant next = null;
        // Yesterday catches intervals crossing midnight. Future days cover DST
        // gaps that collapse a short interval and exceptional skipped dates.
        for (int delta = -2; delta <= 4; delta++) {
            LocalDate date = today.plusDays(delta);
            Instant start = resolve(date, on, zone);
            Instant end = resolve(off.isAfter(on) ? date : date.plusDays(1), off, zone);
            if (!end.isAfter(start)) continue;
            visible |= !now.isBefore(start) && now.isBefore(end);
            if (start.isAfter(now) && (next == null || start.isBefore(next))) next = start;
            if (end.isAfter(now) && (next == null || end.isBefore(next))) next = end;
        }
        if (next == null) throw new IllegalStateException("No future daily boundary");
        return new Decision(visible, next);
    }
}
