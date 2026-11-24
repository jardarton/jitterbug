/*
 * A Vulkan instance and a device with a graphics queue, with no display.
 * ctest points the loader at Mesa's software driver (JB_LAVAPIPE_ICD), so
 * this runs without a GPU. It tests the machine, not jitterbug: the Vulkan
 * backend's tests (todo.md, "Planned after 0.1") need exactly this to work in
 * the sandbox that `nix flake check` builds in.
 */
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>

#define MAX_DEVICES 8
#define MAX_FAMILIES 16

int main(void)
{
    VkApplicationInfo app = {0};
    VkInstanceCreateInfo instance_info = {0};
    VkInstance instance;
    VkPhysicalDevice devices[MAX_DEVICES];
    VkPhysicalDeviceProperties props;
    VkQueueFamilyProperties families[MAX_FAMILIES];
    VkDeviceQueueCreateInfo queue_info = {0};
    VkDeviceCreateInfo device_info = {0};
    VkDevice device;
    uint32_t device_count = MAX_DEVICES, family_count = MAX_FAMILIES, family;
    const float priority = 1.0f;
    VkResult r;

    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "jitterbug vk_smoke";
    app.apiVersion = VK_API_VERSION_1_1;
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    r = vkCreateInstance(&instance_info, NULL, &instance);
    if (r != VK_SUCCESS) {
        printf("FAIL: vkCreateInstance returned %d\n", (int)r);
        return 1;
    }

    r = vkEnumeratePhysicalDevices(instance, &device_count, devices);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || device_count == 0) {
        printf("FAIL: no Vulkan device (result %d, VK_DRIVER_FILES=%s)\n", (int)r,
               getenv("VK_DRIVER_FILES") ? getenv("VK_DRIVER_FILES") : "unset");
        return 1;
    }
    vkGetPhysicalDeviceProperties(devices[0], &props);

    vkGetPhysicalDeviceQueueFamilyProperties(devices[0], &family_count, families);
    for (family = 0; family < family_count; family++)
        if (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            break;
    if (family == family_count) {
        printf("FAIL: %s has no graphics queue\n", props.deviceName);
        return 1;
    }

    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    r = vkCreateDevice(devices[0], &device_info, NULL, &device);
    if (r != VK_SUCCESS) {
        printf("FAIL: vkCreateDevice returned %d on %s\n", (int)r, props.deviceName);
        return 1;
    }

    printf("ok: %s, Vulkan %u.%u.%u\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
