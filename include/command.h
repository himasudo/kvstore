#pragma once
#include <string>
#include <vector> 

struct Command {
    enum class Type {SET, EXISTS, GET, DEL, KEYS, CLEAR, SIZE};
    Type type;
    std::vector<std::string> args;
};

