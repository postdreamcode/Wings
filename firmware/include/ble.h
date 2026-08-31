#pragma once

#include <Arduino.h>

void bleBegin();
void bleService();
bool bleIsConnected();
void bleNotifyStatus();
