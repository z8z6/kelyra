#include <assert.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
  uint32_t s_type;
  const void *next;
  const char *application_name;
  uint32_t application_version;
  const char *engine_name;
  uint32_t engine_version;
  uint32_t api_version;
} ApplicationInfo;

typedef struct {
  uint32_t s_type;
  const void *next;
  uint32_t flags;
  const ApplicationInfo *application_info;
  uint32_t enabled_layer_count;
  const char *const *enabled_layer_names;
  uint32_t enabled_extension_count;
  const char *const *enabled_extension_names;
} InstanceCreateInfo;

int32_t vkEnumerateInstanceVersion(uint32_t *version) {
  *version = (1u << 22) | (3u << 12);
  return 0;
}

int32_t vkCreateInstance(const InstanceCreateInfo *info, const void *allocator,
                         void **instance) {
  assert(info->s_type == 1);
  assert(info->next == NULL);
  assert(info->flags == 0);
  assert(info->application_info != NULL);
  assert(info->application_info->s_type == 0);
  assert(info->application_info->next == NULL);
  assert(info->application_info->application_name[0] == 'K');
  assert(info->application_info->api_version == (1u << 22));
  assert(info->enabled_layer_count == 0);
  assert(info->enabled_layer_names == NULL);
  assert(info->enabled_extension_count == 0);
  assert(info->enabled_extension_names == NULL);
  assert(allocator == NULL);
  *instance = (void *)(uintptr_t)0x1234;
  return 0;
}

int32_t vkEnumeratePhysicalDevices(void *instance, uint32_t *count,
                                    void **devices) {
  assert(instance == (void *)(uintptr_t)0x1234);
  assert(devices == NULL);
  *count = 2;
  return 0;
}

void vkDestroyInstance(void *instance, const void *allocator) {
  assert(instance == (void *)(uintptr_t)0x1234);
  assert(allocator == NULL);
}
