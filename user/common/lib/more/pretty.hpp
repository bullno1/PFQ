/***************************************************************
 *
 * (C) 2014 - Nicola Bonelli <nicola@pfq.io>
 *
 ****************************************************************/

#pragma once

#include <chrono>
#include <string>
#include <sstream>

namespace more {

    namespace detail {

        template <typename T>
        std::string to_string(std::ostringstream &out, T &&arg)
        {
            out << std::move(arg);
            return out.str();
        }
        template <typename T, typename ...Ts>
        std::string to_string(std::ostringstream &out, T &&arg, Ts&&... args)
        {
            out << std::move(arg);
            return to_string(out, std::forward<Ts>(args)...);
        }
    }

    template <typename U, typename T, typename Duration>
    U persecond(T value, Duration dur)
    {
        return static_cast<U>(value) * 1000000 /
            std::chrono::duration_cast<std::chrono::microseconds>(dur).count();
    }

    template <typename ...Ts>
    inline std::string
    to_string(Ts&& ... args)
    {
        std::ostringstream out;
        return detail::to_string(out, std::forward<Ts>(args)...);
    }

    template <typename T>
    std::string
    pretty_number(T value)
    {
        if (value < 1000000000) {
        if (value < 1000000) {
        if (value < 1000) {
             return to_string(value);
        }
        else return to_string(value/1000, "_K");
        }
        else return to_string(value/1000000, "_M");
        }
        else return to_string(value/1000000000, "_G");
    }

} // namespace more

