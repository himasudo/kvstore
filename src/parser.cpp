#include "parser.h"
#include <unordered_map>

static const std::unordered_map<std::string, Command::Type> command_map = {
    {"SET", Command::Type::SET},
    {"GET", Command::Type::GET},
    {"KEYS", Command::Type::KEYS},
    {"SIZE", Command::Type::SIZE},
    {"EXISTS", Command::Type::EXISTS},
    {"DEL", Command::Type::DEL},
    {"CLEAR", Command::Type::CLEAR},
};

std::expected<Command, std::string> parse(std::string_view input) {
    const char *c = input.data();

    if (*c != '*') {
        return std::unexpected("Error[Invalid Input]: input should always start with '*'");
    }
    const char *end = c + input.size();
    bool arg_count_flag = true;
    bool arg_size_flag = false;
    bool arg_flag = false;
    std::string argument;
    std::string command_type;   
    std::string arg_count_str;
    std::string arg_size_str;
    std::string command_size_str;
    int arg_count; // for storing how many arguments are there
    int arg_size = -1; // for storing the size of the argument
    int command_size = 0; // for storing the size of the command

    Command command;
    c++; // start from 1st element
    while (c < end) {
        if (arg_count_flag && *c== '\r' && (c + 1) < end && *(c + 1) == '\n') {
            if (arg_count_str.empty()) {
                return std::unexpected("Error[Invalid Input]: The input provided does not contain token size");
            }
            try {
                arg_count = std::stoi(arg_count_str);
            } catch (const std::exception&) {
                return std::unexpected("Error[Invalid Input]: Argument count out of range");
            }
            arg_count--;
            if (arg_count > MAX_ARG_COUNT) {
                return std::unexpected("Error[Invalid Input]: too many arguments");
            }
            arg_count_flag = false;
            c+=2; // consume both "/r/n" and break
            break;
        }
        else if (arg_count_flag) {
            if (std::isdigit(*c)) {
                arg_count_str += *c;
            } else {
                return std::unexpected("Error[Invalid Input]: The token size is expected to be a digit");
            }
        }

        c++;
    }
    if (arg_count_flag) {
        return std::unexpected("Error[Invalid Input]: All input tokens must be saparated with \\r\\n and end with \\r\\n");
    }
    if (c == end) {
        return std::unexpected("Error[Invalid Input]: Input cannot have no tokens");
    }
    if (*c != '$') {
        return std::unexpected("Error[Invalid Input]: Command size cannot be zero");
    }
    c++;

    while (c < end) {
        if (*c == '\r' && (c + 1) < end && *(c + 1) == '\n') {
            try {
                command_size = std::stoi(command_size_str);
            } catch (const std::exception&) {
                return std::unexpected("Error[Invalid Input]: Command count out of range");
            }
            if (static_cast<size_t>(command_size) > MAX_COMMAND_SIZE) {
                return std::unexpected("Error[Invalid Input]: The command provided exceeds maximum limit");
            }
            c += 2;
            break;
        } else if (std::isdigit(*c)) {
            command_size_str += *c;
        } else {
            return std::unexpected("Error[Invalid Input] The command size must be a digit");
        }
        c++;
    }
    if (command_size < 0) {
        return std::unexpected("Error[Invalid Input] Command size either not present or not a digit");
    }
    while (c < end) {
        if (command_size > 0) {
            command_type += *c;
            command_size--;
        } else if (command_size == 0 && *c == '\r' && (c + 1) < end && *(c + 1) == '\n') {
            if (command_type.empty()) {
                return std::unexpected("Error[Invalid Input]: Input cannot have no command");
            }
            auto it = command_map.find(command_type);
            if (it == command_map.end()) {
                return std::unexpected("Error[Invalid Input]: Invalid command in the input");
            }
            command.type = it->second;
            command.args.reserve(arg_count);
            c+=2;
            break;
        } else {
            return std::unexpected("Error[Invalid Input] Command size provided doesnt match with the actual command");
        }
        c++;
    }
    if (c == end) {
        return command;
    }

    while (c < end) {
        if (arg_size_flag && *c == '\r' && (c + 1) < end && *(c + 1) == '\n') {
            if (arg_size_str.empty()) {
                return std::unexpected("Error[Invalid Input]: Argument mentioned but not present");
            }
            try {
                arg_size = std::stoi(arg_size_str);
            } catch (const std::exception&) {
                return std::unexpected("Error[Invalid Input]: Argument size out of range");
            }
            if (static_cast<size_t>(arg_size) > MAX_VALUE_SIZE) {
                return std::unexpected("Error[Invalid Input]: The argument size provided exceeds maximum limit");
            }
            arg_size_str = "";
            arg_size_flag = false;
            arg_flag = true;
            c++;
        } else if (arg_size_flag) {
            if (!std::isdigit(*c)) {
                return std::unexpected("Error[Invalid Input]: Argument must be a digit");
            }
            arg_size_str += *c;
        } else if (arg_flag && arg_size > 0){
            argument += *c;
            arg_size --;
        } else if (arg_flag && arg_size == 0) {
            if(*c == '\r' && (c + 1) < end && *(c + 1) == '\n'){
                command.args.push_back(argument);
                argument = "";
                arg_count--;
                arg_flag = false;
                c++;
            } else {
                return std::unexpected("Error[Invalid Input]: Argument does not match with the one present in input.");
            }
        } else if (*c == '$') {
            arg_size_flag = true;
        } else {
            return std::unexpected("Error[Invalid Input]: Argument size must start with '$'");
        }
        c++;
    }

    if(arg_count > 0 || arg_count < 0) {
        return std::unexpected("Error[Invalid Input]: Argument count does not match with the value present in input");
    }

    return command;
};
