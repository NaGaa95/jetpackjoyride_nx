/* Jetpack Joyride 1.104.1 / Halfbrick Mortar host for Nintendo Switch. */

#include "saltynx.h"
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#include <switch.h>
#include <SDL2/SDL.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "libc_shim.h"
#include "opensles.h"
#include "aaudio.h"
#include "bsd_bridge.h"

#define DATA_ROOT      GAME_HOME
#define LIB_MORTAR     "lib/arm64-v8a/libmortargame.so"
#define ASSET_ARCHIVE  "assets/assets.zip"
#define CLEAN_MARKER   ".nx-apk-cleaned-v1"
#define MORTAR_JNI_VERSION 0x00010004

#define MORTAR_SO_SIZE     UINT64_C(30582928)
#define MORTAR_ASSETS_SIZE UINT64_C(178724101)
#define SO_REGION_BYTES    ((size_t)96 * 1024 * 1024)

static void *heap_so_base;
static size_t heap_so_limit;

/* Globals consumed by the Android libc/mmap compatibility layer. */
void  *g_mmap_arena_base;
size_t g_mmap_arena_size;
size_t g_mmap_big_align = MMAP_ARENA_ALIGN_36;
int    g_overcommit;
u64    g_alias_base, g_alias_size;

/* Compatibility modules retained by the shared loader ABI. */
so_module main_mod, unity_mod, il2cpp_mod;
static so_module mortar_mod;

void __libnx_initheap(void) {
  void *addr = NULL;
  size_t size = 0;
  size_t total = 0, used = 0;
  const size_t MB = 1024 * 1024;
  const u64 limit36 = 1ull << 36;

  u64 aslr_base = 0, aslr_size = 0, stack_base = 0;
  const int aslr39 =
      R_SUCCEEDED(svcGetInfo(&aslr_base, InfoType_AslrRegionAddress,
                             CUR_PROCESS_HANDLE, 0)) &&
      R_SUCCEEDED(svcGetInfo(&aslr_size, InfoType_AslrRegionSize,
                             CUR_PROCESS_HANDLE, 0)) &&
      (aslr_base >= limit36 || aslr_size > limit36 - aslr_base);
  const int stack39 =
      R_SUCCEEDED(svcGetInfo(&stack_base, InfoType_StackRegionAddress,
                             CUR_PROCESS_HANDLE, 0)) &&
      stack_base >= limit36;
  if (aslr39 || stack39)
    g_mmap_big_align = MMAP_ARENA_ALIGN_39;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (total > used + 0x200000)
      size = (total - used - 0x200000) & ~(size_t)0x1fffff;
    if (!size) size = 512 * MB;
    if (R_FAILED(svcSetHeapSize(&addr, size)))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 2) so_zone = size / 2;
  const size_t align = g_mmap_big_align;
  const size_t newlib_floor = 768 * MB;
  size_t arena = MMAP_ARENA_RESERVE;
  size_t fake_heap_size;

  if (size > so_zone + align + newlib_floor + 128 * MB) {
    const size_t available = size - so_zone - align - newlib_floor;
    if (arena > available) arena = available & ~(align - 1);
    const size_t usable = size - so_zone - align;
    const size_t cap = ((usable * 30) / 100) & ~(align - 1);
    if (arena > cap) arena = cap;
    fake_heap_size = size - so_zone - arena - align;
  } else {
    arena = 0;
    fake_heap_size = size > so_zone ? size - so_zone : size / 2;
  }

  extern char *fake_heap_start, *fake_heap_end;
  fake_heap_start = (char *)addr;
  fake_heap_end = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)addr + fake_heap_size, 0x1000);
  heap_so_limit = so_zone;
  if (arena) {
    g_mmap_arena_base =
        (void *)ALIGN_MEM((uintptr_t)heap_so_base + so_zone, align);
    g_mmap_arena_size = arena;
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("The launcher did not provide an own-process handle. Start the "
                "NRO through a title override, not applet mode.");
}

static int regular_file(const char *path, uint64_t *size) {
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
  if (size) *size = (uint64_t)st.st_size;
  return 1;
}

static int directory_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void make_directory(const char *path) {
  if (mkdir(path, 0777) != 0 && errno != EEXIST)
    fatal_error("Could not create directory:\n%s\n\nerrno=%d", path, errno);
}

static void remove_file(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
    unlink(path);
  }
}

static void remove_tree(const char *path) {
  DIR *dir = opendir(path);
  if (!dir) {
    remove_file(path);
    return;
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    char child[1024];
    struct stat st;
    if (snprintf(child, sizeof child, "%s/%s", path, entry->d_name) >=
        (int)sizeof child)
      continue;
    if (stat(child, &st) == 0 && S_ISDIR(st.st_mode))
      remove_tree(child);
    else
      remove_file(child);
  }
  closedir(dir);
  rmdir(path);
}

static int copy_file(const char *source, const char *destination) {
  FILE *in = fopen(source, "rb");
  if (!in) return 0;
  FILE *out = fopen(destination, "wb");
  if (!out) {
    fclose(in);
    return 0;
  }
  void *buffer = malloc(256 * 1024);
  int ok = buffer != NULL;
  while (ok) {
    const size_t got = fread(buffer, 1, 256 * 1024, in);
    if (got && fwrite(buffer, 1, got, out) != got) ok = 0;
    if (got < 256 * 1024) {
      if (ferror(in)) ok = 0;
      break;
    }
  }
  free(buffer);
  if (fclose(out) != 0) ok = 0;
  fclose(in);
  if (!ok) unlink(destination);
  return ok;
}

static int find_named_file(const char *directory, const char *name, int depth,
                           char *out, size_t capacity) {
  if (depth < 0) return 0;
  DIR *dir = opendir(directory);
  if (!dir) return 0;
  struct dirent *entry;
  int found = 0;
  while (!found && (entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
        !strcmp(entry->d_name, "save") || !strcmp(entry->d_name, "cache") ||
        !strcmp(entry->d_name, "external"))
      continue;
    char path[1024];
    struct stat st;
    if (snprintf(path, sizeof path, "%s/%s", directory, entry->d_name) >=
        (int)sizeof path || stat(path, &st) != 0)
      continue;
    if (S_ISREG(st.st_mode) && !strcmp(entry->d_name, name)) {
      snprintf(out, capacity, "%s", path);
      found = 1;
    } else if (S_ISDIR(st.st_mode) && depth > 0) {
      found = find_named_file(path, name, depth - 1, out, capacity);
    }
  }
  closedir(dir);
  return found;
}

static void migrate_required_file(const char *destination, const char *name) {
  if (regular_file(destination, NULL)) return;
  char source[1024];
  if (!find_named_file(DATA_ROOT, name, 7, source, sizeof source)) return;
  if (!strcmp(source, destination)) return;
  if (rename(source, destination) == 0) return;
  if (!copy_file(source, destination))
    fatal_error("Found %s but could not move it into place.\n\nFrom: %s\nTo: %s",
                name, source, destination);
  unlink(source);
}

static int contains_apk_marker(const char *directory, int depth) {
  static const char *const markers[] = {
    "AndroidManifest.xml", "classes.dex", "resources.arsc"
  };
  for (size_t i = 0; i < sizeof markers / sizeof markers[0]; ++i) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", directory, markers[i]);
    if (access(path, F_OK) == 0) return 1;
  }
  if (depth <= 0) return 0;
  DIR *dir = opendir(directory);
  if (!dir) return 0;
  struct dirent *entry;
  int found = 0;
  while (!found && (entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
        !strcmp(entry->d_name, "assets") || !strcmp(entry->d_name, "lib") ||
        !strcmp(entry->d_name, "save") || !strcmp(entry->d_name, "cache") ||
        !strcmp(entry->d_name, "external"))
      continue;
    char path[1024];
    if (snprintf(path, sizeof path, "%s/%s", directory, entry->d_name) >=
        (int)sizeof path)
      continue;
    if (directory_exists(path)) found = contains_apk_marker(path, depth - 1);
  }
  closedir(dir);
  return found;
}

static int has_suffix(const char *name, const char *suffix) {
  const size_t n = strlen(name), s = strlen(suffix);
  return n >= s && !strcasecmp(name + n - s, suffix);
}

static void prune_directory_except(const char *directory, const char *keep) {
  DIR *dir = opendir(directory);
  if (!dir) return;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
        !strcmp(entry->d_name, keep))
      continue;
    char path[1024];
    if (snprintf(path, sizeof path, "%s/%s", directory, entry->d_name) >=
        (int)sizeof path)
      continue;
    if (directory_exists(path)) remove_tree(path);
    else remove_file(path);
  }
  closedir(dir);
}

static void cleanup_apk_extract(void) {
  if (access(DATA_ROOT "/" CLEAN_MARKER, F_OK) == 0) return;

  static const char *const android_dirs[] = {
    "META-INF", "res", "kotlin", "okhttp3", "org", "com", "google",
    "firebase-encoders", "play-services-basement", "stamp-cert-sha256"
  };
  for (size_t i = 0; i < sizeof android_dirs / sizeof android_dirs[0]; ++i) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", DATA_ROOT, android_dirs[i]);
    remove_tree(path);
  }

  DIR *root = opendir(DATA_ROOT);
  if (root) {
    struct dirent *entry;
    while ((entry = readdir(root)) != NULL) {
      const char *name = entry->d_name;
      if (!strcmp(name, ".") || !strcmp(name, "..") ||
          !strcmp(name, "assets") || !strcmp(name, "lib") ||
          !strcmp(name, "save") || !strcmp(name, "cache") ||
          !strcmp(name, "external"))
        continue;
      char path[1024];
      if (snprintf(path, sizeof path, "%s/%s", DATA_ROOT, name) >=
          (int)sizeof path)
        continue;
      if (directory_exists(path)) {
        if (!strncmp(name, "split", 5) || !strcasecmp(name, "base") ||
            contains_apk_marker(path, 3))
          remove_tree(path);
      } else if (has_suffix(name, ".dex") || has_suffix(name, ".apk") ||
                 has_suffix(name, ".arsc") || has_suffix(name, ".prof") ||
                 has_suffix(name, ".profm") || has_suffix(name, ".proto") ||
                 has_suffix(name, ".properties") ||
                 !strcmp(name, "AndroidManifest.xml")) {
        remove_file(path);
      }
    }
    closedir(root);
  }

  prune_directory_except(DATA_ROOT "/assets", "assets.zip");
  prune_directory_except(DATA_ROOT "/lib/arm64-v8a", "libmortargame.so");

  DIR *libs = opendir(DATA_ROOT "/lib");
  if (libs) {
    struct dirent *entry;
    while ((entry = readdir(libs)) != NULL) {
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
          !strcmp(entry->d_name, "arm64-v8a"))
        continue;
      char path[1024];
      snprintf(path, sizeof path, "%s/lib/%s", DATA_ROOT, entry->d_name);
      remove_tree(path);
    }
    closedir(libs);
  }

  FILE *marker = fopen(DATA_ROOT "/" CLEAN_MARKER, "w");
  if (marker) {
    fputs("Jetpack Joyride 1.104.1 APK cleanup completed.\n", marker);
    fclose(marker);
  }
}

static int file_header(const char *path, unsigned char *out, size_t size) {
  FILE *file = fopen(path, "rb");
  if (!file) return 0;
  const size_t got = fread(out, 1, size, file);
  fclose(file);
  return got == size;
}

static void check_game_data(void) {
  uint64_t so_size = 0, assets_size = 0;
  if (!regular_file(DATA_ROOT "/" LIB_MORTAR, &so_size))
    fatal_error("Missing %s.\n\nExtract the ARM64 split APK and copy its "
                "contents into %s.", LIB_MORTAR, DATA_ROOT);
  if (!regular_file(DATA_ROOT "/" ASSET_ARCHIVE, &assets_size))
    fatal_error("Missing %s.\n\nExtract the InstallPack split APK and copy its "
                "contents into %s.", ASSET_ARCHIVE, DATA_ROOT);

  unsigned char elf[6], zip[4];
  const int valid_elf = file_header(DATA_ROOT "/" LIB_MORTAR, elf, sizeof elf) &&
                        !memcmp(elf, "\x7f" "ELF\x02\x01", sizeof elf);
  const int valid_zip = file_header(DATA_ROOT "/" ASSET_ARCHIVE, zip, sizeof zip) &&
                        zip[0] == 'P' && zip[1] == 'K' && zip[2] == 3 && zip[3] == 4;
  if (!valid_elf || !valid_zip || so_size != MORTAR_SO_SIZE ||
      assets_size != MORTAR_ASSETS_SIZE) {
    fatal_error("Unsupported or incomplete game files.\n\nThis wrapper requires "
                "Jetpack Joyride %s (version code %d), ARM64, with InstallPack."
                "\n\nlibmortargame.so: %llu bytes (expected %llu)"
                "\nassets.zip: %llu bytes (expected %llu)",
                GAME_VERSION_NAME, GAME_VERSION_CODE,
                (unsigned long long)so_size,
                (unsigned long long)MORTAR_SO_SIZE,
                (unsigned long long)assets_size,
                (unsigned long long)MORTAR_ASSETS_SIZE);
  }
}

static int load_mortar(void) {
  const char *path = DATA_ROOT "/" LIB_MORTAR;
  if (so_load(&mortar_mod, path, heap_so_base, heap_so_limit) < 0) return 0;
  snprintf(mortar_mod.name, sizeof mortar_mod.name, "%s", "libmortargame.so");
  const size_t used = ALIGN_MEM(mortar_mod.load_size, 0x1000);
  if (used > heap_so_limit) return 0;
  heap_so_base = (char *)heap_so_base + used;
  heap_so_limit -= used;
  return 1;
}

static bool offline_remote_config_is_waiting(void *manager) {
  (void)manager;
  return false;
}

static bool offline_google_play_is_authenticating(void) {
  return false;
}

static bool offline_notification_permission(void) {
  return true;
}

/* Jetpack::TimeService normally confirms the device clock by POSTing to a
 * Halfbrick endpoint and reading the UTC seconds it echoes back. Nothing
 * else here calls out to the internet, so that round trip always times
 * out, timeJJ::IsServerTimeReliable() stays false, and every feature that
 * gates on it refuses to run: SAMRewardSystem's daily reset (Strong Arm
 * Machine), DailyMissions::Sync, Game::UpdateDiscountedItems,
 * Level::SetupNextBarryBotToken, TitleMenu::Update, notice-board/ad-
 * mediation date checks, and more.
 *
 * Queue a synthetic response to RequestTimeUpdate(), also reached through
 * ForceUpdate(). The UI sets its pending flag AFTER ForceUpdate() returns,
 * so listener callbacks must run later, outside the game's update stack.
 * Use the game's own weak_ptr-aware dispatcher to retain listener lifetime
 * management and the normal success triggers. All offsets below are for
 * the signature-checked 1.104.1 ARM64 build. */
typedef void (*time_service_notify_fn)(void *service, void *listeners,
                                       uintptr_t method, ptrdiff_t adjustment);
static time_service_notify_fn time_service_notify;
static void *pending_time_service;

static bool spoof_time_service_request_update(void *time_service) {
  uint8_t *ts = (uint8_t *)time_service;
  if (pending_time_service || *(uint8_t *)(ts + 0xe8) ||
      *(int32_t *)(ts + 0x104) > 0)
    return false;

  *(uint8_t *)(ts + 0xe8) = 1; /* request in flight */
  *(int32_t *)(ts + 0x104) = *(int32_t *)(ts + 0xd0);
  pending_time_service = time_service;
  return true;
}

static void process_offline_time_sync(void) {
  uint8_t *ts = (uint8_t *)pending_time_service;
  if (!ts) return;
  /* A listener may request another update; leave that for the next frame. */
  pending_time_service = NULL;
  const int64_t now = (int64_t)time(NULL);

  *(uint8_t *)(ts + 0xe8) = 0;    /* no request in flight */
  *(uint8_t *)(ts + 0xd4) = 1;    /* reliable */
  *(int64_t *)(ts + 0xd8) = 0;    /* accumulated drift */
  *(int64_t *)(ts + 0xe0) = now;  /* local clock at "receipt" */
  *(int64_t *)(ts + 0xf0) = now;  /* local clock at "request" */
  *(int64_t *)(ts + 0xf8) = now;  /* corrected server epoch */
  *(int32_t *)(ts + 0x100) = 0;   /* baseline offset (perfect sync) */
  /* ARM64 libc++ member pointer: virtual slot 0x10, adjustment 0 + virtual
   * bit 1. This is the exact success dispatch used by OnDownloadResponse(). */
  time_service_notify(ts, ts + 0x108, 0x10, 1);
}

typedef void *(*splash_game_get_fn)(void);
typedef bool (*splash_load_content_fn)(void *game, int *state);
typedef void (*splash_setup_text_fn)(void *splash);

static splash_game_get_fn splash_game_get;
static splash_load_content_fn splash_load_content;
static splash_setup_text_fn splash_setup_text;

/* FastLoad follows the splash loader lifetime. */
static bool splash_loading_active;
static bool fast_load_applied;

static void apply_loading_cpu_boost(bool enabled) {
  if (fast_load_applied == enabled)
    return;

  const Result rc = appletSetCpuBoostMode(
      enabled ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
  if (R_SUCCEEDED(rc)) fast_load_applied = enabled;
}

static void begin_splash_loading_cpu_boost(void) {
  if (splash_loading_active)
    return;
  splash_loading_active = true;
  apply_loading_cpu_boost(true);
}

static void end_splash_loading_cpu_boost(void) {
  splash_loading_active = false;
  apply_loading_cpu_boost(false);
}

static void suspend_splash_loading_cpu_boost(void) {
  if (splash_loading_active)
    apply_loading_cpu_boost(false);
}

static void resume_splash_loading_cpu_boost(void) {
  if (splash_loading_active)
    apply_loading_cpu_boost(true);
}

static bool splash_update_with_cpu_boost(void *splash, void *unused_game) {
  (void)unused_game;
  int *state = (int *)((uint8_t *)splash + 0x40);
  begin_splash_loading_cpu_boost();
  const bool complete = splash_load_content(splash_game_get(), state);
  splash_setup_text(splash);
  if (complete) end_splash_loading_cpu_boost();
  return complete;
}

static void install_offline_service_patches(void) {
  static const char firebase_provider_get[] =
      "_ZNK6Mortar14ServiceManager11GetProviderINS_20Provider_FirebaseCPPEEEPT_v";
  static const uint32_t expected[] = {
    UINT32_C(0xa9be7bfd), /* stp x29, x30, [sp, #-32]! */
    UINT32_C(0xf9000bf3), /* str x19, [sp, #16] */
  };
  static const uint32_t offline_return[] = {
    UINT32_C(0xd2800000), /* mov x0, #0 */
    UINT32_C(0xd65f03c0), /* ret */
  };

  uint32_t *code = (uint32_t *)so_find_addr(&mortar_mod,
                                             firebase_provider_get);
  if (memcmp(code, expected, sizeof expected) != 0)
    fatal_error("Unsupported FirebaseCPP provider code in libmortargame.so.");
  memcpy(code, offline_return, sizeof offline_return);
  armDCacheFlush(code, sizeof offline_return);

  static const char remote_config_waiting[] =
      "_ZNK7Jetpack19RemoteConfigManager24IsWaitingForFetchResultsEv";
  static const uint32_t expected_remote_config[] = {
    UINT32_C(0xf9400408), /* ldr x8, [x0, #8] */
    UINT32_C(0xb9401409), /* ldr w9, [x0, #20] */
    UINT32_C(0xf100011f), /* cmp x8, #0 */
    UINT32_C(0x7a401924), /* ccmp w9, #0, #4, ne */
  };

  code = (uint32_t *)so_find_addr(&mortar_mod, remote_config_waiting);
  if (!code || memcmp(code, expected_remote_config,
                      sizeof expected_remote_config) != 0)
    fatal_error("Unsupported RemoteConfig wait code in libmortargame.so.");
  hook_arm64((uintptr_t)code,
             (uintptr_t)offline_remote_config_is_waiting);
  armDCacheFlush(code, 4 * sizeof(*code));

  static const char google_play_authenticating[] =
      "_ZN6Mortar19Provider_GooglePlay16IsAuthenticatingEv";
  static const uint32_t expected_google_play_auth[] = {
    UINT32_C(0xb0004c88), /* adrp x8, m_authState@GOTPAGE */
    UINT32_C(0xf9401908), /* ldr x8, [x8, #48] */
    UINT32_C(0x88dffd08), /* ldar w8, [x8] */
    UINT32_C(0x7100011f), /* cmp w8, #0 */
  };

  code = (uint32_t *)so_find_addr(&mortar_mod, google_play_authenticating);
  if (!code || memcmp(code, expected_google_play_auth,
                      sizeof expected_google_play_auth) != 0)
    fatal_error("Unsupported Google Play auth code in libmortargame.so.");
  hook_arm64((uintptr_t)code,
             (uintptr_t)offline_google_play_is_authenticating);
  armDCacheFlush(code, 4 * sizeof(*code));

  /* Keep Google Play billing registered while Halfbrick Plus is enabled. */
  static const char subscribe_purchase_services[] =
      "_ZN7Jetpack15PurchaseManager33SubscribeToMortarPurchaseServicesEv";
  code = (uint32_t *)so_find_addr(&mortar_mod, subscribe_purchase_services);
  if (!code || code[0x38 / sizeof(*code)] != UINT32_C(0x37000f40))
    fatal_error("Unsupported purchase service subscription code in "
                "libmortargame.so.");
  code[0x38 / sizeof(*code)] = UINT32_C(0xd503201f); /* nop */
  armDCacheFlush(code + 0x38 / sizeof(*code), sizeof(*code));

  /* Route shop purchases through the local billing service. */
  static const char iap_begin_purchase[] =
      "_ZN7Jetpack5Utils10IAPWrapper13BeginPurchaseEPKcS3_";
  code = (uint32_t *)so_find_addr(&mortar_mod, iap_begin_purchase);
  if (!code || code[0xc4 / sizeof(*code)] != UINT32_C(0x34000ce0))
    fatal_error("Unsupported IAP reachability gate in libmortargame.so.");
  code[0xc4 / sizeof(*code)] = UINT32_C(0xd503201f); /* nop */
  armDCacheFlush(code + 0xc4 / sizeof(*code), sizeof(*code));

  static const char notification_permission[] =
      "_ZN6Mortar22NotificationPermission29RequestNotificationPermissionEv";
  static const uint32_t expected_notification_permission[] = {
    UINT32_C(0xa9bd7bfd), /* stp x29, x30, [sp, #-48]! */
    UINT32_C(0xf9000bf5), /* str x21, [sp, #16] */
    UINT32_C(0xa9024ff4), /* stp x20, x19, [sp, #32] */
    UINT32_C(0x910003fd), /* mov x29, sp */
  };

  code = (uint32_t *)so_find_addr(&mortar_mod, notification_permission);
  if (!code || memcmp(code, expected_notification_permission,
                      sizeof expected_notification_permission) != 0)
    fatal_error("Unsupported notification permission code in libmortargame.so.");
  hook_arm64((uintptr_t)code,
             (uintptr_t)offline_notification_permission);
  armDCacheFlush(code, 4 * sizeof(*code));

  /* Select the built-in offline connection factory. */
  static const char open_connection[] = "_ZN4Game14OpenConnectionEv";
  code = (uint32_t *)so_find_addr(&mortar_mod, open_connection);
  if (!code || code[0x60 / sizeof(*code)] != UINT32_C(0x34000528))
    fatal_error("Unsupported multiplayer connection code in libmortargame.so.");
  code[0x60 / sizeof(*code)] = UINT32_C(0x14000029); /* b .+0xa4 */
  armDCacheFlush(code + 0x60 / sizeof(*code), sizeof(*code));

  static const char splash_update[] =
      "_ZN13SplashScreens6UpdateER4Game";
  static const uint32_t expected_splash_update[] = {
    UINT32_C(0xa9be7bfd), /* stp x29, x30, [sp, #-32]! */
    UINT32_C(0xa9014ff4), /* stp x20, x19, [sp, #16] */
    UINT32_C(0x910003fd), /* mov x29, sp */
    UINT32_C(0xaa0003f3), /* mov x19, x0 */
  };

  code = (uint32_t *)so_find_addr(&mortar_mod, splash_update);
  if (!code || memcmp(code, expected_splash_update,
                      sizeof expected_splash_update) != 0)
    fatal_error("Unsupported SplashScreens update code in libmortargame.so.");
  hook_arm64((uintptr_t)code, (uintptr_t)splash_update_with_cpu_boost);
  armDCacheFlush(code, 4 * sizeof(*code));

  /* Trust the console's own clock instead of failing the server-time sync;
   * see spoof_time_service_request_update() for why. */
  static const char time_service_request_update[] =
      "_ZN7Jetpack11TimeService17RequestTimeUpdateEv";
  static const uint32_t expected_time_service_request_update[] = {
    UINT32_C(0xd10443ff), /* sub sp, sp, #0x110 */
    UINT32_C(0xa90e7bfd), /* stp x29, x30, [sp, #0xe0] */
    UINT32_C(0xa90f57fc), /* stp x28, x21, [sp, #0xf0] */
    UINT32_C(0xa9104ff4), /* stp x20, x19, [sp, #0x100] */
  };

  code = (uint32_t *)so_find_addr(&mortar_mod, time_service_request_update);
  if (!code || memcmp(code, expected_time_service_request_update,
                      sizeof expected_time_service_request_update) != 0)
    fatal_error("Unsupported TimeService code in libmortargame.so.");

  static const char time_service_response[] =
      "_ZN7Jetpack11TimeService18OnDownloadResponseEPN6Mortar11HttpRequestEPNS1_12HttpResponseE";
  static const uint32_t expected_time_service_dispatch[] = {
    UINT32_C(0xd10203ff), /* sub sp, sp, #0x80 */
    UINT32_C(0xa9027bfd), /* stp x29, x30, [sp, #0x20] */
    UINT32_C(0xf9001bfb), /* str x27, [sp, #0x30] */
    UINT32_C(0xa90467fa), /* stp x26, x25, [sp, #0x40] */
  };
  const uint32_t *response = (const uint32_t *)so_find_addr(
      &mortar_mod, time_service_response);
  /* Verify both the success member pointer and its call to the internal
   * dispatcher before reusing that helper at OnDownloadResponse + 0x290. */
  if (!response || response[0xa8 / 4] != UINT32_C(0x52800023) ||
      response[0xac / 4] != UINT32_C(0x52800202) ||
      response[0x15c / 4] != UINT32_C(0x9400004d) ||
      memcmp(response + 0x290 / 4, expected_time_service_dispatch,
             sizeof expected_time_service_dispatch) != 0)
    fatal_error("Unsupported TimeService listener code in libmortargame.so.");
  time_service_notify = (time_service_notify_fn)(
      so_find_addr_rx(&mortar_mod, time_service_response) + 0x290);

  hook_arm64((uintptr_t)code, (uintptr_t)spoof_time_service_request_update);
  armDCacheFlush(code, 4 * sizeof(*code));
}

typedef struct {
  EGLDisplay display;
  EGLSurface surface;
  EGLContext context;
} Graphics;

static void egl_require(EGLBoolean ok, const char *operation) {
  if (!ok) fatal_error("%s failed (EGL error 0x%04x).", operation, eglGetError());
}

static void graphics_init(Graphics *graphics) {
  memset(graphics, 0, sizeof *graphics);
  graphics->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (graphics->display == EGL_NO_DISPLAY)
    fatal_error("eglGetDisplay failed (0x%04x).", eglGetError());
  egl_require(eglInitialize(graphics->display, NULL, NULL), "eglInitialize");
  egl_require(eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI");

  const EGLint config_attributes[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_NONE
  };
  EGLConfig egl_config = NULL;
  EGLint count = 0;
  egl_require(eglChooseConfig(graphics->display, config_attributes, &egl_config,
                              1, &count), "eglChooseConfig");
  if (!count) fatal_error("No compatible GLES2 EGL configuration is available.");

  NWindow *window = nwindowGetDefault();
  nwindowSetDimensions(window, (u32)screen_width, (u32)screen_height);
  nwindowSetCrop(window, 0, 0, (u32)screen_width, (u32)screen_height);
  nwindowSetTransform(window, 0);
  graphics->surface =
      eglCreateWindowSurface(graphics->display, egl_config, window, NULL);
  if (graphics->surface == EGL_NO_SURFACE)
    fatal_error("eglCreateWindowSurface failed (0x%04x).", eglGetError());

  const EGLint context_attributes[] = {
    EGL_CONTEXT_CLIENT_VERSION, 2,
    EGL_NONE
  };
  graphics->context = eglCreateContext(graphics->display, egl_config,
                                       EGL_NO_CONTEXT, context_attributes);
  if (graphics->context == EGL_NO_CONTEXT)
    fatal_error("eglCreateContext failed (0x%04x).", eglGetError());
  egl_require(eglMakeCurrent(graphics->display, graphics->surface,
                             graphics->surface, graphics->context),
              "eglMakeCurrent");
  eglSwapInterval(graphics->display, 1);
}

static void graphics_shutdown(Graphics *graphics) {
  if (!graphics || graphics->display == EGL_NO_DISPLAY) return;
  eglMakeCurrent(graphics->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                 EGL_NO_CONTEXT);
  if (graphics->context != EGL_NO_CONTEXT)
    eglDestroyContext(graphics->display, graphics->context);
  if (graphics->surface != EGL_NO_SURFACE)
    eglDestroySurface(graphics->display, graphics->surface);
  eglTerminate(graphics->display);
  memset(graphics, 0, sizeof *graphics);
}

typedef uint8_t jboolean;
typedef void (*jni_void_fn)(void *, void *);
typedef int (*jni_onload_fn)(void *, void *);
typedef void (*jni_init_files_fn)(void *, void *, void *, void *, void *, void *,
                                  jboolean);
typedef void (*jni_system_init_fn)(void *, void *, int, int, void *);
typedef void (*jni_set_license_fn)(void *, void *, jboolean);
typedef void (*jni_object_fn)(void *, void *, void *);
typedef jboolean (*jni_bool_fn)(void *, void *);
typedef void (*jni_resume_fn)(void *, void *, void *, int, int, jboolean);
typedef void (*jni_insets_fn)(void *, void *, int, int, int, int);
typedef void (*jni_touch_fn)(void *, void *, int, int64_t, int, float, float,
                             float, float);
typedef void (*jni_key_fn)(void *, void *, int, jboolean, jboolean, int);
typedef void (*jni_motion_fn)(void *, void *, int, int, float, float);
typedef void (*jni_controller_attach_fn)(void *, void *, int, void *);
typedef void (*jni_controller_detach_fn)(void *, void *, int);

typedef struct {
  jni_init_files_fn init_files;
  jni_system_init_fn system_init;
  jni_void_fn init_device_properties;
  jni_set_license_fn set_app_licensed;
  jni_object_fn set_consent_platform;
  jni_void_fn game_init;
  jni_bool_fn step;
  jni_bool_fn resume_step;
  jni_bool_fn game_requested_quit;
  jni_bool_fn game_requested_restart;
  jni_resume_fn resume;
  jni_void_fn resume_soft;
  jni_void_fn pause;
  jni_void_fn focus_lost;
  jni_void_fn focus_retrieved;
  jni_void_fn pause_audio;
  jni_void_fn resume_audio;
  jni_void_fn save_on_exit;
  jni_insets_fn update_insets;
  jni_touch_fn touch;
  jni_key_fn key;
  jni_motion_fn motion;
  jni_controller_attach_fn controller_attach;
  jni_controller_detach_fn controller_detach;
} MortarApi;

static uintptr_t mortar_symbol(const char *name) {
  const uintptr_t address = so_try_find_addr_rx(&mortar_mod, name);
  if (!address) fatal_error("Required Mortar export is missing:\n%s", name);
  return address;
}

#define MORTAR_NATIVE(name) \
  "Java_com_halfbrick_mortar_NativeGameLib_native_1" name

static void mortar_api_init(MortarApi *api) {
  memset(api, 0, sizeof *api);
  api->init_files = (jni_init_files_fn)mortar_symbol(MORTAR_NATIVE("InitFileManager"));
  api->system_init = (jni_system_init_fn)mortar_symbol(MORTAR_NATIVE("SystemInit"));
  api->init_device_properties = (jni_void_fn)mortar_symbol(MORTAR_NATIVE("InitDeviceProperties"));
  api->set_app_licensed = (jni_set_license_fn)mortar_symbol(MORTAR_NATIVE("SetAppLicensed"));
  api->set_consent_platform = (jni_object_fn)mortar_symbol(MORTAR_NATIVE("SetConsentManagementPlatform"));
  api->game_init = (jni_void_fn)mortar_symbol(MORTAR_NATIVE("GameInit"));
  api->step = (jni_bool_fn)mortar_symbol(MORTAR_NATIVE("step"));
  api->resume_step = (jni_bool_fn)mortar_symbol(MORTAR_NATIVE("onResumeStep"));
  api->game_requested_quit = (jni_bool_fn)mortar_symbol(MORTAR_NATIVE("gameRequestedQuit"));
  api->game_requested_restart = (jni_bool_fn)mortar_symbol(MORTAR_NATIVE("gameRequestedRestart"));
  api->resume = (jni_resume_fn)mortar_symbol(MORTAR_NATIVE("onResume"));
  api->resume_soft = (jni_void_fn)mortar_symbol(MORTAR_NATIVE("onResumeSoft"));
  api->pause = (jni_void_fn)mortar_symbol(MORTAR_NATIVE("onPause"));
  api->focus_lost = (jni_void_fn)mortar_symbol(MORTAR_NATIVE("onFocusLost"));
  api->focus_retrieved = (jni_void_fn)mortar_symbol(MORTAR_NATIVE("onFocusRetrieved"));
  api->pause_audio = (jni_void_fn)mortar_symbol(MORTAR_NATIVE("pauseAudioSL"));
  api->resume_audio = (jni_void_fn)mortar_symbol(MORTAR_NATIVE("resumeAudioSL"));
  api->save_on_exit = (jni_void_fn)mortar_symbol(MORTAR_NATIVE("saveOnExit"));
  api->update_insets = (jni_insets_fn)mortar_symbol(MORTAR_NATIVE("onViewInsetsUpdated"));
  api->touch = (jni_touch_fn)mortar_symbol(MORTAR_NATIVE("touchEvent"));
  api->key = (jni_key_fn)mortar_symbol(MORTAR_NATIVE("keyEvent"));
  api->motion = (jni_motion_fn)mortar_symbol(MORTAR_NATIVE("motionEvent"));
  api->controller_attach = (jni_controller_attach_fn)mortar_symbol(MORTAR_NATIVE("onGameControllerAttach"));
  api->controller_detach = (jni_controller_detach_fn)mortar_symbol(MORTAR_NATIVE("onGameControllerDetach"));
}

static int64_t event_time_ms(void) {
  struct timespec ts = {0};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

typedef enum {
  TOUCH_NONE, TOUCH_PHYSICAL, TOUCH_MINUS, TOUCH_CURSOR
} TouchSource;

typedef struct {
  PadState pad;
  HidTouchScreenState touch_state;
  int active_touch;
  float last_x, last_y;
  float cursor_x, cursor_y;
  uint64_t previous_ns;
  int cursor_visible;
  int mouse_mode; /* 1 = on-screen cursor active, 0 = full controller passthrough */
  int prev_hat; /* combined physical/stick D-pad: up, down, left, right */
  TouchSource touch_source;
} InputState;

typedef struct { uint64_t button; int android_key; } KeyBinding;
static const KeyBinding key_bindings[] = {
  { HidNpadButton_A,      96 }, { HidNpadButton_B,      97 },
  { HidNpadButton_X,      99 }, { HidNpadButton_Y,     100 },
  { HidNpadButton_L,     102 }, { HidNpadButton_R,     103 },
  { HidNpadButton_ZR,    105 },
  { HidNpadButton_StickL,106 }, { HidNpadButton_StickR,107 },
  { HidNpadButton_Plus,  108 },
};

static uint64_t monotonic_ns(void) {
  struct timespec ts = {0};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void input_init(InputState *input) {
  memset(input, 0, sizeof *input);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&input->pad);
  hidInitializeTouchScreen();
  input->cursor_x = input->last_x = 0.5f;
  input->cursor_y = input->last_y = 0.5f;
  input->previous_ns = monotonic_ns();
  /* Controller mode starts with the cursor hidden in handheld and docked. */
}

static void send_touch(const MortarApi *api, void *env, void *thiz, int action,
                       float x, float y) {
  if (x < 0.0f) x = 0.0f; else if (x > 1.0f) x = 1.0f;
  if (y < 0.0f) y = 0.0f; else if (y > 1.0f) y = 1.0f;
  api->touch(env, thiz, action, event_time_ms(), 0, x, y,
             action == 1 ? 0.0f : 1.0f, 1.0f);
}

static void input_update(InputState *input, const MortarApi *api,
                         void *env, void *thiz) {
  padUpdate(&input->pad);
  const uint64_t down = padGetButtonsDown(&input->pad);
  const uint64_t up = padGetButtonsUp(&input->pad);
  const uint64_t held = padGetButtons(&input->pad);

  /* The common D-pad/touch paths below release state from the old mode. */
  if (down & HidNpadButton_ZL) {
    input->mouse_mode = !input->mouse_mode;
    if (!input->mouse_mode)
      api->motion(env, thiz, 0, 0, 0.0f, 0.0f);
  }

  for (size_t i = 0; i < sizeof key_bindings / sizeof key_bindings[0]; ++i) {
    if (down & key_bindings[i].button)
      api->key(env, thiz, key_bindings[i].android_key, 1, 0, 0);
    if (up & key_bindings[i].button)
      api->key(env, thiz, key_bindings[i].android_key, 0, 0, 0);
  }

  const uint64_t now = monotonic_ns();
  float dt = (float)(now - input->previous_ns) / 1000000000.0f;
  input->previous_ns = now;
  if (dt < 0.0f || dt > 0.1f) dt = 1.0f / 60.0f;
  const HidAnalogStickState stick = padGetStickPos(&input->pad, 0);
  float sx = (float)stick.x / 32768.0f;
  float sy = (float)stick.y / 32768.0f;
  if (sx > -0.14f && sx < 0.14f) sx = 0.0f;
  if (sy > -0.14f && sy < 0.14f) sy = 0.0f;

  /* Emit transitions for the combined state, including in mouse mode, so
   * leaving stick navigation releases it without cancelling a held D-pad. */
  const int mask =
      ((held & HidNpadButton_Up) || (!input->mouse_mode && sy > 0.5f) ? 1 : 0) |
      ((held & HidNpadButton_Down) || (!input->mouse_mode && sy < -0.5f) ? 2 : 0) |
      ((held & HidNpadButton_Left) || (!input->mouse_mode && sx < -0.5f) ? 4 : 0) |
      ((held & HidNpadButton_Right) || (!input->mouse_mode && sx > 0.5f) ? 8 : 0);
  const int changed = mask ^ input->prev_hat;
  for (int i = 0; i < 4; ++i) {
    if (changed & (1 << i))
      api->key(env, thiz, 19 + i, (mask & (1 << i)) != 0, 0, 0);
  }
  input->prev_hat = mask;

  if (input->mouse_mode) {
    input->cursor_x += sx * dt * 0.75f;
    input->cursor_y -= sy * dt * 0.75f;
    if (input->cursor_x < 0.0f) input->cursor_x = 0.0f;
    if (input->cursor_x > 1.0f) input->cursor_x = 1.0f;
    if (input->cursor_y < 0.0f) input->cursor_y = 0.0f;
    if (input->cursor_y > 1.0f) input->cursor_y = 1.0f;
    api->motion(env, thiz, 0, 0, sx, -sy);
  }

  const int physical = hidGetTouchScreenStates(&input->touch_state, 1) > 0 &&
                       input->touch_state.count > 0;
  input->cursor_visible = input->mouse_mode && !physical;

  /* One pointer, with explicit priority: real finger, Minus, cursor click.
   * Releasing a lower-priority source must not release the current owner. */
  TouchSource source = TOUCH_NONE;
  float x = input->cursor_x, y = input->cursor_y;
  if (physical) {
    source = TOUCH_PHYSICAL;
    /* HID coordinates describe the 1280x720 touch surface, not the render target. */
    x = (float)input->touch_state.touches[0].x / 1280.0f;
    y = (float)input->touch_state.touches[0].y / 720.0f;
  } else if (held & HidNpadButton_Minus) {
    source = TOUCH_MINUS;
    x = 0.5f;
    y = (float)(screen_height - 1) / (float)screen_height;
  } else if (input->mouse_mode &&
             (held & (HidNpadButton_A | HidNpadButton_ZR))) {
    source = TOUCH_CURSOR;
  }

  if (input->active_touch && source != input->touch_source)
    send_touch(api, env, thiz, 1, input->last_x, input->last_y);
  if (source) {
    send_touch(api, env, thiz, source == input->touch_source ? 2 : 0, x, y);
    input->last_x = x;
    input->last_y = y;
  }
  input->active_touch = source != TOUCH_NONE;
  input->touch_source = source;
}

typedef struct {
  GLuint program;
  GLint position;
  int ready;
} CursorRenderer;

static GLuint compile_shader(GLenum type, const char *source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static void cursor_renderer_init(CursorRenderer *cursor) {
  memset(cursor, 0, sizeof *cursor);
  static const char vertex_source[] =
      "attribute vec2 p; void main(){gl_Position=vec4(p,0.0,1.0);"
      "gl_PointSize=18.0;}";
  static const char fragment_source[] =
      "precision mediump float; void main(){vec2 q=gl_PointCoord-vec2(0.5);"
      "if(dot(q,q)>0.25) discard; gl_FragColor=vec4(1.0,0.85,0.1,0.95);}";
  const GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
  const GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
  if (!vertex || !fragment) return;
  cursor->program = glCreateProgram();
  glAttachShader(cursor->program, vertex);
  glAttachShader(cursor->program, fragment);
  glLinkProgram(cursor->program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint ok = GL_FALSE;
  glGetProgramiv(cursor->program, GL_LINK_STATUS, &ok);
  if (!ok) {
    glDeleteProgram(cursor->program);
    cursor->program = 0;
    return;
  }
  cursor->position = glGetAttribLocation(cursor->program, "p");
  cursor->ready = cursor->position >= 0;
}

static void cursor_renderer_draw(const CursorRenderer *cursor,
                                 const InputState *input) {
  if (!cursor->ready || !input->mouse_mode || !input->cursor_visible) return;
  GLint old_program = 0, old_buffer = 0, old_framebuffer = 0;
  GLint old_viewport[4] = {0};
  GLint attribute_enabled = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_buffer);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_framebuffer);
  glGetIntegerv(GL_VIEWPORT, old_viewport);
  glGetVertexAttribiv((GLuint)cursor->position, GL_VERTEX_ATTRIB_ARRAY_ENABLED,
                      &attribute_enabled);
  const GLboolean depth = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean cull = glIsEnabled(GL_CULL_FACE);
  const GLboolean blend = glIsEnabled(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, screen_width, screen_height);
  glUseProgram(cursor->program);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  const GLfloat point[] = {
    input->cursor_x * 2.0f - 1.0f,
    1.0f - input->cursor_y * 2.0f
  };
  glEnableVertexAttribArray((GLuint)cursor->position);
  glVertexAttribPointer((GLuint)cursor->position, 2, GL_FLOAT, GL_FALSE, 0, point);
  glDrawArrays(GL_POINTS, 0, 1);
  if (!attribute_enabled)
    glDisableVertexAttribArray((GLuint)cursor->position);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_buffer);
  glUseProgram((GLuint)old_program);
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_framebuffer);
  glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
  if (depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
  if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
  if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
}

static void cursor_renderer_shutdown(CursorRenderer *cursor) {
  if (cursor->program) glDeleteProgram(cursor->program);
  memset(cursor, 0, sizeof *cursor);
}

static const char *game_language(void) {
  if (config.language == LANG_JA) return "ja";
  if (config.language == LANG_EN) return "en";
  static char language[3] = "en";
  const char *locale = jni_locale_name();
  if (locale && locale[0] && locale[1]) {
    language[0] = locale[0];
    language[1] = locale[1];
  }
  return language;
}

int main(int argc, char **argv) {
  saltynx_start();
  atexit(saltynx_stop);
  (void)argc;
  (void)argv;

  startup_status_begin("Preparing extracted Android files...");
  mkdir(DATA_ROOT, 0777);
  if (chdir(DATA_ROOT) != 0) fatal_error("Could not open %s.", DATA_ROOT);
  check_syscalls();

  make_directory(DATA_ROOT "/assets");
  make_directory(DATA_ROOT "/lib");
  make_directory(DATA_ROOT "/lib/arm64-v8a");
  make_directory(DATA_ROOT "/save");
  make_directory(DATA_ROOT "/cache");
  make_directory(DATA_ROOT "/external");
  migrate_required_file(DATA_ROOT "/" LIB_MORTAR, "libmortargame.so");
  migrate_required_file(DATA_ROOT "/" ASSET_ARCHIVE, "assets.zip");

  startup_status_update("Validating Jetpack Joyride 1.104.1...");
  check_game_data();
  startup_status_update("Removing unused APK files (first boot)...");
  cleanup_apk_extract();

  const char *config_path = DATA_ROOT "/" CONFIG_NAME;
  if (read_config(config_path) != 0) write_config(config_path);
  config.portrait = 0;
  if (config.screen_width >= 640 && config.screen_width <= 1920 &&
      config.screen_height >= 360 && config.screen_height <= 1080) {
    screen_width = config.screen_width;
    screen_height = config.screen_height;
  } else {
    screen_width = 1280;
    screen_height = 720;
  }

  startup_status_update("Starting network, audio, and Android services...");
  nx_net_init();
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    fatal_error("SDL_Init failed: %s", SDL_GetError());

  startup_status_update("Loading the Halfbrick Mortar engine...");
  if (!load_mortar()) fatal_error("Could not map %s.", LIB_MORTAR);
  const int unresolved = resolve_imports(&mortar_mod);
  if (unresolved != 0)
    fatal_error("libmortargame.so has %d unresolved native import%s.",
                unresolved, unresolved == 1 ? "" : "s");
  install_offline_service_patches();
  so_finalize(&mortar_mod);
  so_flush_caches(&mortar_mod);
  static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_tls);
  so_execute_init_array(&mortar_mod);
  so_free_temp(&mortar_mod);

  startup_status_update("Creating the GLES2 display...");
  startup_status_end();
  Graphics graphics = {
    .display = EGL_NO_DISPLAY,
    .surface = EGL_NO_SURFACE,
    .context = EGL_NO_CONTEXT,
  };
  graphics_init(&graphics);

  jni_init();
  jni_set_snapshot_opened_callback(
      (jni_snapshot_opened_fn)mortar_symbol(
          "Java_com_halfbrick_mortar_"
          "GooglePlaySnapshotManager_OnSnapshotOpened"));
  jni_set_snapshot_committed_callback(
      (jni_snapshot_committed_fn)mortar_symbol(
          "Java_com_halfbrick_mortar_"
          "GooglePlaySnapshotManager_OnSnapshotCommitted"));
  void *env = fake_env;
  void *thiz = jni_make_activity_object();
  jni_onload_fn onload =
      (jni_onload_fn)mortar_symbol("JNI_OnLoad");
  const int jni_version = onload(fake_vm, NULL);
  if (jni_version != MORTAR_JNI_VERSION)
    fatal_error("Mortar JNI initialization returned 0x%x; expected JNI 1.4 "
                "(0x%x).", jni_version, MORTAR_JNI_VERSION);

  MortarApi api;
  mortar_api_init(&api);
  splash_game_get =
      (splash_game_get_fn)mortar_symbol("_ZN4Game11GetInstanceEv");
  splash_load_content =
      (splash_load_content_fn)mortar_symbol("_ZN4Game15LoadGameContentERi");
  splash_setup_text =
      (splash_setup_text_fn)mortar_symbol("_ZN13SplashScreens9SetupTextEv");
  void *source_path = jni_make_string(managed_path(DATA_ROOT "/base.apk"));
  void *save_path = jni_make_string(managed_path(DATA_ROOT "/save/"));
  void *cache_path = jni_make_string(managed_path(DATA_ROOT "/cache/"));
  void *external_path = jni_make_string(managed_path(DATA_ROOT "/external/"));
  void *language = jni_make_string(game_language());

  /* Required by Game::WaitConsentManagement. */
  api.set_consent_platform(
      env, thiz,
      jni_make_object("com/halfbrick/mortar/ConsentManagementPlatform"));

  /* InitFileManager consumes the premade device state. */
  api.init_device_properties(env, thiz);
  api.init_files(env, thiz, source_path, save_path, cache_path, external_path, 0);
  api.system_init(env, thiz, screen_width, screen_height, language);
  api.set_app_licensed(env, thiz, 1);
  glFrontFace(GL_CCW);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  api.game_init(env, thiz);
  glFrontFace(GL_CCW);
  glEnable(GL_CULL_FACE);
  glCullFace(GL_BACK);
  glEnable(GL_DEPTH_TEST);
  api.update_insets(env, thiz, 0, 0, 0, 0);
  api.controller_attach(env, thiz, 0,
                        jni_make_string("Nintendo Switch Controller"));
  api.focus_retrieved(env, thiz);

  InputState input;
  input_init(&input);
  CursorRenderer cursor;
  cursor_renderer_init(&cursor);
  int resuming = 1;
  AppletFocusState previous_focus = appletGetFocusState();

  while (appletMainLoop() && !jni_quit_requested) {
    const AppletFocusState focus = appletGetFocusState();
    if (focus != previous_focus) {
      const int active = focus == AppletFocusState_InFocus;
      if (active) {
        resume_splash_loading_cpu_boost();
        api.resume_audio(env, thiz);
        api.resume_soft(env, thiz);
        api.focus_retrieved(env, thiz);
        resuming = 1;
      } else {
        suspend_splash_loading_cpu_boost();
        api.focus_lost(env, thiz);
        api.pause(env, thiz);
        api.pause_audio(env, thiz);
      }
      previous_focus = focus;
    }
    if (focus != AppletFocusState_InFocus) {
      svcSleepThread(16000000ull);
      continue;
    }

    input_update(&input, &api, env, thiz);
    jni_process_soft_keyboard();
    jni_process_deferred_callbacks();
    process_offline_time_sync();
    const jboolean keep_running = api.step(env, thiz);
    if (resuming) resuming = api.resume_step(env, thiz) != 0;
    cursor_renderer_draw(&cursor, &input);
    if (!eglSwapBuffers(graphics.display, graphics.surface)) break;
    if (!keep_running || api.game_requested_quit(env, thiz) ||
        api.game_requested_restart(env, thiz))
      break;
  }

  end_splash_loading_cpu_boost();
  if (input.active_touch)
    send_touch(&api, env, thiz, 1, input.last_x, input.last_y);
  api.focus_lost(env, thiz);
  api.controller_detach(env, thiz, 0);
  api.save_on_exit(env, thiz);
  api.pause(env, thiz);
  api.pause_audio(env, thiz);
  cursor_renderer_shutdown(&cursor);
  graphics_shutdown(&graphics);
  aaudio_shutdown();
  opensles_shutdown();
  SDL_Quit();
  socketExit();
  nifmExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
