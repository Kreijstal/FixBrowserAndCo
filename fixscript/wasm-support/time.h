#ifndef TIME_H
#define TIME_H

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 0

typedef long long time_t;
typedef int clockid_t;

struct timespec {
   time_t tv_sec;
   long int tv_nsec;
};

int clock_gettime(clockid_t clk_id, struct timespec *tp);

#endif /* TIME_H */
