#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "vk_mem_alloc.h"
#include <stdbool.h>
#include <stdio.h>
#include <vulkan/vulkan.h>

#define ARRAYSIZE(array) (sizeof(array) / sizeof((array)[0]))

#define VK_CHECK(exp)                                                          \
  do {                                                                         \
    VkResult result = exp;                                                     \
    assert(result == VK_SUCCESS);                                              \
  } while (0)

#ifndef NDEBUG
#define ENABLE_VALIDATION
#endif

#ifdef ENABLE_VALIDATION
const char *const REQUIRED_VALIDATION_LAYERS[] = {
    "VK_LAYER_LUNARG_standard_validation",
};
#else
const char *const REQUIRED_VALIDATION_LAYERS[] = {};
#endif

#ifdef ENABLE_VALIDATION
const char *const REQUIRED_INSTANCE_EXTENSIONS[] = {
    VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
};
#else
const char *const REQUIRED_INSTANCE_EXTENSIONS[] = {};
#endif

const char *const REQUIRED_DEVICE_EXTENSIONS[] = {};

typedef union mat4_t {
  float elems[16];
  float columns[4][4];
} mat4_t;

typedef struct camera_uniform_t {
  mat4_t view;
  mat4_t proj;
} camera_uniform_t;

typedef struct cubemap_t {
  VkImage image;
  VmaAllocation allocation;
  VkImageView image_view;
  VkSampler sampler;

  uint32_t width;
  uint32_t height;

  VkFormat format;
} cubemap_t;

// clang-format off
const mat4_t camera_views[6] = {
  (mat4_t){.elems = {
    0.0, 0.0, -1.0, 0.0, 
    0.0, -1.0, -0.0, 0.0,
    -1.0, 0.0, -0.0, 0.0,
    -0.0, -0.0, 0.0, 1.0,
  }},
  (mat4_t){.elems = {
      0.0, 0.0, 1.0, 0.0,
      0.0, -1.0, -0.0, 0.0,
      1.0, 0.0, -0.0, 0.0,
      -0.0, -0.0, 0.0, 1.0,
    }},
  (mat4_t){.elems = {
      1.0, 0.0, -0.0, 0.0,
      0.0, 0.0, -1.0, 0.0,
      0.0, 1.0, -0.0, 0.0,
      -0.0, -0.0, 0.0, 1.0,
    }},
  (mat4_t){.elems = {
      1.0, 0.0, -0.0, 0.0,
      0.0, 0.0, 1.0, 0.0,
      0.0, -1.0, -0.0, 0.0,
      -0.0, -0.0, 0.0, 1.0,
    }},
  (mat4_t){.elems = {
      1.0, 0.0, -0.0, 0.0,
      0.0, -1.0, -0.0, 0.0,
      -0.0, 0.0, -1.0, 0.0,
      -0.0, -0.0, 0.0, 1.0,
    }},
  (mat4_t){.elems = {
      -1.0, 0.0, -0.0, 0.0,
      -0.0, -1.0, -0.0, 0.0,
      -0.0, 0.0, 1.0, 0.0,
      0.0, -0.0, 0.0, 1.0,
    }},
};
// clang-format on

float to_radians(float degrees) {
  return degrees * (3.14159265358979323846 / 180.0f);
}

mat4_t
mat4_perspective(float fovy, float aspect_ratio, float znear, float zfar) {
  mat4_t result = {0};

  float tan_half_fovy = tan(fovy / 2.0f);

  result.columns[0][0] = 1.0f / (aspect_ratio * tan_half_fovy);
  result.columns[1][1] = 1.0f / tan_half_fovy;
  result.columns[2][2] = -(zfar + znear) / (zfar - znear);
  result.columns[2][3] = -1.0f;
  result.columns[3][2] = -(2.0 * zfar * znear) / (zfar - znear);

  return result;
}

VkInstance g_instance = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
VkPhysicalDevice g_physical_device = VK_NULL_HANDLE;

VkDebugReportCallbackEXT g_debug_callback = VK_NULL_HANDLE;

uint32_t g_graphics_queue_family_index = UINT32_MAX;

VkQueue g_graphics_queue = VK_NULL_HANDLE;

VmaAllocator g_gpu_allocator = VK_NULL_HANDLE;

VkCommandPool g_command_pool = VK_NULL_HANDLE;

VkDescriptorPool g_descriptor_pool = VK_NULL_HANDLE;

VkDescriptorSetLayout g_bake_cubemap_descriptor_set_layout = VK_NULL_HANDLE;

unsigned char *load_bytes_from_file(const char *path, size_t *size) {
  FILE *file = fopen(path, "rb");
  if (file == NULL)
    return NULL;

  fseek(file, 0, SEEK_END);
  *size = ftell(file);
  fseek(file, 0, SEEK_SET);

  unsigned char *buffer = (unsigned char *)malloc(*size);

  fread(buffer, *size, 1, file);

  fclose(file);

  return buffer;
}

void set_image_layout(
    VkCommandBuffer command_buffer,
    VkImage image,
    VkImageLayout old_image_layout,
    VkImageLayout new_image_layout,
    VkImageSubresourceRange subresource_range,
    VkPipelineStageFlags src_stage_mask,
    VkPipelineStageFlags dst_stage_mask) {
  // Create an image barrier object
  VkImageMemoryBarrier image_memory_barrier = {};
  image_memory_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  image_memory_barrier.pNext = NULL;
  image_memory_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  image_memory_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  image_memory_barrier.oldLayout = old_image_layout;
  image_memory_barrier.newLayout = new_image_layout;
  image_memory_barrier.image = image;
  image_memory_barrier.subresourceRange = subresource_range;

  // Source layouts (old)
  // Source access mask controls actions that have to be finished on the old
  // layout before it will be transitioned to the new layout
  switch (old_image_layout) {
  case VK_IMAGE_LAYOUT_UNDEFINED:
    // Image layout is undefined (or does not matter)
    // Only valid as initial layout
    // No flags required, listed only for completeness
    image_memory_barrier.srcAccessMask = 0;
    break;

  case VK_IMAGE_LAYOUT_PREINITIALIZED:
    // Image is preinitialized
    // Only valid as initial layout for linear images, preserves memory contents
    // Make sure host writes have been finished
    image_memory_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    break;

  case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    // Image is a color attachment
    // Make sure any writes to the color buffer have been finished
    image_memory_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    break;

  case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    // Image is a depth/stencil attachment
    // Make sure any writes to the depth/stencil buffer have been finished
    image_memory_barrier.srcAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    break;

  case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    // Image is a transfer source
    // Make sure any reads from the image have been finished
    image_memory_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    break;

  case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
    // Image is a transfer destination
    // Make sure any writes to the image have been finished
    image_memory_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    break;

  case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    // Image is read by a shader
    // Make sure any shader reads from the image have been finished
    image_memory_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    break;
  default:
    // Other source layouts aren't handled (yet)
    break;
  }

  // Target layouts (new)
  // Destination access mask controls the dependency for the new image layout
  switch (new_image_layout) {
  case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
    // Image will be used as a transfer destination
    // Make sure any writes to the image have been finished
    image_memory_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    break;

  case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    // Image will be used as a transfer source
    // Make sure any reads from the image have been finished
    image_memory_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    break;

  case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    // Image will be used as a color attachment
    // Make sure any writes to the color buffer have been finished
    image_memory_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    break;

  case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    // Image layout will be used as a depth/stencil attachment
    // Make sure any writes to depth/stencil buffer have been finished
    image_memory_barrier.dstAccessMask =
        image_memory_barrier.dstAccessMask |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    break;

  case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    // Image will be read in a shader (sampler, input attachment)
    // Make sure any writes to the image have been finished
    if (image_memory_barrier.srcAccessMask == 0) {
      image_memory_barrier.srcAccessMask =
          VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    image_memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    break;
  default:
    // Other source layouts aren't handled (yet)
    break;
  }

  vkCmdPipelineBarrier(
      command_buffer,
      src_stage_mask,
      dst_stage_mask,
      0,
      0,
      NULL,
      0,
      NULL,
      1,
      &image_memory_barrier);
}

static inline VkGraphicsPipelineCreateInfo default_pipeline_create_info(
    VkShaderModule vertex_module,
    VkShaderModule fragment_module,
    VkPipelineLayout pipeline_layout,
    VkRenderPass render_pass) {
  static VkPipelineVertexInputStateCreateInfo vertex_input_state =
      (VkPipelineVertexInputStateCreateInfo){
          VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, // sType
          NULL,                                                      // pNext
          0,                                                         // flags
          0,    // vertexBindingDescriptionCount
          NULL, // pVertexBindingDescriptions
          0,    // vertexAttributeDescriptionCount
          NULL, // pVertexAttributeDescriptions
      };

  static VkPipelineInputAssemblyStateCreateInfo input_assembly_state =
      (VkPipelineInputAssemblyStateCreateInfo){
          VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
          NULL,
          0,                                   // flags
          VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, // topology
          VK_FALSE                             // primitiveRestartEnable
      };

  static VkPipelineViewportStateCreateInfo viewport_state =
      (VkPipelineViewportStateCreateInfo){
          VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
          NULL,
          0,    // flags
          1,    // viewportCount
          NULL, // pViewports
          1,    // scissorCount
          NULL  // pScissors
      };

  static VkPipelineRasterizationStateCreateInfo rasterization_state =
      (VkPipelineRasterizationStateCreateInfo){
          VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
          NULL,
          0,                       // flags
          VK_FALSE,                // depthClampEnable
          VK_FALSE,                // rasterizerDiscardEnable
          VK_POLYGON_MODE_FILL,    // polygonMode
          VK_CULL_MODE_BACK_BIT,   // cullMode
          VK_FRONT_FACE_CLOCKWISE, // frontFace
          VK_FALSE,                // depthBiasEnable
          0.0f,                    // depthBiasConstantFactor,
          0.0f,                    // depthBiasClamp
          0.0f,                    // depthBiasSlopeFactor
          1.0f,                    // lineWidth
      };

  static VkPipelineMultisampleStateCreateInfo multisample_state =
      (VkPipelineMultisampleStateCreateInfo){
          VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
          NULL,
          0,                     // flags
          VK_SAMPLE_COUNT_1_BIT, // rasterizationSamples
          VK_FALSE,              // sampleShadingEnable
          0.25f,                 // minSampleShading
          NULL,                  // pSampleMask
          VK_FALSE,              // alphaToCoverageEnable
          VK_FALSE               // alphaToOneEnable
      };

  static VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .pNext = NULL,
      .flags = 0,
      .depthTestEnable = VK_FALSE,
      .depthWriteEnable = VK_FALSE,
      .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
      .depthBoundsTestEnable = VK_FALSE,
      .stencilTestEnable = VK_FALSE,
  };

  static VkPipelineColorBlendAttachmentState color_blend_attachment_state =
      (VkPipelineColorBlendAttachmentState){
          VK_TRUE,                             // blendEnable
          VK_BLEND_FACTOR_SRC_ALPHA,           // srcColorBlendFactor
          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, // dstColorBlendFactor
          VK_BLEND_OP_ADD,                     // colorBlendOp
          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, // srcAlphaBlendFactor
          VK_BLEND_FACTOR_ZERO,                // dstAlphaBlendFactor
          VK_BLEND_OP_ADD,                     // alphaBlendOp
          VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
              VK_COLOR_COMPONENT_B_BIT |
              VK_COLOR_COMPONENT_A_BIT, // colorWriteMask
      };

  static VkPipelineColorBlendStateCreateInfo color_blend_state =
      (VkPipelineColorBlendStateCreateInfo){
          VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
          NULL,
          0,                             // flags
          VK_FALSE,                      // logicOpEnable
          VK_LOGIC_OP_COPY,              // logicOp
          1,                             // attachmentCount
          &color_blend_attachment_state, // pAttachments
          {0.0f, 0.0f, 0.0f, 0.0f},      // blendConstants
      };

  static VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
  };

  static VkPipelineDynamicStateCreateInfo dynamic_state =
      (VkPipelineDynamicStateCreateInfo){
          VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
          NULL,
          0,                                   // flags
          (uint32_t)ARRAYSIZE(dynamic_states), // dynamicStateCount
          dynamic_states,                      // pDyanmicStates
      };

  static VkPipelineShaderStageCreateInfo pipeline_stages[2] = {
      {
          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, // sType
          NULL,                                                // pNext
          0,                                                   // flags
          VK_SHADER_STAGE_VERTEX_BIT,                          // stage
          VK_NULL_HANDLE,                                      // module
          "main",                                              // pName
          NULL, // pSpecializationInfo
      },
      {
          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, // sType
          NULL,                                                // pNext
          0,                                                   // flags
          VK_SHADER_STAGE_FRAGMENT_BIT,                        // stage
          VK_NULL_HANDLE,                                      // module
          "main",                                              // pName
          NULL, // pSpecializationInfo
      },
  };

  pipeline_stages[0].module = vertex_module;
  pipeline_stages[1].module = fragment_module;

  return (VkGraphicsPipelineCreateInfo){
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      NULL,
      0,                          // flags
      ARRAYSIZE(pipeline_stages), // stageCount
      pipeline_stages,            // pStages
      &vertex_input_state,        // pVertexInputState
      &input_assembly_state,      // pInputAssemblyState
      NULL,                       // pTesselationState
      &viewport_state,            // pViewportState
      &rasterization_state,       // pRasterizationState
      &multisample_state,         // multisampleState
      &depth_stencil_state,       // pDepthStencilState
      &color_blend_state,         // pColorBlendState
      &dynamic_state,             // pDynamicState
      pipeline_layout,            // pipelineLayout
      render_pass,                // renderPass
      0,                          // subpass
      0,                          // basePipelineHandle
      -1                          // basePipelineIndex
  };
}

static inline VkCommandBuffer begin_single_time_command_buffer() {
  VkCommandBufferAllocateInfo allocateInfo = (VkCommandBufferAllocateInfo){
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      NULL,
      g_command_pool,                  // commandPool
      VK_COMMAND_BUFFER_LEVEL_PRIMARY, // level
      1,                               // commandBufferCount
  };

  VkCommandBuffer command_buffer;

  VK_CHECK(vkAllocateCommandBuffers(g_device, &allocateInfo, &command_buffer));

  VkCommandBufferBeginInfo commandBufferBeginInfo = (VkCommandBufferBeginInfo){
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      NULL,
      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, // flags
      NULL,                                        // pInheritanceInfo
  };

  VK_CHECK(vkBeginCommandBuffer(command_buffer, &commandBufferBeginInfo));

  return command_buffer;
}

static inline void
end_single_time_command_buffer(VkCommandBuffer command_buffer) {
  VK_CHECK(vkEndCommandBuffer(command_buffer));

  VkSubmitInfo submit_info = (VkSubmitInfo){
      VK_STRUCTURE_TYPE_SUBMIT_INFO,
      NULL,
      0,               // waitSemaphoreCount
      NULL,            // pWaitSemaphores
      NULL,            // pWaitDstStageMask
      1,               // commandBufferCount
      &command_buffer, // pCommandBuffers
      0,               // signalSemaphoreCount
      NULL,            // pSignalSemaphores
  };

  VK_CHECK(vkQueueSubmit(g_graphics_queue, 1, &submit_info, VK_NULL_HANDLE));

  VK_CHECK(vkQueueWaitIdle(g_graphics_queue));

  vkFreeCommandBuffers(g_device, g_command_pool, 1, &command_buffer);
}

static inline void create_buffer(
    VkBuffer *buffer,
    VmaAllocation *allocation,
    size_t size,
    VkBufferUsageFlags buffer_usage,
    VmaMemoryUsage memory_usage,
    VkMemoryPropertyFlags memory_property) {
  VkBufferCreateInfo buffer_create_info = (VkBufferCreateInfo){
      VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      NULL,
      0,                         // flags
      size,                      // size
      buffer_usage,              // usage
      VK_SHARING_MODE_EXCLUSIVE, // sharingMode
      0,                         // queueFamilyIndexCount
      NULL                       // pQueueFamilyIndices
  };

  VmaAllocationCreateInfo alloc_info = {};
  alloc_info.usage = memory_usage;
  alloc_info.requiredFlags = memory_property;

  VK_CHECK(vmaCreateBuffer(
      g_gpu_allocator,
      &buffer_create_info,
      &alloc_info,
      buffer,
      allocation,
      NULL));
}

/*
 *
 * Vulkan setup stuff
 *
 */

// Debug callback

// Ignore warnings for this function
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugReportFlagsEXT flags,
    VkDebugReportObjectTypeEXT objType,
    uint64_t obj,
    size_t location,
    int32_t code,
    const char *layerPrefix,
    const char *msg,
    void *userData) {
  printf("Validation layer: %s\n", msg);

  return VK_FALSE;
}
#pragma GCC diagnostic pop

VkResult CreateDebugReportCallbackEXT(
    VkInstance instance,
    const VkDebugReportCallbackCreateInfoEXT *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkDebugReportCallbackEXT *pCallback) {
  PFN_vkCreateDebugReportCallbackEXT func =
      (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(
          instance, "vkCreateDebugReportCallbackEXT");
  if (func != NULL) {
    return func(instance, pCreateInfo, pAllocator, pCallback);
  } else {
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  }
}

void DestroyDebugReportCallbackEXT(
    VkInstance instance,
    VkDebugReportCallbackEXT callback,
    const VkAllocationCallbacks *pAllocator) {
  PFN_vkDestroyDebugReportCallbackEXT func =
      (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(
          instance, "vkDestroyDebugReportCallbackEXT");
  if (func != NULL) {
    func(instance, callback, pAllocator);
  }
}

static inline bool check_validation_layer_support() {
  uint32_t count;
  vkEnumerateInstanceLayerProperties(&count, NULL);
  VkLayerProperties *available_layers =
      malloc(sizeof(VkLayerProperties) * count);
  vkEnumerateInstanceLayerProperties(&count, available_layers);

  for (uint32_t l = 0; l < ARRAYSIZE(REQUIRED_VALIDATION_LAYERS); l++) {
    const char *layer_name = REQUIRED_VALIDATION_LAYERS[l];
    bool layer_found = false;

    for (uint32_t i = 0; i < count; i++) {
      if (strcmp(available_layers[i].layerName, layer_name) == 0) {
        layer_found = true;
        break;
      }
    }

    if (!layer_found) {
      free(available_layers);
      return false;
    }
  }

  free(available_layers);
  return true;
}

static inline bool
check_physical_device_properties(VkPhysicalDevice physical_device) {
  uint32_t extension_count;
  vkEnumerateDeviceExtensionProperties(
      physical_device, NULL, &extension_count, NULL);
  VkExtensionProperties *available_extensions =
      malloc(sizeof(VkExtensionProperties) * extension_count);
  vkEnumerateDeviceExtensionProperties(
      physical_device, NULL, &extension_count, available_extensions);

  VkPhysicalDeviceProperties device_properties;
  vkGetPhysicalDeviceProperties(physical_device, &device_properties);

  for (uint32_t i = 0; i < ARRAYSIZE(REQUIRED_DEVICE_EXTENSIONS); i++) {
    const char *required_extension = REQUIRED_DEVICE_EXTENSIONS[i];
    bool found = false;
    for (uint32_t i = 0; i < extension_count; i++) {
      if (strcmp(required_extension, available_extensions[i].extensionName) ==
          0) {
        found = true;
      }
    }

    if (!found) {
      printf(
          "Physical device %s doesn't support extension named \"%s\"\n",
          device_properties.deviceName,
          required_extension);
      free(available_extensions);
      return false;
    }
  }

  free(available_extensions);

  uint32_t major_version = VK_VERSION_MAJOR(device_properties.apiVersion);

  if (major_version < 1 &&
      device_properties.limits.maxImageDimension2D < 4096) {
    printf(
        "Physical device %s doesn't support required parameters!\n",
        device_properties.deviceName);
    return false;
  }

  // Check for required device features
  VkPhysicalDeviceFeatures features = {};
  vkGetPhysicalDeviceFeatures(physical_device, &features);
  if (!features.wideLines) {
    printf(
        "Physical device %s doesn't support required features!\n",
        device_properties.deviceName);
    return false;
  }

  uint32_t queue_family_prop_count;
  vkGetPhysicalDeviceQueueFamilyProperties(
      physical_device, &queue_family_prop_count, NULL);
  VkQueueFamilyProperties *queue_family_properties =
      malloc(sizeof(VkQueueFamilyProperties) * queue_family_prop_count);
  vkGetPhysicalDeviceQueueFamilyProperties(
      physical_device, &queue_family_prop_count, queue_family_properties);

  g_graphics_queue_family_index = UINT32_MAX;

  for (uint32_t i = 0; i < queue_family_prop_count; i++) {
    if (queue_family_properties[i].queueCount > 0 &&
        queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      if (g_graphics_queue_family_index == UINT32_MAX) {
        g_graphics_queue_family_index = i;
      }
    }
  }

  free(queue_family_properties);

  if (g_graphics_queue_family_index == UINT32_MAX) {
    printf(
        "Could not find queue family with requested properties on physical "
        "device %s\n",
        device_properties.deviceName);

    return false;
  }

  return true;
}

static inline void create_instance() {
#ifdef ENABLE_VALIDATION
  if (check_validation_layer_support()) {
    printf("Using validation layers\n");
  } else {
    printf("Validation layers requested but not available\n");
  }
#endif

  VkApplicationInfo appInfo = {};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pNext = NULL;
  appInfo.pApplicationName = "IBL Baker";
  appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  appInfo.pEngineName = "No engine";
  appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  appInfo.apiVersion = VK_API_VERSION_1_0;

  VkInstanceCreateInfo createInfo = {};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.flags = 0;
  createInfo.pApplicationInfo = &appInfo;

  createInfo.enabledLayerCount = 0;
  createInfo.ppEnabledLayerNames = NULL;

#ifdef ENABLE_VALIDATION
  if (check_validation_layer_support()) {
    createInfo.enabledLayerCount =
        (uint32_t)ARRAYSIZE(REQUIRED_VALIDATION_LAYERS);
    createInfo.ppEnabledLayerNames = REQUIRED_VALIDATION_LAYERS;
  }
#endif

  // Set required instance extensions
  createInfo.enabledExtensionCount = ARRAYSIZE(REQUIRED_INSTANCE_EXTENSIONS);
  createInfo.ppEnabledExtensionNames = REQUIRED_INSTANCE_EXTENSIONS;

  VK_CHECK(vkCreateInstance(&createInfo, NULL, &g_instance));
}

static inline void setup_debug_callback() {
  VkDebugReportCallbackCreateInfoEXT createInfo = {};
  createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
  createInfo.flags =
      VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT;
  createInfo.pfnCallback = debug_callback;

  VK_CHECK(CreateDebugReportCallbackEXT(
      g_instance, &createInfo, NULL, &g_debug_callback));
}

static inline void create_device() {
  uint32_t physical_device_count;
  vkEnumeratePhysicalDevices(g_instance, &physical_device_count, NULL);
  VkPhysicalDevice *physical_devices =
      malloc(sizeof(VkPhysicalDevice) * physical_device_count);
  vkEnumeratePhysicalDevices(
      g_instance, &physical_device_count, physical_devices);

  for (uint32_t i = 0; i < physical_device_count; i++) {
    if (check_physical_device_properties(physical_devices[i])) {
      g_physical_device = physical_devices[i];
      break;
    }
  }

  free(physical_devices);

  if (!g_physical_device) {
    printf("Could not select physical device based on chosen properties\n");
    abort();
  }

  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(g_physical_device, &properties);

  printf("Using physical device: %s\n", properties.deviceName);

  uint32_t queue_create_info_count = 0;
  VkDeviceQueueCreateInfo queue_create_infos[1] = {};
  float queue_priorities[] = {1.0f};

  queue_create_infos[queue_create_info_count++] = (VkDeviceQueueCreateInfo){
      VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      NULL,
      0,
      g_graphics_queue_family_index,
      (uint32_t)ARRAYSIZE(queue_priorities),
      queue_priorities,
  };

  VkDeviceCreateInfo deviceCreateInfo = {};
  deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  deviceCreateInfo.flags = 0;
  deviceCreateInfo.queueCreateInfoCount = queue_create_info_count;
  deviceCreateInfo.pQueueCreateInfos = queue_create_infos;

  deviceCreateInfo.enabledLayerCount = 0;
  deviceCreateInfo.ppEnabledLayerNames = NULL;

  // Validation layer stuff
#ifdef ENABLE_VALIDATION
  if (check_validation_layer_support()) {
    deviceCreateInfo.enabledLayerCount =
        (uint32_t)ARRAYSIZE(REQUIRED_VALIDATION_LAYERS);
    deviceCreateInfo.ppEnabledLayerNames = REQUIRED_VALIDATION_LAYERS;
  }
#endif

  deviceCreateInfo.enabledExtensionCount =
      (uint32_t)ARRAYSIZE(REQUIRED_DEVICE_EXTENSIONS);
  deviceCreateInfo.ppEnabledExtensionNames = REQUIRED_DEVICE_EXTENSIONS;

  // Enable all features
  VkPhysicalDeviceFeatures features;
  vkGetPhysicalDeviceFeatures(g_physical_device, &features);
  deviceCreateInfo.pEnabledFeatures = &features;

  VK_CHECK(
      vkCreateDevice(g_physical_device, &deviceCreateInfo, NULL, &g_device));
}

static inline void get_device_queues() {
  vkGetDeviceQueue(
      g_device, g_graphics_queue_family_index, 0, &g_graphics_queue);
}

static inline void setup_memory_allocator() {
  VmaAllocatorCreateInfo allocatorInfo = {};
  allocatorInfo.physicalDevice = g_physical_device;
  allocatorInfo.device = g_device;

  VK_CHECK(vmaCreateAllocator(&allocatorInfo, &g_gpu_allocator));
}

static inline void create_command_pool() {
  VkCommandPoolCreateInfo createInfo = {};
  createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  createInfo.pNext = 0;
  createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  createInfo.queueFamilyIndex = g_graphics_queue_family_index;

  VK_CHECK(vkCreateCommandPool(g_device, &createInfo, NULL, &g_command_pool));
}

static inline void create_descriptor_pool() {
  VkDescriptorPoolSize pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10},
  };

  VkDescriptorPoolCreateInfo create_info = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,     // sType
      NULL,                                              // pNext
      VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, // flags
      1000 * (uint32_t)ARRAYSIZE(pool_sizes),            // maxSets
      (uint32_t)ARRAYSIZE(pool_sizes),                   // poolSizeCount
      pool_sizes,                                        // pPoolSizes
  };

  VK_CHECK(
      vkCreateDescriptorPool(g_device, &create_info, NULL, &g_descriptor_pool));
}

static inline void create_descriptor_set_layout() {
  VkDescriptorSetLayoutBinding bindings[] = {{
      0,                                         // binding
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // descriptorType
      1,                                         // descriptorCount
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, // stageFlags
      NULL, // pImmutableSamplers
  }};

  VkDescriptorSetLayoutCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  create_info.pNext = NULL;
  create_info.flags = 0;
  create_info.bindingCount = ARRAYSIZE(bindings);
  create_info.pBindings = bindings;

  vkCreateDescriptorSetLayout(
      g_device, &create_info, NULL, &g_bake_cubemap_descriptor_set_layout);
}

static inline void vulkan_setup() {
  create_instance();

#ifdef ENABLE_VALIDATION
  if (check_validation_layer_support()) {
    setup_debug_callback();
  }
#endif

  create_device();
  get_device_queues();
  setup_memory_allocator();
  create_command_pool();
  create_descriptor_pool();
  create_descriptor_set_layout();
}

static inline void vulkan_teardown() {
  VK_CHECK(vkDeviceWaitIdle(g_device));

  vkDestroyDescriptorPool(g_device, g_descriptor_pool, NULL);

  vkDestroyDescriptorSetLayout(
      g_device, g_bake_cubemap_descriptor_set_layout, NULL);

  vkDestroyCommandPool(g_device, g_command_pool, NULL);

  vmaDestroyAllocator(g_gpu_allocator);
  vkDestroyDevice(g_device, NULL);
  DestroyDebugReportCallbackEXT(g_instance, g_debug_callback, NULL);
  vkDestroyInstance(g_instance, NULL);
}

/*
 *
 * Canvas stuff
 *
 */

typedef struct canvas_t {
  uint32_t width;
  uint32_t height;

  VkRenderPass render_pass;

  VkFormat color_format;

  VkImage image;
  VmaAllocation allocation;
  VkSampler sampler;
  VkImageView image_view;

  VkFramebuffer framebuffer;
} canvas_t;

static inline void create_color_target(canvas_t *canvas) {
  VkImageCreateInfo imageCreateInfo = (VkImageCreateInfo){
      VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      NULL,
      0,                    // flags
      VK_IMAGE_TYPE_2D,     // imageType
      canvas->color_format, // format
      {
          canvas->width,       // width
          canvas->height,      // height
          1,                   // depth
      },                       // extent
      1,                       // mipLevels
      1,                       // arrayLayers
      VK_SAMPLE_COUNT_1_BIT,   // samples
      VK_IMAGE_TILING_OPTIMAL, // tiling
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
          VK_IMAGE_USAGE_SAMPLED_BIT |
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, // usage
      VK_SHARING_MODE_EXCLUSIVE,               // sharingMode
      1,                                       // queueFamilyIndexCount
      &g_graphics_queue_family_index,          // pQueueFamilyIndices
      VK_IMAGE_LAYOUT_UNDEFINED,               // initialLayout
  };

  VmaAllocationCreateInfo imageAllocCreateInfo = {};
  imageAllocCreateInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

  VK_CHECK(vmaCreateImage(
      g_gpu_allocator,
      &imageCreateInfo,
      &imageAllocCreateInfo,
      &canvas->image,
      &canvas->allocation,
      NULL));

  VkImageViewCreateInfo imageViewCreateInfo = (VkImageViewCreateInfo){
      VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      NULL,
      0,                     // flags
      canvas->image,         // image
      VK_IMAGE_VIEW_TYPE_2D, // viewType
      canvas->color_format,  // format
      {
          VK_COMPONENT_SWIZZLE_IDENTITY, // r
          VK_COMPONENT_SWIZZLE_IDENTITY, // g
          VK_COMPONENT_SWIZZLE_IDENTITY, // b
          VK_COMPONENT_SWIZZLE_IDENTITY, // a
      },                                 // components
      {
          VK_IMAGE_ASPECT_COLOR_BIT, // aspectMask
          0,                         // baseMipLevel
          1,                         // levelCount
          0,                         // baseArrayLayer
          1,                         // layerCount
      },                             // subresourceRange
  };

  VK_CHECK(vkCreateImageView(
      g_device, &imageViewCreateInfo, NULL, &canvas->image_view));

  VkSamplerCreateInfo samplerCreateInfo = (VkSamplerCreateInfo){
      VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      NULL,
      0,                                       // flags
      VK_FILTER_LINEAR,                        // magFilter
      VK_FILTER_LINEAR,                        // minFilter
      VK_SAMPLER_MIPMAP_MODE_LINEAR,           // mipmapMode
      VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, // addressModeU
      VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, // addressModeV
      VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, // addressModeW
      0.0f,                                    // mipLodBias
      VK_FALSE,                                // anisotropyEnable
      1.0f,                                    // maxAnisotropy
      VK_FALSE,                                // compareEnable
      VK_COMPARE_OP_NEVER,                     // compareOp
      0.0f,                                    // minLod
      0.0f,                                    // maxLod
      VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, // borderColor
      VK_FALSE,                                // unnormalizedCoordinates
  };

  VK_CHECK(
      vkCreateSampler(g_device, &samplerCreateInfo, NULL, &canvas->sampler));
}

static inline void destroy_color_target(canvas_t *canvas) {
  VK_CHECK(vkDeviceWaitIdle(g_device));

  if (canvas->image != VK_NULL_HANDLE) {
    vkDestroyImageView(g_device, canvas->image_view, NULL);
  }

  if (canvas->sampler != VK_NULL_HANDLE) {
    vkDestroySampler(g_device, canvas->sampler, NULL);
  }

  if (canvas->image != VK_NULL_HANDLE) {
    vmaDestroyImage(g_gpu_allocator, canvas->image, canvas->allocation);
  }

  canvas->image = VK_NULL_HANDLE;
  canvas->allocation = VK_NULL_HANDLE;
  canvas->image_view = VK_NULL_HANDLE;
  canvas->sampler = VK_NULL_HANDLE;
}

static inline void create_framebuffer(canvas_t *canvas) {
  VkImageView attachments[] = {
      canvas->image_view,
  };

  VkFramebufferCreateInfo createInfo = {
      VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, // sType
      NULL,                                      // pNext
      0,                                         // flags
      canvas->render_pass,                       // renderPass
      (uint32_t)ARRAYSIZE(attachments),          // attachmentCount
      attachments,                               // pAttachments
      canvas->width,                             // width
      canvas->height,                            // height
      1,                                         // layers
  };

  VK_CHECK(
      vkCreateFramebuffer(g_device, &createInfo, NULL, &canvas->framebuffer));
}

static inline void destroy_framebuffer(canvas_t *canvas) {
  VK_CHECK(vkDeviceWaitIdle(g_device));
  vkDestroyFramebuffer(g_device, canvas->framebuffer, NULL);
}

static inline void create_render_pass(canvas_t *canvas) {
  VkAttachmentDescription attachmentDescriptions[] = {
      // Resolved color attachment
      (VkAttachmentDescription){
          0,                                        // flags
          canvas->color_format,                     // format
          VK_SAMPLE_COUNT_1_BIT,                    // samples
          VK_ATTACHMENT_LOAD_OP_CLEAR,              // loadOp
          VK_ATTACHMENT_STORE_OP_STORE,             // storeOp
          VK_ATTACHMENT_LOAD_OP_DONT_CARE,          // stencilLoadOp
          VK_ATTACHMENT_STORE_OP_DONT_CARE,         // stencilStoreOp
          VK_IMAGE_LAYOUT_UNDEFINED,                // initialLayout
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, // finalLayout
      },
  };

  VkAttachmentReference colorAttachmentReference = {
      0,                                        // attachment
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, // layout
  };

  VkSubpassDescription subpassDescription = {
      0,                               // flags
      VK_PIPELINE_BIND_POINT_GRAPHICS, // pipelineBindPoint
      0,                               // inputAttachmentCount
      NULL,                            // pInputAttachments
      1,                               // colorAttachmentCount
      &colorAttachmentReference,       // pColorAttachments
      NULL,                            // pResolveAttachments
      NULL,                            // pDepthStencilAttachment
      0,                               // preserveAttachmentCount
      NULL,                            // pPreserveAttachments
  };

  VkSubpassDependency dependencies[] = {
      (VkSubpassDependency){
          VK_SUBPASS_EXTERNAL,                           // srcSubpass
          0,                                             // dstSubpass
          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,          // srcStageMask
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, // dstStageMask
          VK_ACCESS_MEMORY_READ_BIT,                     // srcAccessMask
          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, // dstAccessMask
          VK_DEPENDENCY_BY_REGION_BIT,              // dependencyFlags
      },
      (VkSubpassDependency){
          0,                                             // srcSubpass
          VK_SUBPASS_EXTERNAL,                           // dstSubpass
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, // srcStageMask
          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,          // dstStageMask
          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, // srcAccessMask
          VK_ACCESS_MEMORY_READ_BIT,                // dstAccessMask
          VK_DEPENDENCY_BY_REGION_BIT,              // dependencyFlags
      },
  };

  VkRenderPassCreateInfo renderPassCreateInfo = {
      VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,   // sType
      NULL,                                        // pNext
      0,                                           // flags
      (uint32_t)ARRAYSIZE(attachmentDescriptions), // attachmentCount
      attachmentDescriptions,                      // pAttachments
      1,                                           // subpassCount
      &subpassDescription,                         // pSubpasses
      (uint32_t)ARRAYSIZE(dependencies),           // dependencyCount
      dependencies,                                // pDependencies
  };

  VK_CHECK(vkCreateRenderPass(
      g_device, &renderPassCreateInfo, NULL, &canvas->render_pass));
}

static inline void destroy_render_pass(canvas_t *canvas) {
  VK_CHECK(vkDeviceWaitIdle(g_device));
  vkDestroyRenderPass(g_device, canvas->render_pass, NULL);
}

void canvas_init(
    canvas_t *canvas,
    const uint32_t width,
    const uint32_t height,
    const VkFormat color_format) {
  canvas->width = width;
  canvas->height = height;
  canvas->color_format = color_format;

  create_color_target(canvas);
  create_render_pass(canvas);
  create_framebuffer(canvas);
}

void canvas_begin(canvas_t *canvas, const VkCommandBuffer command_buffer) {
  VkClearValue clearValues[2] = {};
  clearValues[0].color = (VkClearColorValue){{0.0f, 0.0f, 0.0f, 1.0f}};
  clearValues[1].depthStencil = (VkClearDepthStencilValue){1.0f, 0};

  VkRenderPassBeginInfo renderPassBeginInfo = {
      VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,  // sType
      NULL,                                      // pNext
      canvas->render_pass,                       // renderPass
      canvas->framebuffer,                       // framebuffer
      {{0, 0}, {canvas->width, canvas->height}}, // renderArea
      ARRAYSIZE(clearValues),                    // clearValueCount
      clearValues,                               // pClearValues
  };

  vkCmdBeginRenderPass(
      command_buffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport viewport = (VkViewport){
      0.0f,                  // x
      0.0f,                  // y
      (float)canvas->width,  // width
      (float)canvas->height, // height
      0.0f,                  // minDepth
      1.0f,                  // maxDepth
  };

  vkCmdSetViewport(command_buffer, 0, 1, &viewport);

  VkRect2D scissor = (VkRect2D){{0, 0}, {canvas->width, canvas->height}};

  vkCmdSetScissor(command_buffer, 0, 1, &scissor);
}

void canvas_end(canvas_t *canvas, const VkCommandBuffer command_buffer) {
  vkCmdEndRenderPass(command_buffer);
}

void canvas_destroy(canvas_t *canvas) {
  destroy_framebuffer(canvas);
  destroy_render_pass(canvas);
  destroy_color_target(canvas);
}

/*
 *
 * Cubemap stuff
 *
 */

static void create_image_and_image_view(
    VkImage *image,
    VmaAllocation *allocation,
    VkImageView *image_view,
    VkFormat format,
    uint32_t width,
    uint32_t height,
    VkImageUsageFlags usage) {
  VkImageCreateInfo image_create_info = {
      VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      NULL,
      0,                // flags
      VK_IMAGE_TYPE_2D, // imageType
      format,           // format
      {
          width,                      // width
          height,                     // height
          1,                          // depth
      },                              // extent
      1,                              // mipLevels
      1,                              // arrayLayers
      VK_SAMPLE_COUNT_1_BIT,          // samples
      VK_IMAGE_TILING_OPTIMAL,        // tiling
      usage,                          // usage
      VK_SHARING_MODE_EXCLUSIVE,      // sharingMode
      1,                              // queueFamilyIndexCount
      &g_graphics_queue_family_index, // pQueueFamilyIndices
      VK_IMAGE_LAYOUT_UNDEFINED,      // initialLayout
  };

  VmaAllocationCreateInfo image_alloc_create_info = {};
  image_alloc_create_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;

  VK_CHECK(vmaCreateImage(
      g_gpu_allocator,
      &image_create_info,
      &image_alloc_create_info,
      image,
      allocation,
      NULL));

  VkImageViewCreateInfo image_view_create_info = {
      VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      NULL,
      0,                     // flags
      *image,                // image
      VK_IMAGE_VIEW_TYPE_2D, // viewType
      format,                // format
      {
          VK_COMPONENT_SWIZZLE_IDENTITY, // r
          VK_COMPONENT_SWIZZLE_IDENTITY, // g
          VK_COMPONENT_SWIZZLE_IDENTITY, // b
          VK_COMPONENT_SWIZZLE_IDENTITY, // a
      },                                 // components
      {
          VK_IMAGE_ASPECT_COLOR_BIT, // aspectMask
          0,                         // baseMipLevel
          1,                         // levelCount
          0,                         // baseArrayLayer
          1,                         // layerCount
      },                             // subresourceRange
  };

  VK_CHECK(
      vkCreateImageView(g_device, &image_view_create_info, NULL, image_view));
}

static void create_sampler(VkSampler *sampler) {
  VkSamplerCreateInfo sampler_create_info = {
      VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      NULL,
      0,                                       // flags
      VK_FILTER_LINEAR,                        // magFilter
      VK_FILTER_LINEAR,                        // minFilter
      VK_SAMPLER_MIPMAP_MODE_LINEAR,           // mipmapMode
      VK_SAMPLER_ADDRESS_MODE_REPEAT,          // addressModeU
      VK_SAMPLER_ADDRESS_MODE_REPEAT,          // addressModeV
      VK_SAMPLER_ADDRESS_MODE_REPEAT,          // addressModeW
      0.0f,                                    // mipLodBias
      VK_FALSE,                                // anisotropyEnable
      1.0f,                                    // maxAnisotropy
      VK_FALSE,                                // compareEnable
      VK_COMPARE_OP_NEVER,                     // compareOp
      0.0f,                                    // minLod
      0.0f,                                    // maxLod
      VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, // borderColor
      VK_FALSE,                                // unnormalizedCoordinates
  };

  VK_CHECK(vkCreateSampler(g_device, &sampler_create_info, NULL, sampler));
}

static void copy_side_image_to_cubemap(
    VkCommandBuffer command_buffer,
    VkImage side_image,
    cubemap_t *cubemap,
    uint32_t layer,
    uint32_t level) {
  VkImageSubresourceRange side_image_subresource_range = {};
  side_image_subresource_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  side_image_subresource_range.baseMipLevel = 0;
  side_image_subresource_range.levelCount = 1;
  side_image_subresource_range.baseArrayLayer = 0;
  side_image_subresource_range.layerCount = 1;

  set_image_layout(
      command_buffer,
      side_image,
      VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      side_image_subresource_range,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

  VkImageSubresourceRange cube_face_subresource_range = {};
  cube_face_subresource_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  cube_face_subresource_range.baseMipLevel = level;
  cube_face_subresource_range.levelCount = 1;
  cube_face_subresource_range.baseArrayLayer = layer;
  cube_face_subresource_range.layerCount = 1;

  set_image_layout(
      command_buffer,
      cubemap->image,
      VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      cube_face_subresource_range,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

  VkImageCopy copy_region = {};

  copy_region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy_region.srcSubresource.baseArrayLayer = 0;
  copy_region.srcSubresource.mipLevel = 0;
  copy_region.srcSubresource.layerCount = 1;
  copy_region.srcOffset = (VkOffset3D){0, 0, 0};

  copy_region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy_region.dstSubresource.baseArrayLayer = layer;
  copy_region.dstSubresource.mipLevel = level;
  copy_region.dstSubresource.layerCount = 1;
  copy_region.dstOffset = (VkOffset3D){0, 0, 0};

  copy_region.extent.width = cubemap->width;
  copy_region.extent.height = cubemap->height;
  copy_region.extent.depth = 1;

  // Put image copy into command buffer
  vkCmdCopyImage(
      command_buffer,
      side_image,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      cubemap->image,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      1,
      &copy_region);

  // Transform framebuffer color attachment back
  set_image_layout(
      command_buffer,
      side_image,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      side_image_subresource_range,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

  // Change image layout of copied face to shader read
  set_image_layout(
      command_buffer,
      cubemap->image,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      cube_face_subresource_range,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
}

static void render_equirec_to_cubemap(
    const char *hdr_file,
    cubemap_t *dest_cubemap,
    uint32_t level,
    const char *vert_path,
    const char *frag_path) {
  // Load HDR image
  int hdr_width, hdr_height, nr_components;
  float *hdr_data =
      stbi_loadf(hdr_file, &hdr_width, &hdr_height, &nr_components, 4);

  assert(hdr_data != NULL);

  // Create HDR VkImage and stuff
  VkImage hdr_image = VK_NULL_HANDLE;
  VmaAllocation hdr_allocation = VK_NULL_HANDLE;
  VkImageView hdr_image_view = VK_NULL_HANDLE;
  VkSampler hdr_sampler = VK_NULL_HANDLE;

  create_image_and_image_view(
      &hdr_image,
      &hdr_allocation,
      &hdr_image_view,
      dest_cubemap->format,
      (uint32_t)hdr_width,
      (uint32_t)hdr_height,
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

  create_sampler(&hdr_sampler);

  // Upload data to image
  {
    size_t hdr_size = hdr_width * hdr_height * 4 * sizeof(float);

    VkBuffer staging_buffer;
    VmaAllocation staging_allocation;

    create_buffer(
        &staging_buffer,
        &staging_allocation,
        hdr_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_CPU_ONLY,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    void *stagingMemoryPointer;
    vmaMapMemory(g_gpu_allocator, staging_allocation, &stagingMemoryPointer);
    memcpy(stagingMemoryPointer, hdr_data, hdr_size);

    VkCommandBuffer command_buffer = begin_single_time_command_buffer();

    VkImageSubresourceRange subresource_range = {};
    subresource_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subresource_range.baseMipLevel = 0;
    subresource_range.levelCount = 1;
    subresource_range.baseArrayLayer = 0;
    subresource_range.layerCount = 1;

    set_image_layout(
        command_buffer,
        hdr_image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        subresource_range,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    VkBufferImageCopy region = (VkBufferImageCopy){
        0, // bufferOffset
        0, // bufferRowLength
        0, // bufferImageHeight
        {
            VK_IMAGE_ASPECT_COLOR_BIT, // aspectMask
            0,                         // mipLevel
            0,                         // baseArrayLayer
            1,                         // layerCount
        },                             // imageSubresource
        {0, 0, 0},                     // imageOffset
        {hdr_width, hdr_height, 1},    // imageExtent
    };

    vkCmdCopyBufferToImage(
        command_buffer,
        staging_buffer,
        hdr_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &region);

    set_image_layout(
        command_buffer,
        hdr_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        subresource_range,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    end_single_time_command_buffer(command_buffer);

    VK_CHECK(vkDeviceWaitIdle(g_device));
    vmaDestroyBuffer(g_gpu_allocator, staging_buffer, staging_allocation);
  }

  // Create hdrDescriptorSet
  VkDescriptorSet descriptor_set;
  {
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.pNext = NULL;
    alloc_info.descriptorPool = g_descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &g_bake_cubemap_descriptor_set_layout;

    VK_CHECK(vkAllocateDescriptorSets(g_device, &alloc_info, &descriptor_set));
  }

  {
    VkDescriptorImageInfo image_descriptor = {
        hdr_sampler,
        hdr_image_view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkWriteDescriptorSet descriptor_write = {
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        NULL,
        descriptor_set,                            // dstSet
        0,                                         // dstBinding
        0,                                         // dstArrayElement
        1,                                         // descriptorCount
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // descriptorType
        &image_descriptor,                         // pImageInfo
        NULL,                                      // pBufferInfo
        NULL,                                      // pTexelBufferView
    };

    vkUpdateDescriptorSets(g_device, 1, &descriptor_write, 0, NULL);
  }

  // Camera matrices
  camera_uniform_t camera_ubo;
  camera_ubo.proj = mat4_perspective(to_radians(90.0f), 1.0f, 0.1f, 10.0f);

  canvas_t canvas;
  canvas_init(
      &canvas, dest_cubemap->width, dest_cubemap->height, dest_cubemap->format);

  // Create pipeline
  VkShaderModule vertex_module;
  VkShaderModule fragment_module;

  size_t vertex_code_size;
  unsigned char *vertex_code =
      load_bytes_from_file(vert_path, &vertex_code_size);

  size_t fragment_code_size;
  unsigned char *fragment_code =
      load_bytes_from_file(frag_path, &fragment_code_size);

  // Vertex module
  {
    VkShaderModuleCreateInfo create_info = {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        NULL,
        0,
        vertex_code_size,
        (uint32_t *)vertex_code};

    VK_CHECK(
        vkCreateShaderModule(g_device, &create_info, NULL, &vertex_module));
  }

  // Fragment module
  {
    VkShaderModuleCreateInfo create_info = {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        NULL,
        0,
        fragment_code_size,
        (uint32_t *)fragment_code};

    VK_CHECK(
        vkCreateShaderModule(g_device, &create_info, NULL, &fragment_module));
  }

  free(vertex_code);
  free(fragment_code);

  VkDescriptorSetLayout set_layouts[] = {
      g_bake_cubemap_descriptor_set_layout,
  };

  VkPipelineLayout pipeline_layout;

  VkPushConstantRange push_constant_range = {};
  push_constant_range.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  push_constant_range.offset = 0;
  push_constant_range.size = 128;

  VkPipelineLayoutCreateInfo create_info;
  create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  create_info.pNext = NULL;
  create_info.flags = 0;
  create_info.setLayoutCount = ARRAYSIZE(set_layouts);
  create_info.pSetLayouts = set_layouts;
  create_info.pushConstantRangeCount = 1;
  create_info.pPushConstantRanges = &push_constant_range;

  VK_CHECK(
      vkCreatePipelineLayout(g_device, &create_info, NULL, &pipeline_layout));

  VkPipeline pipeline;

  VkGraphicsPipelineCreateInfo pipeline_create_info =
      default_pipeline_create_info(
          vertex_module, fragment_module, pipeline_layout, canvas.render_pass);

  VK_CHECK(vkCreateGraphicsPipelines(
      g_device, VK_NULL_HANDLE, 1, &pipeline_create_info, NULL, &pipeline));

  // Allocate command buffer
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  {
    VkCommandBufferAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.pNext = NULL;
    allocate_info.commandPool = g_command_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;

    vkAllocateCommandBuffers(g_device, &allocate_info, &command_buffer);
  }

  // Begin command buffer
  VkCommandBufferBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = 0;
  begin_info.pInheritanceInfo = NULL;

  VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));

  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

  vkCmdBindDescriptorSets(
      command_buffer,
      VK_PIPELINE_BIND_POINT_GRAPHICS,
      pipeline_layout,
      0, // firstSet
      1,
      &descriptor_set,
      0,
      NULL);

  for (size_t i = 0; i < ARRAYSIZE(camera_views); i++) {
    canvas_begin(&canvas, command_buffer);

    camera_ubo.view = camera_views[i];

    vkCmdPushConstants(
        command_buffer,
        pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(camera_uniform_t),
        &camera_ubo);

    vkCmdDraw(command_buffer, 36, 1, 0, 0);

    canvas_end(&canvas, command_buffer);

    copy_side_image_to_cubemap(
        command_buffer, canvas.image, dest_cubemap, i, level);
  }

  VK_CHECK(vkEndCommandBuffer(command_buffer));

  // Submit
  VkPipelineStageFlags wait_dst_stage_mask =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

  VkSubmitInfo submit_info = {
      VK_STRUCTURE_TYPE_SUBMIT_INFO, // sType
      NULL,                          // pNext
      0,                             // waitSemaphoreCount
      NULL,                          // pWaitSemaphores
      &wait_dst_stage_mask,          // pWaitDstStageMask
      1,                             // commandBufferCount
      &command_buffer,               // pCommandBuffers
      0,                             // signalSemaphoreCount
      NULL,                          // pSignalSemaphores
  };

  vkQueueSubmit(g_graphics_queue, 1, &submit_info, VK_NULL_HANDLE);

  VK_CHECK(vkDeviceWaitIdle(g_device));

  vkDestroyImageView(g_device, hdr_image_view, NULL);
  vkDestroySampler(g_device, hdr_sampler, NULL);
  vmaDestroyImage(g_gpu_allocator, hdr_image, hdr_allocation);

  vkFreeCommandBuffers(g_device, g_command_pool, 1, &command_buffer);

  vkFreeDescriptorSets(g_device, g_descriptor_pool, 1, &descriptor_set);

  stbi_image_free(hdr_data);

  canvas_destroy(&canvas);

  vkDestroyShaderModule(g_device, vertex_module, NULL);

  vkDestroyShaderModule(g_device, fragment_module, NULL);

  vkDestroyPipeline(g_device, pipeline, NULL);
  vkDestroyPipelineLayout(g_device, pipeline_layout, NULL);
}

static void render_cubemap_to_cubemap(
    cubemap_t *dest_cubemap,
    cubemap_t *source_cubemap,
    const char *vert_path,
    const char *frag_path,
    uint32_t level) {
  // Create hdrDescriptorSet
  VkDescriptorSet descriptor_set;
  {
    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.pNext = NULL;
    alloc_info.descriptorPool = g_descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &g_bake_cubemap_descriptor_set_layout;

    VK_CHECK(vkAllocateDescriptorSets(g_device, &alloc_info, &descriptor_set));
  }

  {
    VkDescriptorImageInfo image_descriptor = {
        source_cubemap->sampler,
        source_cubemap->image_view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkWriteDescriptorSet descriptor_write = {
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        NULL,
        descriptor_set,                            // dstSet
        0,                                         // dstBinding
        0,                                         // dstArrayElement
        1,                                         // descriptorCount
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // descriptorType
        &image_descriptor,                         // pImageInfo
        NULL,                                      // pBufferInfo
        NULL,                                      // pTexelBufferView
    };

    vkUpdateDescriptorSets(g_device, 1, &descriptor_write, 0, NULL);
  }

  // Camera matrices
  camera_uniform_t camera_ubo;
  camera_ubo.proj = mat4_perspective(to_radians(90.0f), 1.0f, 0.1f, 10.0f);

  canvas_t canvas;
  canvas_init(
      &canvas, dest_cubemap->width, dest_cubemap->height, dest_cubemap->format);

  // Create pipeline
  VkShaderModule vertex_module;
  VkShaderModule fragment_module;

  size_t vertex_code_size;
  unsigned char *vertex_code =
      load_bytes_from_file(vert_path, &vertex_code_size);

  size_t fragment_code_size;
  unsigned char *fragment_code =
      load_bytes_from_file(frag_path, &fragment_code_size);

  // Vertex module
  {
    VkShaderModuleCreateInfo create_info = {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        NULL,
        0,
        vertex_code_size,
        (uint32_t *)vertex_code};

    VK_CHECK(
        vkCreateShaderModule(g_device, &create_info, NULL, &vertex_module));
  }

  // Fragment module
  {
    VkShaderModuleCreateInfo create_info = {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        NULL,
        0,
        fragment_code_size,
        (uint32_t *)fragment_code};

    VK_CHECK(
        vkCreateShaderModule(g_device, &create_info, NULL, &fragment_module));
  }

  free(vertex_code);
  free(fragment_code);

  VkDescriptorSetLayout set_layouts[] = {
      g_bake_cubemap_descriptor_set_layout,
  };

  VkPipelineLayout pipeline_layout;

  VkPushConstantRange push_constant_range = {};
  push_constant_range.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  push_constant_range.offset = 0;
  push_constant_range.size = 128;

  VkPipelineLayoutCreateInfo create_info;
  create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  create_info.pNext = NULL;
  create_info.flags = 0;
  create_info.setLayoutCount = ARRAYSIZE(set_layouts);
  create_info.pSetLayouts = set_layouts;
  create_info.pushConstantRangeCount = 1;
  create_info.pPushConstantRanges = &push_constant_range;

  VK_CHECK(
      vkCreatePipelineLayout(g_device, &create_info, NULL, &pipeline_layout));

  VkPipeline pipeline;

  VkGraphicsPipelineCreateInfo pipeline_create_info =
      default_pipeline_create_info(
          vertex_module, fragment_module, pipeline_layout, canvas.render_pass);

  VK_CHECK(vkCreateGraphicsPipelines(
      g_device, VK_NULL_HANDLE, 1, &pipeline_create_info, NULL, &pipeline));

  // Allocate command buffer
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  {
    VkCommandBufferAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.pNext = NULL;
    allocate_info.commandPool = g_command_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;

    vkAllocateCommandBuffers(g_device, &allocate_info, &command_buffer);
  }

  // Begin command buffer
  VkCommandBufferBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = 0;
  begin_info.pInheritanceInfo = NULL;

  VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));

  VkImageSubresourceRange subresource_range = {};
  subresource_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  subresource_range.baseMipLevel = 0;
  subresource_range.levelCount = 1;
  subresource_range.baseArrayLayer = 0;
  subresource_range.layerCount = 6;

  set_image_layout(
      command_buffer,
      source_cubemap->image,
      VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      subresource_range,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

  vkCmdBindDescriptorSets(
      command_buffer,
      VK_PIPELINE_BIND_POINT_GRAPHICS,
      pipeline_layout,
      0, // firstSet
      1,
      &descriptor_set,
      0,
      NULL);

  for (size_t i = 0; i < ARRAYSIZE(camera_views); i++) {
    canvas_begin(&canvas, command_buffer);

    camera_ubo.view = camera_views[i];

    vkCmdPushConstants(
        command_buffer,
        pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(camera_uniform_t),
        &camera_ubo);

    vkCmdDraw(command_buffer, 36, 1, 0, 0);

    canvas_end(&canvas, command_buffer);

    copy_side_image_to_cubemap(
        command_buffer, canvas.image, dest_cubemap, i, level);
  }

  VK_CHECK(vkEndCommandBuffer(command_buffer));

  // Submit
  VkPipelineStageFlags wait_dst_stage_mask =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

  VkSubmitInfo submit_info = {
      VK_STRUCTURE_TYPE_SUBMIT_INFO, // sType
      NULL,                          // pNext
      0,                             // waitSemaphoreCount
      NULL,                          // pWaitSemaphores
      &wait_dst_stage_mask,          // pWaitDstStageMask
      1,                             // commandBufferCount
      &command_buffer,               // pCommandBuffers
      0,                             // signalSemaphoreCount
      NULL,                          // pSignalSemaphores
  };

  vkQueueSubmit(g_graphics_queue, 1, &submit_info, VK_NULL_HANDLE);

  VK_CHECK(vkDeviceWaitIdle(g_device));

  vkFreeCommandBuffers(g_device, g_command_pool, 1, &command_buffer);

  vkFreeDescriptorSets(g_device, g_descriptor_pool, 1, &descriptor_set);

  canvas_destroy(&canvas);

  vkDestroyShaderModule(g_device, vertex_module, NULL);

  vkDestroyShaderModule(g_device, fragment_module, NULL);

  vkDestroyPipeline(g_device, pipeline, NULL);
  vkDestroyPipelineLayout(g_device, pipeline_layout, NULL);
}

static void create_cubemap_image(
    VkImage *image,
    VmaAllocation *allocation,
    VkImageView *image_view,
    VkSampler *sampler,
    VkFormat format,
    uint32_t width,
    uint32_t height,
    uint32_t levels) {
  VkImageCreateInfo image_create_info = (VkImageCreateInfo){
      VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      NULL,                                // pNext
      VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, // flags
      VK_IMAGE_TYPE_2D,                    // imageType
      format,                              // format
      {
          width,               // width
          height,              // height
          1,                   // depth
      },                       // extent
      levels,                  // mipLevels
      6,                       // arrayLayers
      VK_SAMPLE_COUNT_1_BIT,   // samples
      VK_IMAGE_TILING_OPTIMAL, // tiling
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
          VK_IMAGE_USAGE_SAMPLED_BIT, // usage
      VK_SHARING_MODE_EXCLUSIVE,      // sharingMode
      1,                              // queueFamilyIndexCount
      &g_graphics_queue_family_index, // pQueueFamilyIndices
      VK_IMAGE_LAYOUT_UNDEFINED,      // initialLayout
  };

  VmaAllocationCreateInfo image_alloc_create_info = {};
  image_alloc_create_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;

  VK_CHECK(vmaCreateImage(
      g_gpu_allocator,
      &image_create_info,
      &image_alloc_create_info,
      image,
      allocation,
      NULL));

  VkImageViewCreateInfo image_view_create_info = (VkImageViewCreateInfo){
      VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      NULL,
      0,                       // flags
      *image,                  // image
      VK_IMAGE_VIEW_TYPE_CUBE, // viewType
      format,                  // format
      {
          VK_COMPONENT_SWIZZLE_IDENTITY, // r
          VK_COMPONENT_SWIZZLE_IDENTITY, // g
          VK_COMPONENT_SWIZZLE_IDENTITY, // b
          VK_COMPONENT_SWIZZLE_IDENTITY, // a
      },                                 // components
      {
          VK_IMAGE_ASPECT_COLOR_BIT, // aspectMask
          0,                         // baseMipLevel
          levels,                    // levelCount
          0,                         // baseArrayLayer
          6,                         // layerCount
      },                             // subresourceRange
  };

  VK_CHECK(
      vkCreateImageView(g_device, &image_view_create_info, NULL, image_view));

  VkSamplerCreateInfo sampler_create_info = (VkSamplerCreateInfo){
      VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      NULL,
      0,                                       // flags
      VK_FILTER_LINEAR,                        // magFilter
      VK_FILTER_LINEAR,                        // minFilter
      VK_SAMPLER_MIPMAP_MODE_LINEAR,           // mipmapMode
      VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, // addressModeU
      VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, // addressModeV
      VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, // addressModeW
      0.0f,                                    // mipLodBias
      VK_FALSE,                                // anisotropyEnable
      1.0f,                                    // maxAnisotropy
      VK_FALSE,                                // compareEnable
      VK_COMPARE_OP_NEVER,                     // compareOp
      0.0f,                                    // minLod
      (float)levels,                           // maxLod
      VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, // borderColor
      VK_FALSE,                                // unnormalizedCoordinates
  };

  VK_CHECK(vkCreateSampler(g_device, &sampler_create_info, NULL, sampler));
}

void cubemap_init_skybox_from_hdr_equirec(
    cubemap_t *skybox_cubemap,
    const char *path,
    const uint32_t width,
    const uint32_t height,
    const char *vert_path,
    const char *frag_path) {
  skybox_cubemap->width = width;
  skybox_cubemap->height = height;
  skybox_cubemap->format = VK_FORMAT_R32G32B32A32_SFLOAT;

  create_cubemap_image(
      &skybox_cubemap->image,
      &skybox_cubemap->allocation,
      &skybox_cubemap->image_view,
      &skybox_cubemap->sampler,
      skybox_cubemap->format,
      width,
      height,
      1);

  render_equirec_to_cubemap(path, skybox_cubemap, 0, vert_path, frag_path);
}

void cubemap_init_irradiance_from_skybox(
    cubemap_t *irradiance_cubemap,
    cubemap_t *skybox_cubemap,
    const uint32_t width,
    const uint32_t height,
    const char *vert_path,
    const char *frag_path) {
  irradiance_cubemap->width = width;
  irradiance_cubemap->height = height;
  irradiance_cubemap->format = VK_FORMAT_R32G32B32A32_SFLOAT;

  create_cubemap_image(
      &irradiance_cubemap->image,
      &irradiance_cubemap->allocation,
      &irradiance_cubemap->image_view,
      &irradiance_cubemap->sampler,
      irradiance_cubemap->format,
      width,
      height,
      1);

  render_cubemap_to_cubemap(
      irradiance_cubemap, skybox_cubemap, vert_path, frag_path, 0);
}

void cubemap_destroy(cubemap_t *cubemap) {
  VK_CHECK(vkDeviceWaitIdle(g_device));

  vkDestroyImageView(g_device, cubemap->image_view, NULL);
  vkDestroySampler(g_device, cubemap->sampler, NULL);
  vmaDestroyImage(g_gpu_allocator, cubemap->image, cubemap->allocation);
}

void save_cubemap(cubemap_t *cubemap, const char *prefix) {
  size_t hdr_size = cubemap->width * cubemap->height * 4 * sizeof(float);

  VkBuffer staging_buffer;
  VmaAllocation staging_allocation;

  create_buffer(
      &staging_buffer,
      &staging_allocation,
      hdr_size,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VMA_MEMORY_USAGE_CPU_ONLY,
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  void *staging_memory_pointer;
  vmaMapMemory(g_gpu_allocator, staging_allocation, &staging_memory_pointer);

  for (uint32_t layer = 0; layer < 6; layer++) {
    VkCommandBuffer command_buffer = begin_single_time_command_buffer();

    VkImageSubresourceRange subresource_range = {};
    subresource_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subresource_range.baseMipLevel = 0;
    subresource_range.levelCount = 1;
    subresource_range.baseArrayLayer = layer;
    subresource_range.layerCount = 1;

    set_image_layout(
        command_buffer,
        cubemap->image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        subresource_range,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    VkBufferImageCopy region = (VkBufferImageCopy){
        0, // bufferOffset
        0, // bufferRowLength
        0, // bufferImageHeight
        {
            VK_IMAGE_ASPECT_COLOR_BIT,        // aspectMask
            0,                                // mipLevel
            layer,                            // baseArrayLayer
            1,                                // layerCount
        },                                    // imageSubresource
        {0, 0, 0},                            // imageOffset
        {cubemap->width, cubemap->height, 1}, // imageExtent
    };

    vkCmdCopyImageToBuffer(
        command_buffer,
        cubemap->image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        staging_buffer,
        1,
        &region);

    set_image_layout(
        command_buffer,
        cubemap->image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        subresource_range,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    end_single_time_command_buffer(command_buffer);

    VK_CHECK(vkDeviceWaitIdle(g_device));

    // Save side
    char filename[512] = "";
    sprintf(filename, "%s_side_%d.hdr", prefix, layer);

    stbi_write_hdr(
        filename,
        (int)cubemap->width,
        (int)cubemap->height,
        4,
        (float *)staging_memory_pointer);
  }

  VK_CHECK(vkDeviceWaitIdle(g_device));
  vmaDestroyBuffer(g_gpu_allocator, staging_buffer, staging_allocation);
}

int main(int argc, char *argv[]) {
  if (argc <= 1) {
    // TODO: print help
    printf(
        "Usage: %s <path-to-skybox.hdr> [skybox prefix] [irradiance prefix]\n",
        argv[0]);
    return 0;
  }

  char *skybox_prefix = "skybox";
  char *irradiance_prefix = "irradiance";

  if (argc >= 3) {
    skybox_prefix = argv[2];
  }

  if (argc >= 4) {
    irradiance_prefix = argv[3];
  }

  vulkan_setup();

  char *path = argv[1];
  uint32_t width = 512;
  uint32_t height = 512;

  // Skybox
  cubemap_t skybox_cubemap;
  cubemap_init_skybox_from_hdr_equirec(
      &skybox_cubemap,
      path,
      width,
      height,
      "../shaders/out/skybox.vert.spv",
      "../shaders/out/skybox.frag.spv");
  printf("Done rendering skybox\n");

  save_cubemap(&skybox_cubemap, skybox_prefix);
  printf("Done saving skybox\n");

  // Irradiance
  {
    cubemap_t irradiance_cubemap;
    cubemap_init_irradiance_from_skybox(
        &irradiance_cubemap,
        &skybox_cubemap,
        64,
        64,
        "../shaders/out/skybox.vert.spv",
        "../shaders/out/irradiance.frag.spv");
    printf("Done rendering irradiance\n");

    save_cubemap(&irradiance_cubemap, irradiance_prefix);
    printf("Done saving irradiance\n");

    cubemap_destroy(&irradiance_cubemap);
  }

  cubemap_destroy(&skybox_cubemap);

  vulkan_teardown();

  return 0;
}
