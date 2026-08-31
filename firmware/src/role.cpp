#include "role.h"
#include "Config.h"

static BoardRole g_role = ROLE_MASTER;

void roleBegin() {
  pinMode(PIN_ROLE, INPUT_PULLUP);
  delay(5);
  g_role = (digitalRead(PIN_ROLE) == HIGH) ? ROLE_MASTER : ROLE_SLAVE;
}

BoardRole roleGet() { return g_role; }
bool roleIsMaster() { return g_role == ROLE_MASTER; }
const char* roleName() { return roleIsMaster() ? "MASTER" : "SLAVE"; }
