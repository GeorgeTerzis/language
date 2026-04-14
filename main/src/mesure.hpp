#pragma once

#include <chrono>


//mesures the execution time of function in nanoseconds
template<typename T,typename ...Args>
auto mesure(const T&& function,Args... args) {
  const auto start = std::chrono::high_resolution_clock::now();
  auto r = function(args...);
  const auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> duration = end - start;
  //heaptrack
  // std::cerr << stage  << ": " << duration.count() << "sec" << std::endl;
  return std::tuple{std::move(r),duration};
};
