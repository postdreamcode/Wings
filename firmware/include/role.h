#pragma once

#include <Arduino.h>

enum BoardRole : uint8_t {
  ROLE_SLAVE  = 0,
  ROLE_MASTER = 1
};

void roleBegin();
BoardRole roleGet();
bool roleIsMaster();
const char* roleName();
