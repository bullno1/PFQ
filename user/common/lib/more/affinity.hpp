/***************************************************************
 *
 * (C) 2014 - Nicola Bonelli <nicola@pfq.io>
 *
 ****************************************************************/

#pragma once

#include <pthread.h> // pthread_setaffinity_np
#include <sched.h>

#include <thread>
#include <stdexcept>

namespace more {

    static inline
    void set_affinity(std::thread &t, size_t n)
    {
        if(t.get_id() == std::thread::id())
            throw std::runtime_error("thread not running");

        cpu_set_t cpuset;

        CPU_ZERO(&cpuset);
        CPU_SET(n, &cpuset);

        auto pth = t.native_handle();
        if ( ::pthread_setaffinity_np(pth, sizeof(cpuset), &cpuset) != 0)
            throw std::runtime_error("pthread_setaffinity_np");
    }

} // namespace more

