/* Exercise deferred completion using the same call/return ordering as the UI. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static time_t test_time(time_t *out) {
  const time_t now = 1800000000;
  if (out) *out = now;
  return now;
}
#define time test_time
#include "time_sync_under_test.inc"

static _Alignas(8) uint8_t service[0x120];
static int callbacks;
static bool ui_pending, requeue;
static bool force_update(void) {
  *(int32_t *)(service + 0x104) = 0;
  return spoof_time_service_request_update(service);
}
static void notify(void *object, void *listeners, uintptr_t method,
                    ptrdiff_t adjustment) {
  assert(object == service && listeners == service + 0x108);
  assert(method == 0x10 && adjustment == 1);
  assert(!service[0xe8] && service[0xd4]);
  assert(*(int64_t *)(service + 0xd8) == 0);
  assert(*(int64_t *)(service + 0xe0) == 1800000000);
  assert(*(int64_t *)(service + 0xf8) == 1800000000);
  assert(*(int32_t *)(service + 0x100) == 0);
  callbacks++;
  ui_pending = false;
  if (requeue) {
    requeue = false;
    ui_pending = force_update();
    assert(ui_pending);
  }
}
static bool refresh_ui(void) {
  if (ui_pending) return false;
  /* The real UICallback writes its flag only AFTER ForceUpdate returns. */
  const bool accepted = force_update();
  ui_pending = accepted;
  return accepted;
}
int main(void) {
  memset(service, 0, sizeof service);
  *(int32_t *)(service + 0xd0) = 1800;
  time_service_notify = notify;
  process_offline_time_sync();
  assert(callbacks == 0);

  for (int attempt = 1; attempt <= 2; ++attempt) {
    assert(refresh_ui());
    assert(ui_pending && service[0xe8] && callbacks == attempt - 1);
    assert(!refresh_ui() && !force_update());
    process_offline_time_sync();
    assert(!ui_pending && !service[0xe8] && callbacks == attempt);
    assert(*(int64_t *)(service + 0xf0) == 1800000000);
    process_offline_time_sync();
    assert(callbacks == attempt); /* one completion per accepted request */
  }

  *(int32_t *)(service + 0x104) = 10;
  assert(!spoof_time_service_request_update(service));
  process_offline_time_sync();
  assert(callbacks == 2);

  assert(refresh_ui());
  requeue = true;
  process_offline_time_sync();
  assert(callbacks == 3 && ui_pending && service[0xe8]);
  process_offline_time_sync();
  assert(callbacks == 4 && !ui_pending && !service[0xe8]);
  assert(pending_time_service == NULL);
  puts("PASS: deferred time completion, repeated refresh, duplicate/cadence guards and reentrant listeners");
  return 0;
}
