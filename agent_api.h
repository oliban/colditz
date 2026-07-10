#pragma once
#include <stdint.h>
#include <stdbool.h>
#if !defined(WIN32) && !defined(PSP)
extern bool agent_api_enabled;
void agent_api_init(uint16_t port);
void agent_api_tick(void);
#else
#define agent_api_enabled false
#define agent_api_init(port)
#define agent_api_tick()
#endif
