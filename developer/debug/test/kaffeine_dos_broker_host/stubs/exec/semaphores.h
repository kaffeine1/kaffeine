#pragma once
#include <pthread.h>
struct SignalSemaphore { pthread_mutex_t mutex; };
