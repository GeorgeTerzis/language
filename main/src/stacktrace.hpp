#pragma once

#include <sstream>
#include <boost/stacktrace.hpp>
#include <iostream>

inline void print_stacktrace() {
    const auto stack = boost::stacktrace::stacktrace();
    std::ostringstream oss;
    for (std::size_t i = 0; i < 10; ++i) {
      const auto& frame = stack[i];
      oss << "#" << i << ": " << frame.name() << "\n";
    }
    std::cerr << oss.str();
}
