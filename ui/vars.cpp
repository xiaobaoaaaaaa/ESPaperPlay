#include <string>
#include "vars.h"

std::string current_weekday;

extern "C" const char *get_var_current_weekday() {
    return current_weekday.c_str();
}

extern "C" void set_var_current_weekday(const char *value) {
    current_weekday = value;
}
