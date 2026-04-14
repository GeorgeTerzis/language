#pragma once
#ifndef become
#ifdef __clang__
    #define become [[clang::musttail]] return
#else
    #define become return
#endif
#endif
