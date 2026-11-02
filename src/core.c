#include "core.h"
#include <limits.h>
#include <math.h>

int banker_safe(const int available[RESOURCE_COUNT], const Job *jobs, size_t count, size_t *sequence) {
    if (count > MAX_ACTIVE || !available || (count && (!jobs || !sequence))) return -1;
    long long work[RESOURCE_COUNT];
    int finished[MAX_ACTIVE] = {0};
    for (size_t r = 0; r < RESOURCE_COUNT; r++) {
        if (available[r] < 0) return -1;
        work[r] = available[r];
    }
    for (size_t i = 0; i < count; i++)
        for (size_t r = 0; r < RESOURCE_COUNT; r++)
            if (jobs[i].maximum[r] < 0 || jobs[i].allocation[r] < 0 || jobs[i].allocation[r] > jobs[i].maximum[r]) return -1;
    size_t completed = 0;
    while (completed < count) {
        int progress = 0;
        for (size_t i = 0; i < count; i++) {
            if (finished[i]) continue;
            int possible = 1;
            for (size_t r = 0; r < RESOURCE_COUNT; r++)
                if (jobs[i].maximum[r] - jobs[i].allocation[r] > work[r]) possible = 0;
            if (!possible) continue;
            for (size_t r = 0; r < RESOURCE_COUNT; r++) work[r] += jobs[i].allocation[r];
            sequence[completed++] = i;
            finished[i] = 1;
            progress = 1;
        }
        if (!progress) return 0;
    }
    return 1;
}

int effective_priority(const Job *job, double now) {
    double minutes = floor(fmax(0, now - job->queued_at) / 60);
    if (minutes > INT_MAX - job->priority) return INT_MAX;
    return job->priority + (int)minutes;
}

void priority_order(const Job *jobs, size_t count, double now, size_t *order, size_t *length) {
    *length = 0;
    for (size_t i = 0; i < count; i++) {
        if (!jobs[i].eligible) continue;
        size_t pos = (*length)++;
        while (pos > 0) {
            const Job *prev = &jobs[order[pos-1]];
            int a = effective_priority(&jobs[i], now), b = effective_priority(prev, now);
            if (a < b || (a == b && (jobs[i].queued_at > prev->queued_at || (jobs[i].queued_at == prev->queued_at && jobs[i].id >= prev->id)))) break;
            order[pos] = order[pos-1];
            pos--;
        }
        order[pos] = i;
    }
}
