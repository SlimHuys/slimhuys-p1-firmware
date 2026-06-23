#pragma once
#include <Arduino.h>

// Ring-buffer logger — 80 regels van max 120 tekens.
// diagLog() schrijft naar de buffer én naar Serial.
// getDiagLog() geeft de buffer terug als platte tekst (oudste eerst).
// Bedoeld als vangnet voor diagnoseproblemen zonder seriële toegang.

void diagLog(const char* fmt, ...);
String getDiagLog();
