#pragma once

#include <vulkan/vulkan.h>

namespace AmdVkPresent
{
void Created(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, HWND hwnd,
             uint32_t graphicsFamily, VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& description);
void Destroyed(VkDevice device, VkSwapchainKHR swapchain);
bool Process(VkQueue queue, VkPresentInfoKHR& present);
}
