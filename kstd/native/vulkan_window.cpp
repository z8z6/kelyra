#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {
thread_local std::string LastError;

bool Check(VkResult Result, const char *Action) {
  if (Result == VK_SUCCESS)
    return true;
  LastError = std::string(Action) + " failed with VkResult " +
              std::to_string(static_cast<int>(Result));
  return false;
}

struct VulkanTexture {
  VkImage Image = VK_NULL_HANDLE;
  VkDeviceMemory Memory = VK_NULL_HANDLE;
  VkImageView View = VK_NULL_HANDLE;
  VkDescriptorSet DescriptorSet = VK_NULL_HANDLE;
  uint32_t Width = 0;
  uint32_t Height = 0;
};

struct VulkanWindow {
  VkInstance Instance = VK_NULL_HANDLE;
  VkSurfaceKHR Surface = VK_NULL_HANDLE;
  VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
  VkDevice Device = VK_NULL_HANDLE;
  VkQueue Queue = VK_NULL_HANDLE;
  uint32_t QueueFamily = 0;
  VkSwapchainKHR Swapchain = VK_NULL_HANDLE;
  VkFormat Format = VK_FORMAT_UNDEFINED;
  VkExtent2D Extent{};
  std::vector<VkImage> Images;
  std::vector<VkImageView> ImageViews;
  VkRenderPass RenderPass = VK_NULL_HANDLE;
  std::vector<VkFramebuffer> Framebuffers;
  VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
  VkPipeline Pipeline = VK_NULL_HANDLE;
  VkDescriptorSetLayout DescriptorLayout = VK_NULL_HANDLE;
  VkDescriptorPool DescriptorPool = VK_NULL_HANDLE;
  VkSampler Sampler = VK_NULL_HANDLE;
  VkCommandPool CommandPool = VK_NULL_HANDLE;
  VkCommandBuffer Commands = VK_NULL_HANDLE;
  VkFence AcquireFence = VK_NULL_HANDLE;
  uint32_t VertexCount = 0;
  VkBuffer VertexBuffer = VK_NULL_HANDLE;
  VkDeviceMemory VertexMemory = VK_NULL_HANDLE;
  VkBuffer CaptureBuffer = VK_NULL_HANDLE;
  VkDeviceMemory CaptureMemory = VK_NULL_HANDLE;
  bool FrameCaptured = false;
  bool CanCapture = false;
  std::vector<VulkanTexture *> Textures;
  VulkanTexture *CurrentTexture = nullptr;

  void DestroyTexture(VulkanTexture *Texture) {
    if (!Texture)
      return;
    if (Texture->DescriptorSet && DescriptorPool)
      vkFreeDescriptorSets(Device, DescriptorPool, 1, &Texture->DescriptorSet);
    if (Texture->View)
      vkDestroyImageView(Device, Texture->View, nullptr);
    if (Texture->Image)
      vkDestroyImage(Device, Texture->Image, nullptr);
    if (Texture->Memory)
      vkFreeMemory(Device, Texture->Memory, nullptr);
    delete Texture;
  }

  uint32_t MemoryType(uint32_t Allowed, VkMemoryPropertyFlags Required) {
    VkPhysicalDeviceMemoryProperties Properties{};
    vkGetPhysicalDeviceMemoryProperties(PhysicalDevice, &Properties);
    for (uint32_t Index = 0; Index < Properties.memoryTypeCount; ++Index)
      if ((Allowed & (1u << Index)) &&
          (Properties.memoryTypes[Index].propertyFlags & Required) == Required)
        return Index;
    return UINT32_MAX;
  }

  VulkanTexture *CreateTexture(const uint8_t *Pixels, uint32_t Width,
                               uint32_t Height, uint32_t RowPitch) {
    if (!Pixels || !Width || !Height || Width > 16384 || Height > 16384 ||
        RowPitch < static_cast<uint64_t>(Width) * 4 || RowPitch % 4 ||
        static_cast<uint64_t>(RowPitch) * Height > 256ull * 1024 * 1024) {
      LastError = "invalid RGBA8 texture dimensions or row pitch";
      return nullptr;
    }
    VkFormatProperties FormatProperties{};
    vkGetPhysicalDeviceFormatProperties(
        PhysicalDevice, VK_FORMAT_R8G8B8A8_UNORM, &FormatProperties);
    if (!(FormatProperties.optimalTilingFeatures &
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
      LastError = "RGBA8 sampled images are unsupported";
      return nullptr;
    }
    const VkDeviceSize Size = static_cast<VkDeviceSize>(RowPitch) * Height;
    auto *Texture = new VulkanTexture();
    VkBuffer Staging = VK_NULL_HANDLE;
    VkDeviceMemory StagingMemory = VK_NULL_HANDLE;
    VkCommandBuffer Upload = VK_NULL_HANDLE;
    bool Success = [&]() {
      VkBufferCreateInfo BufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      BufferInfo.size = Size;
      BufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      BufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      if (!Check(vkCreateBuffer(Device, &BufferInfo, nullptr, &Staging),
                 "vkCreateBuffer(texture staging)"))
        return false;
      VkMemoryRequirements Requirements{};
      vkGetBufferMemoryRequirements(Device, Staging, &Requirements);
      auto Type = MemoryType(Requirements.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (Type == UINT32_MAX) {
        LastError = "no host-visible coherent texture staging memory";
        return false;
      }
      VkMemoryAllocateInfo Allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      Allocate.allocationSize = Requirements.size;
      Allocate.memoryTypeIndex = Type;
      if (!Check(vkAllocateMemory(Device, &Allocate, nullptr, &StagingMemory),
                 "vkAllocateMemory(texture staging)") ||
          !Check(vkBindBufferMemory(Device, Staging, StagingMemory, 0),
                 "vkBindBufferMemory(texture staging)"))
        return false;
      void *Mapped = nullptr;
      if (!Check(vkMapMemory(Device, StagingMemory, 0, Size, 0, &Mapped),
                 "vkMapMemory(texture staging)"))
        return false;
      std::memcpy(Mapped, Pixels, static_cast<size_t>(Size));
      vkUnmapMemory(Device, StagingMemory);

      VkImageCreateInfo ImageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ImageInfo.imageType = VK_IMAGE_TYPE_2D;
      ImageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
      ImageInfo.extent = {Width, Height, 1};
      ImageInfo.mipLevels = 1;
      ImageInfo.arrayLayers = 1;
      ImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      ImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      ImageInfo.usage =
          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      ImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      ImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (!Check(vkCreateImage(Device, &ImageInfo, nullptr, &Texture->Image),
                 "vkCreateImage(texture)"))
        return false;
      vkGetImageMemoryRequirements(Device, Texture->Image, &Requirements);
      Type = MemoryType(Requirements.memoryTypeBits,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (Type == UINT32_MAX) {
        LastError = "no device-local texture memory";
        return false;
      }
      Allocate.allocationSize = Requirements.size;
      Allocate.memoryTypeIndex = Type;
      if (!Check(vkAllocateMemory(Device, &Allocate, nullptr, &Texture->Memory),
                 "vkAllocateMemory(texture)") ||
          !Check(vkBindImageMemory(Device, Texture->Image, Texture->Memory, 0),
                 "vkBindImageMemory(texture)"))
        return false;
      VkCommandBufferAllocateInfo CommandInfo{
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      CommandInfo.commandPool = CommandPool;
      CommandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      CommandInfo.commandBufferCount = 1;
      if (!Check(vkAllocateCommandBuffers(Device, &CommandInfo, &Upload),
                 "vkAllocateCommandBuffers(texture)"))
        return false;
      VkCommandBufferBeginInfo Begin{
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      Begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      if (!Check(vkBeginCommandBuffer(Upload, &Begin),
                 "vkBeginCommandBuffer(texture)"))
        return false;
      VkImageMemoryBarrier Barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      Barrier.srcAccessMask = 0;
      Barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      Barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      Barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      Barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      Barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      Barrier.image = Texture->Image;
      Barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      Barrier.subresourceRange.levelCount = 1;
      Barrier.subresourceRange.layerCount = 1;
      vkCmdPipelineBarrier(Upload, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &Barrier);
      VkBufferImageCopy Copy{};
      Copy.bufferRowLength = RowPitch / 4;
      Copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      Copy.imageSubresource.layerCount = 1;
      Copy.imageExtent = {Width, Height, 1};
      vkCmdCopyBufferToImage(Upload, Staging, Texture->Image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &Copy);
      Barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      Barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      Barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      Barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      vkCmdPipelineBarrier(Upload, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &Barrier);
      if (!Check(vkEndCommandBuffer(Upload), "vkEndCommandBuffer(texture)"))
        return false;
      VkSubmitInfo Submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      Submit.commandBufferCount = 1;
      Submit.pCommandBuffers = &Upload;
      if (!Check(vkQueueSubmit(Queue, 1, &Submit, VK_NULL_HANDLE),
                 "vkQueueSubmit(texture)") ||
          !Check(vkQueueWaitIdle(Queue), "vkQueueWaitIdle(texture)"))
        return false;
      VkImageViewCreateInfo ViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      ViewInfo.image = Texture->Image;
      ViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      ViewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
      ViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      ViewInfo.subresourceRange.levelCount = 1;
      ViewInfo.subresourceRange.layerCount = 1;
      if (!Check(vkCreateImageView(Device, &ViewInfo, nullptr, &Texture->View),
                 "vkCreateImageView(texture)"))
        return false;
      VkDescriptorSetAllocateInfo SetInfo{
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      SetInfo.descriptorPool = DescriptorPool;
      SetInfo.descriptorSetCount = 1;
      SetInfo.pSetLayouts = &DescriptorLayout;
      if (!Check(vkAllocateDescriptorSets(Device, &SetInfo,
                                          &Texture->DescriptorSet),
                 "vkAllocateDescriptorSets(texture)"))
        return false;
      VkDescriptorImageInfo ImageDescriptor{};
      ImageDescriptor.imageView = Texture->View;
      ImageDescriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      VkDescriptorImageInfo SamplerDescriptor{};
      SamplerDescriptor.sampler = Sampler;
      VkWriteDescriptorSet Writes[2] = {
          {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
          {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
      Writes[0].dstSet = Texture->DescriptorSet;
      Writes[0].dstBinding = 0;
      Writes[0].descriptorCount = 1;
      Writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      Writes[0].pImageInfo = &ImageDescriptor;
      Writes[1].dstSet = Texture->DescriptorSet;
      Writes[1].dstBinding = 1;
      Writes[1].descriptorCount = 1;
      Writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      Writes[1].pImageInfo = &SamplerDescriptor;
      vkUpdateDescriptorSets(Device, 2, Writes, 0, nullptr);
      Texture->Width = Width;
      Texture->Height = Height;
      return true;
    }();
    if (Upload)
      vkFreeCommandBuffers(Device, CommandPool, 1, &Upload);
    if (Staging)
      vkDestroyBuffer(Device, Staging, nullptr);
    if (StagingMemory)
      vkFreeMemory(Device, StagingMemory, nullptr);
    if (!Success) {
      DestroyTexture(Texture);
      return nullptr;
    }
    Textures.push_back(Texture);
    return Texture;
  }

  bool UploadVertices(const float *Vertices, uint32_t Count) {
    if (!Vertices || !Count || Count > 1048576) {
      LastError = "invalid textured vertex buffer";
      return false;
    }
    if (!Check(vkDeviceWaitIdle(Device), "vkDeviceWaitIdle(vertices)"))
      return false;
    if (VertexBuffer)
      vkDestroyBuffer(Device, VertexBuffer, nullptr);
    if (VertexMemory)
      vkFreeMemory(Device, VertexMemory, nullptr);
    VertexBuffer = VK_NULL_HANDLE;
    VertexMemory = VK_NULL_HANDLE;
    VkBufferCreateInfo Info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    Info.size = static_cast<VkDeviceSize>(Count) * sizeof(float) * 4;
    Info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    Info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!Check(vkCreateBuffer(Device, &Info, nullptr, &VertexBuffer),
               "vkCreateBuffer(vertices)"))
      return false;
    VkMemoryRequirements Requirements{};
    vkGetBufferMemoryRequirements(Device, VertexBuffer, &Requirements);
    const auto Type = MemoryType(Requirements.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (Type == UINT32_MAX) {
      LastError = "no host-visible coherent vertex memory";
      return false;
    }
    VkMemoryAllocateInfo Allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    Allocate.allocationSize = Requirements.size;
    Allocate.memoryTypeIndex = Type;
    if (!Check(vkAllocateMemory(Device, &Allocate, nullptr, &VertexMemory),
               "vkAllocateMemory(vertices)") ||
        !Check(vkBindBufferMemory(Device, VertexBuffer, VertexMemory, 0),
               "vkBindBufferMemory(vertices)"))
      return false;
    void *Mapped = nullptr;
    if (!Check(vkMapMemory(Device, VertexMemory, 0, Info.size, 0, &Mapped),
               "vkMapMemory(vertices)"))
      return false;
    std::memcpy(Mapped, Vertices, static_cast<size_t>(Info.size));
    vkUnmapMemory(Device, VertexMemory);
    VertexCount = Count;
    return true;
  }

  ~VulkanWindow() {
    if (Device)
      vkDeviceWaitIdle(Device);
    for (auto *Texture : Textures)
      DestroyTexture(Texture);
    if (VertexBuffer)
      vkDestroyBuffer(Device, VertexBuffer, nullptr);
    if (VertexMemory)
      vkFreeMemory(Device, VertexMemory, nullptr);
    if (DescriptorPool)
      vkDestroyDescriptorPool(Device, DescriptorPool, nullptr);
    if (Sampler)
      vkDestroySampler(Device, Sampler, nullptr);
    if (CaptureBuffer)
      vkDestroyBuffer(Device, CaptureBuffer, nullptr);
    if (CaptureMemory)
      vkFreeMemory(Device, CaptureMemory, nullptr);
    if (AcquireFence)
      vkDestroyFence(Device, AcquireFence, nullptr);
    if (CommandPool)
      vkDestroyCommandPool(Device, CommandPool, nullptr);
    if (Pipeline)
      vkDestroyPipeline(Device, Pipeline, nullptr);
    if (PipelineLayout)
      vkDestroyPipelineLayout(Device, PipelineLayout, nullptr);
    if (DescriptorLayout)
      vkDestroyDescriptorSetLayout(Device, DescriptorLayout, nullptr);
    for (auto Framebuffer : Framebuffers)
      vkDestroyFramebuffer(Device, Framebuffer, nullptr);
    if (RenderPass)
      vkDestroyRenderPass(Device, RenderPass, nullptr);
    for (auto View : ImageViews)
      vkDestroyImageView(Device, View, nullptr);
    if (Swapchain)
      vkDestroySwapchainKHR(Device, Swapchain, nullptr);
    if (Device)
      vkDestroyDevice(Device, nullptr);
    if (Surface)
      vkDestroySurfaceKHR(Instance, Surface, nullptr);
    if (Instance)
      vkDestroyInstance(Instance, nullptr);
  }

  void ReleaseTexture(VulkanTexture *Texture) {
    auto Found = std::find(Textures.begin(), Textures.end(), Texture);
    if (Found == Textures.end())
      return;
    vkDeviceWaitIdle(Device);
    if (CurrentTexture == Texture)
      CurrentTexture = nullptr;
    DestroyTexture(Texture);
    Textures.erase(Found);
  }

  bool SelectTexture(VulkanTexture *Texture) {
    if (Texture && std::find(Textures.begin(), Textures.end(), Texture) ==
                       Textures.end()) {
      LastError = "texture does not belong to this Vulkan context";
      return false;
    }
    CurrentTexture = Texture;
    return true;
  }

  bool Initialize(HWND Window, const uint8_t *Vertex, size_t VertexSize,
                  const uint8_t *Fragment, size_t FragmentSize,
                  uint32_t Count) {
    if (!Window || !Vertex || !Fragment || !VertexSize || !FragmentSize ||
        VertexSize % 4 || FragmentSize % 4 || !Count) {
      LastError = "invalid Vulkan window or shader byte range";
      return false;
    }
    VertexCount = Count;
    const char *InstanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                        VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkApplicationInfo Application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    Application.pApplicationName = "Kelyra";
    Application.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo InstanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    InstanceInfo.pApplicationInfo = &Application;
    InstanceInfo.enabledExtensionCount = 2;
    InstanceInfo.ppEnabledExtensionNames = InstanceExtensions;
    if (!Check(vkCreateInstance(&InstanceInfo, nullptr, &Instance),
               "vkCreateInstance"))
      return false;
    VkWin32SurfaceCreateInfoKHR SurfaceInfo{
        VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    SurfaceInfo.hinstance = GetModuleHandleW(nullptr);
    SurfaceInfo.hwnd = Window;
    if (!Check(
            vkCreateWin32SurfaceKHR(Instance, &SurfaceInfo, nullptr, &Surface),
            "vkCreateWin32SurfaceKHR"))
      return false;

    uint32_t DeviceCount = 0;
    if (!Check(vkEnumeratePhysicalDevices(Instance, &DeviceCount, nullptr),
               "vkEnumeratePhysicalDevices") ||
        DeviceCount == 0) {
      LastError = "no Vulkan physical device";
      return false;
    }
    std::vector<VkPhysicalDevice> Devices(DeviceCount);
    if (!Check(
            vkEnumeratePhysicalDevices(Instance, &DeviceCount, Devices.data()),
            "vkEnumeratePhysicalDevices"))
      return false;
    for (auto Candidate : Devices) {
      uint32_t FamilyCount = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(Candidate, &FamilyCount,
                                               nullptr);
      std::vector<VkQueueFamilyProperties> Families(FamilyCount);
      vkGetPhysicalDeviceQueueFamilyProperties(Candidate, &FamilyCount,
                                               Families.data());
      for (uint32_t Index = 0; Index < FamilyCount; ++Index) {
        VkBool32 Present = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(Candidate, Index, Surface,
                                                 &Present) == VK_SUCCESS &&
            Present && (Families[Index].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
          PhysicalDevice = Candidate;
          QueueFamily = Index;
          break;
        }
      }
      if (PhysicalDevice)
        break;
    }
    if (!PhysicalDevice) {
      LastError = "no Vulkan graphics and presentation queue";
      return false;
    }
    float Priority = 1.0f;
    VkDeviceQueueCreateInfo QueueInfo{
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    QueueInfo.queueFamilyIndex = QueueFamily;
    QueueInfo.queueCount = 1;
    QueueInfo.pQueuePriorities = &Priority;
    const char *DeviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo DeviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    DeviceInfo.queueCreateInfoCount = 1;
    DeviceInfo.pQueueCreateInfos = &QueueInfo;
    DeviceInfo.enabledExtensionCount = 1;
    DeviceInfo.ppEnabledExtensionNames = DeviceExtensions;
    if (!Check(vkCreateDevice(PhysicalDevice, &DeviceInfo, nullptr, &Device),
               "vkCreateDevice"))
      return false;
    vkGetDeviceQueue(Device, QueueFamily, 0, &Queue);

    VkSurfaceCapabilitiesKHR Capabilities{};
    if (!Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                   PhysicalDevice, Surface, &Capabilities),
               "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
      return false;
    uint32_t FormatCount = 0;
    if (!Check(vkGetPhysicalDeviceSurfaceFormatsKHR(PhysicalDevice, Surface,
                                                    &FormatCount, nullptr),
               "vkGetPhysicalDeviceSurfaceFormatsKHR") ||
        FormatCount == 0) {
      LastError = "Vulkan surface has no image formats";
      return false;
    }
    std::vector<VkSurfaceFormatKHR> Formats(FormatCount);
    if (!Check(vkGetPhysicalDeviceSurfaceFormatsKHR(
                   PhysicalDevice, Surface, &FormatCount, Formats.data()),
               "vkGetPhysicalDeviceSurfaceFormatsKHR"))
      return false;
    auto ChosenFormat = Formats.front();
    for (const auto &Candidate : Formats)
      if (Candidate.format == VK_FORMAT_B8G8R8A8_UNORM &&
          Candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        ChosenFormat = Candidate;
        break;
      }
    Format = ChosenFormat.format;
    if (Capabilities.currentExtent.width != UINT32_MAX) {
      Extent = Capabilities.currentExtent;
    } else {
      RECT Client{};
      GetClientRect(Window, &Client);
      Extent.width = static_cast<uint32_t>(Client.right - Client.left);
      Extent.height = static_cast<uint32_t>(Client.bottom - Client.top);
    }
    uint32_t ImageCount = Capabilities.minImageCount + 1;
    if (Capabilities.maxImageCount && ImageCount > Capabilities.maxImageCount)
      ImageCount = Capabilities.maxImageCount;
    VkSwapchainCreateInfoKHR SwapInfo{
        VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    SwapInfo.surface = Surface;
    SwapInfo.minImageCount = ImageCount;
    SwapInfo.imageFormat = Format;
    SwapInfo.imageColorSpace = ChosenFormat.colorSpace;
    SwapInfo.imageExtent = Extent;
    SwapInfo.imageArrayLayers = 1;
    SwapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    CanCapture =
        std::getenv("KSTD_VULKAN_CAPTURE") &&
        (Capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (CanCapture)
      SwapInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    SwapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    SwapInfo.preTransform = Capabilities.currentTransform;
    SwapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    SwapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    SwapInfo.clipped = VK_TRUE;
    if (!Check(vkCreateSwapchainKHR(Device, &SwapInfo, nullptr, &Swapchain),
               "vkCreateSwapchainKHR"))
      return false;
    if (!Check(vkGetSwapchainImagesKHR(Device, Swapchain, &ImageCount, nullptr),
               "vkGetSwapchainImagesKHR"))
      return false;
    Images.resize(ImageCount);
    if (!Check(vkGetSwapchainImagesKHR(Device, Swapchain, &ImageCount,
                                       Images.data()),
               "vkGetSwapchainImagesKHR"))
      return false;
    for (auto Image : Images) {
      VkImageViewCreateInfo ViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      ViewInfo.image = Image;
      ViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      ViewInfo.format = Format;
      ViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      ViewInfo.subresourceRange.levelCount = 1;
      ViewInfo.subresourceRange.layerCount = 1;
      VkImageView View = VK_NULL_HANDLE;
      if (!Check(vkCreateImageView(Device, &ViewInfo, nullptr, &View),
                 "vkCreateImageView"))
        return false;
      ImageViews.push_back(View);
    }

    VkAttachmentDescription Attachment{};
    Attachment.format = Format;
    Attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    Attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    Attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    Attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    Attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    Attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ColorReference{
        0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription Subpass{};
    Subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    Subpass.colorAttachmentCount = 1;
    Subpass.pColorAttachments = &ColorReference;
    VkSubpassDependency Dependency{};
    Dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    Dependency.dstSubpass = 0;
    Dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    Dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    Dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo RenderInfo{
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    RenderInfo.attachmentCount = 1;
    RenderInfo.pAttachments = &Attachment;
    RenderInfo.subpassCount = 1;
    RenderInfo.pSubpasses = &Subpass;
    RenderInfo.dependencyCount = 1;
    RenderInfo.pDependencies = &Dependency;
    if (!Check(vkCreateRenderPass(Device, &RenderInfo, nullptr, &RenderPass),
               "vkCreateRenderPass"))
      return false;
    for (auto View : ImageViews) {
      VkFramebufferCreateInfo FrameInfo{
          VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      FrameInfo.renderPass = RenderPass;
      FrameInfo.attachmentCount = 1;
      FrameInfo.pAttachments = &View;
      FrameInfo.width = Extent.width;
      FrameInfo.height = Extent.height;
      FrameInfo.layers = 1;
      VkFramebuffer Framebuffer = VK_NULL_HANDLE;
      if (!Check(vkCreateFramebuffer(Device, &FrameInfo, nullptr, &Framebuffer),
                 "vkCreateFramebuffer"))
        return false;
      Framebuffers.push_back(Framebuffer);
    }

    auto CreateModule = [&](const uint8_t *Bytes, size_t Size,
                            VkShaderModule *Output) {
      std::vector<uint32_t> Words(Size / 4);
      std::memcpy(Words.data(), Bytes, Size);
      VkShaderModuleCreateInfo Info{
          VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      Info.codeSize = Size;
      Info.pCode = Words.data();
      return Check(vkCreateShaderModule(Device, &Info, nullptr, Output),
                   "vkCreateShaderModule");
    };
    VkShaderModule VertexModule = VK_NULL_HANDLE;
    VkShaderModule FragmentModule = VK_NULL_HANDLE;
    if (!CreateModule(Vertex, VertexSize, &VertexModule) ||
        !CreateModule(Fragment, FragmentSize, &FragmentModule)) {
      if (VertexModule)
        vkDestroyShaderModule(Device, VertexModule, nullptr);
      if (FragmentModule)
        vkDestroyShaderModule(Device, FragmentModule, nullptr);
      return false;
    }
    VkPipelineShaderStageCreateInfo Stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    Stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    Stages[0].module = VertexModule;
    Stages[0].pName = "main";
    Stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    Stages[1].module = FragmentModule;
    Stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo VertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkVertexInputBindingDescription VertexBinding{};
    VertexBinding.binding = 0;
    VertexBinding.stride = sizeof(float) * 4;
    VertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription VertexAttributes[2]{};
    VertexAttributes[0].location = 0;
    VertexAttributes[0].binding = 0;
    VertexAttributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    VertexAttributes[0].offset = 0;
    VertexAttributes[1].location = 1;
    VertexAttributes[1].binding = 0;
    VertexAttributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    VertexAttributes[1].offset = sizeof(float) * 2;
    VertexInput.vertexBindingDescriptionCount = 1;
    VertexInput.pVertexBindingDescriptions = &VertexBinding;
    VertexInput.vertexAttributeDescriptionCount = 2;
    VertexInput.pVertexAttributeDescriptions = VertexAttributes;
    VkPipelineInputAssemblyStateCreateInfo Assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    Assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport Viewport{0.0f,
                        0.0f,
                        static_cast<float>(Extent.width),
                        static_cast<float>(Extent.height),
                        0.0f,
                        1.0f};
    VkRect2D Scissor{{0, 0}, Extent};
    VkPipelineViewportStateCreateInfo ViewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    ViewportState.viewportCount = 1;
    ViewportState.pViewports = &Viewport;
    ViewportState.scissorCount = 1;
    ViewportState.pScissors = &Scissor;
    VkPipelineRasterizationStateCreateInfo Rasterizer{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    Rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    Rasterizer.cullMode = VK_CULL_MODE_NONE;
    Rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    Rasterizer.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo Multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    Multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState Blend{};
    Blend.blendEnable = VK_TRUE;
    Blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    Blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    Blend.colorBlendOp = VK_BLEND_OP_ADD;
    Blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    Blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    Blend.alphaBlendOp = VK_BLEND_OP_ADD;
    Blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo BlendState{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    BlendState.attachmentCount = 1;
    BlendState.pAttachments = &Blend;
    VkDescriptorSetLayoutBinding Bindings[2]{};
    Bindings[0].binding = 0;
    Bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    Bindings[0].descriptorCount = 1;
    Bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    Bindings[1].binding = 1;
    Bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    Bindings[1].descriptorCount = 1;
    Bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo DescriptorInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    DescriptorInfo.bindingCount = 2;
    DescriptorInfo.pBindings = Bindings;
    if (!Check(vkCreateDescriptorSetLayout(Device, &DescriptorInfo, nullptr,
                                           &DescriptorLayout),
               "vkCreateDescriptorSetLayout")) {
      vkDestroyShaderModule(Device, VertexModule, nullptr);
      vkDestroyShaderModule(Device, FragmentModule, nullptr);
      return false;
    }
    VkSamplerCreateInfo SamplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    SamplerInfo.magFilter = VK_FILTER_LINEAR;
    SamplerInfo.minFilter = VK_FILTER_LINEAR;
    SamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    SamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    SamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    SamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    SamplerInfo.maxLod = 0.0f;
    if (!Check(vkCreateSampler(Device, &SamplerInfo, nullptr, &Sampler),
               "vkCreateSampler")) {
      vkDestroyShaderModule(Device, VertexModule, nullptr);
      vkDestroyShaderModule(Device, FragmentModule, nullptr);
      return false;
    }
    VkDescriptorPoolSize PoolSizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64},
                                         {VK_DESCRIPTOR_TYPE_SAMPLER, 64}};
    VkDescriptorPoolCreateInfo DescriptorPoolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    DescriptorPoolInfo.flags =
        VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    DescriptorPoolInfo.maxSets = 64;
    DescriptorPoolInfo.poolSizeCount = 2;
    DescriptorPoolInfo.pPoolSizes = PoolSizes;
    if (!Check(vkCreateDescriptorPool(Device, &DescriptorPoolInfo, nullptr,
                                      &DescriptorPool),
               "vkCreateDescriptorPool")) {
      vkDestroyShaderModule(Device, VertexModule, nullptr);
      vkDestroyShaderModule(Device, FragmentModule, nullptr);
      return false;
    }
    VkPipelineLayoutCreateInfo LayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    LayoutInfo.setLayoutCount = 1;
    LayoutInfo.pSetLayouts = &DescriptorLayout;
    bool Created = Check(
        vkCreatePipelineLayout(Device, &LayoutInfo, nullptr, &PipelineLayout),
        "vkCreatePipelineLayout");
    if (Created) {
      VkGraphicsPipelineCreateInfo PipelineInfo{
          VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      PipelineInfo.stageCount = 2;
      PipelineInfo.pStages = Stages;
      PipelineInfo.pVertexInputState = &VertexInput;
      PipelineInfo.pInputAssemblyState = &Assembly;
      PipelineInfo.pViewportState = &ViewportState;
      PipelineInfo.pRasterizationState = &Rasterizer;
      PipelineInfo.pMultisampleState = &Multisample;
      PipelineInfo.pColorBlendState = &BlendState;
      PipelineInfo.layout = PipelineLayout;
      PipelineInfo.renderPass = RenderPass;
      Created =
          Check(vkCreateGraphicsPipelines(Device, VK_NULL_HANDLE, 1,
                                          &PipelineInfo, nullptr, &Pipeline),
                "vkCreateGraphicsPipelines");
    }
    vkDestroyShaderModule(Device, VertexModule, nullptr);
    vkDestroyShaderModule(Device, FragmentModule, nullptr);
    if (!Created)
      return false;

    VkCommandPoolCreateInfo PoolInfo{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    PoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    PoolInfo.queueFamilyIndex = QueueFamily;
    if (!Check(vkCreateCommandPool(Device, &PoolInfo, nullptr, &CommandPool),
               "vkCreateCommandPool"))
      return false;
    VkCommandBufferAllocateInfo CommandInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    CommandInfo.commandPool = CommandPool;
    CommandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    CommandInfo.commandBufferCount = 1;
    if (!Check(vkAllocateCommandBuffers(Device, &CommandInfo, &Commands),
               "vkAllocateCommandBuffers"))
      return false;
    VkFenceCreateInfo FenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    return Check(vkCreateFence(Device, &FenceInfo, nullptr, &AcquireFence),
                 "vkCreateFence");
  }

  bool BeginCapture(VkImage Image) {
    VkDeviceSize Size =
        static_cast<VkDeviceSize>(Extent.width) * Extent.height * 4;
    VkBufferCreateInfo BufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    BufferInfo.size = Size;
    BufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    BufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!Check(vkCreateBuffer(Device, &BufferInfo, nullptr, &CaptureBuffer),
               "vkCreateBuffer"))
      return false;
    VkMemoryRequirements Requirements{};
    vkGetBufferMemoryRequirements(Device, CaptureBuffer, &Requirements);
    VkPhysicalDeviceMemoryProperties Properties{};
    vkGetPhysicalDeviceMemoryProperties(PhysicalDevice, &Properties);
    uint32_t MemoryType = UINT32_MAX;
    for (uint32_t Index = 0; Index < Properties.memoryTypeCount; ++Index)
      if ((Requirements.memoryTypeBits & (1u << Index)) &&
          (Properties.memoryTypes[Index].propertyFlags &
           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
              (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        MemoryType = Index;
        break;
      }
    if (MemoryType == UINT32_MAX) {
      LastError = "no host-visible Vulkan memory for capture";
      return false;
    }
    VkMemoryAllocateInfo Allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    Allocate.allocationSize = Requirements.size;
    Allocate.memoryTypeIndex = MemoryType;
    if (!Check(vkAllocateMemory(Device, &Allocate, nullptr, &CaptureMemory),
               "vkAllocateMemory") ||
        !Check(vkBindBufferMemory(Device, CaptureBuffer, CaptureMemory, 0),
               "vkBindBufferMemory"))
      return false;
    VkImageMemoryBarrier Barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    Barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    Barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    Barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    Barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    Barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    Barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    Barrier.image = Image;
    Barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    Barrier.subresourceRange.levelCount = 1;
    Barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(
        Commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &Barrier);
    VkBufferImageCopy Copy{};
    Copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    Copy.imageSubresource.layerCount = 1;
    Copy.imageExtent = {Extent.width, Extent.height, 1};
    vkCmdCopyImageToBuffer(Commands, Image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, CaptureBuffer,
                           1, &Copy);
    Barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    Barrier.dstAccessMask = 0;
    Barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    Barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(Commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &Barrier);
    return true;
  }

  bool SaveCapture(const char *Path) {
    void *Pixels = nullptr;
    if (!Check(vkMapMemory(Device, CaptureMemory, 0, VK_WHOLE_SIZE, 0, &Pixels),
               "vkMapMemory"))
      return false;
    std::ofstream Output(Path, std::ios::binary);
    Output << "P6\n" << Extent.width << " " << Extent.height << "\n255\n";
    const auto *Bytes = static_cast<const uint8_t *>(Pixels);
    const bool Bgra =
        Format == VK_FORMAT_B8G8R8A8_UNORM || Format == VK_FORMAT_B8G8R8A8_SRGB;
    for (uint64_t Pixel = 0;
         Pixel < static_cast<uint64_t>(Extent.width) * Extent.height; ++Pixel) {
      const auto *Source = Bytes + Pixel * 4;
      const char Color[3] = {static_cast<char>(Source[Bgra ? 2 : 0]),
                             static_cast<char>(Source[1]),
                             static_cast<char>(Source[Bgra ? 0 : 2])};
      Output.write(Color, 3);
    }
    vkUnmapMemory(Device, CaptureMemory);
    FrameCaptured = true;
    return Output.good();
  }

  int Draw() {
    uint32_t Index = 0;
    auto Result = vkAcquireNextImageKHR(Device, Swapchain, UINT64_MAX,
                                        VK_NULL_HANDLE, AcquireFence, &Index);
    if (Result == VK_ERROR_OUT_OF_DATE_KHR)
      return 2;
    if (Result != VK_SUCCESS && Result != VK_SUBOPTIMAL_KHR) {
      Check(Result, "vkAcquireNextImageKHR");
      return 0;
    }
    if (!Check(vkWaitForFences(Device, 1, &AcquireFence, VK_TRUE, UINT64_MAX),
               "vkWaitForFences") ||
        !Check(vkResetFences(Device, 1, &AcquireFence), "vkResetFences") ||
        !Check(vkResetCommandBuffer(Commands, 0), "vkResetCommandBuffer"))
      return 0;
    VkCommandBufferBeginInfo Begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (!Check(vkBeginCommandBuffer(Commands, &Begin), "vkBeginCommandBuffer"))
      return 0;
    VkClearValue Clear{};
    Clear.color.float32[0] = 0.08f;
    Clear.color.float32[1] = 0.10f;
    Clear.color.float32[2] = 0.18f;
    Clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo Pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    Pass.renderPass = RenderPass;
    Pass.framebuffer = Framebuffers[Index];
    Pass.renderArea.extent = Extent;
    Pass.clearValueCount = 1;
    Pass.pClearValues = &Clear;
    vkCmdBeginRenderPass(Commands, &Pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(Commands, VK_PIPELINE_BIND_POINT_GRAPHICS, Pipeline);
    if (VertexBuffer) {
      VkDeviceSize Offset = 0;
      vkCmdBindVertexBuffers(Commands, 0, 1, &VertexBuffer, &Offset);
    }
    if (CurrentTexture)
      vkCmdBindDescriptorSets(Commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              PipelineLayout, 0, 1,
                              &CurrentTexture->DescriptorSet, 0, nullptr);
    vkCmdDraw(Commands, VertexCount, 1, 0, 0);
    vkCmdEndRenderPass(Commands);
    const char *CapturePath = std::getenv("KSTD_VULKAN_CAPTURE");
    const bool Capture = CapturePath && !FrameCaptured && CanCapture;
    if (Capture && !BeginCapture(Images[Index]))
      return 0;
    if (!Check(vkEndCommandBuffer(Commands), "vkEndCommandBuffer"))
      return 0;
    VkSubmitInfo Submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    Submit.commandBufferCount = 1;
    Submit.pCommandBuffers = &Commands;
    if (!Check(vkQueueSubmit(Queue, 1, &Submit, VK_NULL_HANDLE),
               "vkQueueSubmit") ||
        !Check(vkQueueWaitIdle(Queue), "vkQueueWaitIdle"))
      return 0;
    if (Capture && !SaveCapture(CapturePath))
      return 0;
    VkPresentInfoKHR Present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    Present.swapchainCount = 1;
    Present.pSwapchains = &Swapchain;
    Present.pImageIndices = &Index;
    Result = vkQueuePresentKHR(Queue, &Present);
    if (Result == VK_ERROR_OUT_OF_DATE_KHR || Result == VK_SUBOPTIMAL_KHR)
      return 2;
    return Check(Result, "vkQueuePresentKHR") ? 1 : 0;
  }
};
} // namespace

extern "C" __declspec(dllexport) void *
kstd_vulkan_create(HWND Window, const uint8_t *Vertex, size_t VertexSize,
                   const uint8_t *Fragment, size_t FragmentSize,
                   uint32_t VertexCount) {
  LastError.clear();
  auto *Context = new VulkanWindow();
  if (!Context->Initialize(Window, Vertex, VertexSize, Fragment, FragmentSize,
                           VertexCount)) {
    delete Context;
    return nullptr;
  }
  return Context;
}

extern "C" __declspec(dllexport) int kstd_vulkan_draw(void *Context) {
  return Context ? static_cast<VulkanWindow *>(Context)->Draw() : 0;
}

extern "C" __declspec(dllexport) void *
kstd_vulkan_texture_create(void *Context, const uint8_t *Pixels, uint32_t Width,
                           uint32_t Height, uint32_t RowPitch) {
  return Context ? static_cast<VulkanWindow *>(Context)->CreateTexture(
                       Pixels, Width, Height, RowPitch)
                 : nullptr;
}

extern "C" __declspec(dllexport) int kstd_vulkan_texture_select(void *Context,
                                                                void *Texture) {
  return Context && static_cast<VulkanWindow *>(Context)->SelectTexture(
                        static_cast<VulkanTexture *>(Texture));
}

extern "C" __declspec(dllexport) int
kstd_vulkan_vertices_upload(void *Context, const float *Vertices,
                            uint32_t Count) {
  return Context &&
         static_cast<VulkanWindow *>(Context)->UploadVertices(Vertices, Count);
}

extern "C" __declspec(dllexport) void
kstd_vulkan_texture_destroy(void *Context, void *Texture) {
  if (Context)
    static_cast<VulkanWindow *>(Context)->ReleaseTexture(
        static_cast<VulkanTexture *>(Texture));
}

extern "C" __declspec(dllexport) void kstd_vulkan_destroy(void *Context) {
  delete static_cast<VulkanWindow *>(Context);
}

extern "C" __declspec(dllexport) const char *kstd_vulkan_last_error() {
  return LastError.c_str();
}
