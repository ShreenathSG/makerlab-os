#ifndef MAKERLAB_CORE_H
#define MAKERLAB_CORE_H
#include <stddef.h>
#define RESOURCE_COUNT 3
#define MAX_ACTIVE 100

typedef struct {
    long long id;
    int priority;
    double queued_at;
    int maximum[RESOURCE_COUNT];
    int allocation[RESOURCE_COUNT];
    int eligible;
} Job;

/* Returns 1 for safe, 0 for unsafe, -1 for invalid input. Sequence stores indexes. */
int banker_safe(const int available[RESOURCE_COUNT], const Job *jobs, size_t count, size_t *sequence);
int effective_priority(const Job *job, double now);
void priority_order(const Job *jobs, size_t count, double now, size_t *order, size_t *length);
#endif
