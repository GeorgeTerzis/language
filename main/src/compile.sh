#!/bin/bash
EXEC_NAME='main'
COMPLILER='clang++'
LLVM_CONF="$(llvm-config --libs)"

# -s -fvisibility=hidden -flto
FLAGS='-std=c++26 -Wno-c++20-extensions -Wno-c++23-extensions'
# FLAGS="$FLAGS -Og -ggdb3 -fno-omit-frame-pointer"
# FLAGS="$FLAGS -O0 -ggdb3 -fno-omit-frame-pointer "
FLAGS="$FLAGS -g0 -O3"
# FLAGS="$FLAGS -s -O3 -flto -g0"

# FLAGS="$FLAGS -S -emit-llvm"
# OLVL='-O0'
# LLVM_CONF='-lLLVM-19'
# LLVM_CONF=`llvm-config --cxxflags --ldflags --libs --system-libs` #no exceptions no unwinding tables --std=C++17

WORKING_DIR="$(pwd)"
LIBS_DIR="$(dirname "$WORKING_DIR")/libs"

# echo $LLVM_CONF
echo "Executable: $EXEC_NAME"
echo "$COMPLILER $OLVL $GLVL $FLAGS $LLVM_CONF"

front(){
  echo "Compiling the frontend"
    $COMPLILER $GLVL $OLVL $FLAGS --shared -o lexer.so ./frontend/lexer.cpp -fPIC &
    $COMPLILER $GLVL $OLVL $FLAGS --shared -o parser.so ./frontend/parser.cpp -fPIC &
  wait
}

front

echo "Compiling Main"
$COMPLILER  $GLVL $OLVL $FLAGS $LLVM_CONF  -o $EXEC_NAME ./main.cpp ./parser.so ./lexer.so   -lboost_program_options
