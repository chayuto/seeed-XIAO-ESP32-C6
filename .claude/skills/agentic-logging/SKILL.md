---
name: agentic-logging
description: How every project in this repo logs, so the agent reading the serial console sees what it needs without a person at the board. Apply when starting a project, adding a module, or when a log is not telling you enough.
---

# Agentic-first logging

The main reader of this board's logs is the agent, over a non-resetting `attach.sh`.
Log for that reader.

## sdkconfig.defaults

```
CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y      # DEBUG compiled in everywhere
CONFIG_LOG_DEFAULT_LEVEL_INFO=y       # third-party stays at INFO
CONFIG_LOG_COLORS=n                   # no escape codes in greps
CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096
```

In `app_main`, first thing, raise the project's own tags to DEBUG:

```c
static const char *const own_tags[] = {"main", "wifi_sta", "xiao_board", /* ... */};
for (size_t i = 0; i < sizeof(own_tags) / sizeof(own_tags[0]); i++)
    esp_log_level_set(own_tags[i], ESP_LOG_DEBUG);
```

## Shape of a line

- `key=value` pairs, one event per line: `status seq=12 pm25=4 iai=1 pwr=1`.
- A **boot** line: project, build time, IDF version, reset reason, chip rev, flash size, MAC.
- A **heartbeat** every 30 s: `hb up=… heap=… heap_min=… rssi=… <project counters>`.
  Anything that can go silent (a socket, a sensor) gets an "age of last event" field so
  silence is visible.
- Self-tests at boot print `selftest <name>=ok|FAIL <detail>`.
- Never mute a component to make the log quiet; lower its level with `esp_log_level_set`
  and say why in a comment.

## Serial commands

A single-byte command reader on the console is cheap and lets the agent poke the board
without resetting it (`send.sh`). Convention: `i` = print status now, `d` = toggle DEBUG
for everything, `r` = restart.
