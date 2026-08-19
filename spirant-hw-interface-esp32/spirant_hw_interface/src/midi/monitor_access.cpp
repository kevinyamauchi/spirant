#include "monitor_access.h"

#include <Arduino.h>

namespace monitor
{
namespace
{

MonitorState      g_state;
SemaphoreHandle_t g_mutex = nullptr;

}  // namespace

bool init()
{
    if (g_mutex == nullptr) g_mutex = xSemaphoreCreateMutex();
    return g_mutex != nullptr;
}

MonitorState* beginWrite(uint32_t timeout_ms)
{
    if (g_mutex == nullptr) return nullptr;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return nullptr;
    return &g_state;
}

void endWrite()
{
    if (g_mutex != nullptr) xSemaphoreGive(g_mutex);
}

bool snapshot(MonitorSnapshot& out, uint32_t timeout_ms)
{
    if (g_mutex == nullptr) return false;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;

    g_state.copyTo(out);

    xSemaphoreGive(g_mutex);
    return true;
}

}  // namespace monitor
