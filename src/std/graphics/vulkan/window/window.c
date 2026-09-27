#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
  SDL_Window *window;
  SDL_GPUDevice *device;
  SDL_GPUGraphicsPipeline *pipeline;
} KstdVulkanWindow;

void kstd_vulkan_window_destroy(KstdVulkanWindow *context) {
  if (!context) {
    return;
  }
  if (context->device) {
    SDL_WaitForGPUIdle(context->device);
    if (context->pipeline) {
      SDL_ReleaseGPUGraphicsPipeline(context->device, context->pipeline);
    }
    if (context->window) {
      SDL_ReleaseWindowFromGPUDevice(context->device, context->window);
    }
    SDL_DestroyGPUDevice(context->device);
  }
  if (context->window) {
    SDL_DestroyWindow(context->window);
  }
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
  free(context);
}

KstdVulkanWindow *kstd_vulkan_window_create(const char *title, int width,
                                             int height) {
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    return NULL;
  }
  KstdVulkanWindow *context = calloc(1, sizeof(*context));
  if (!context) {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return NULL;
  }
  context->window = SDL_CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE);
  if (!context->window) {
    kstd_vulkan_window_destroy(context);
    return NULL;
  }
  context->device =
      SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, "vulkan");
  if (!context->device) {
    kstd_vulkan_window_destroy(context);
    return NULL;
  }
  if (!SDL_ClaimWindowForGPUDevice(context->device, context->window)) {
    kstd_vulkan_window_destroy(context);
    return NULL;
  }
  return context;
}

int kstd_vulkan_window_set_pipeline(KstdVulkanWindow *context,
                                    const uint8_t *vertex, size_t vertex_size,
                                    const uint8_t *fragment,
                                    size_t fragment_size) {
  if (!context || !vertex || !fragment || vertex_size == 0 ||
      fragment_size == 0 || vertex_size % 4 != 0 || fragment_size % 4 != 0) {
    return 0;
  }
  SDL_GPUShaderCreateInfo vertex_info = {0};
  vertex_info.code = vertex;
  vertex_info.code_size = vertex_size;
  vertex_info.entrypoint = "main";
  vertex_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  vertex_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  SDL_GPUShader *vertex_shader =
      SDL_CreateGPUShader(context->device, &vertex_info);
  if (!vertex_shader) {
    return 0;
  }
  SDL_GPUShaderCreateInfo fragment_info = vertex_info;
  fragment_info.code = fragment;
  fragment_info.code_size = fragment_size;
  fragment_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  SDL_GPUShader *fragment_shader =
      SDL_CreateGPUShader(context->device, &fragment_info);
  if (!fragment_shader) {
    SDL_ReleaseGPUShader(context->device, vertex_shader);
    return 0;
  }
  SDL_GPUColorTargetDescription color_target = {0};
  color_target.format =
      SDL_GetGPUSwapchainTextureFormat(context->device, context->window);
  SDL_GPUGraphicsPipelineCreateInfo pipeline_info = {0};
  pipeline_info.vertex_shader = vertex_shader;
  pipeline_info.fragment_shader = fragment_shader;
  pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  pipeline_info.target_info.color_target_descriptions = &color_target;
  pipeline_info.target_info.num_color_targets = 1;
  SDL_GPUGraphicsPipeline *pipeline =
      SDL_CreateGPUGraphicsPipeline(context->device, &pipeline_info);
  SDL_ReleaseGPUShader(context->device, vertex_shader);
  SDL_ReleaseGPUShader(context->device, fragment_shader);
  if (!pipeline) {
    return 0;
  }
  if (context->pipeline) {
    SDL_WaitForGPUIdle(context->device);
    SDL_ReleaseGPUGraphicsPipeline(context->device, context->pipeline);
  }
  context->pipeline = pipeline;
  return 1;
}

int kstd_vulkan_window_draw(KstdVulkanWindow *context, uint32_t vertex_count,
                            float red, float green, float blue, float alpha) {
  if (!context || !context->pipeline || vertex_count == 0) {
    return 0;
  }
  SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(context->device);
  if (!commands) {
    return 0;
  }
  SDL_GPUTexture *target = NULL;
  if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, context->window,
                                             &target, NULL, NULL)) {
    SDL_CancelGPUCommandBuffer(commands);
    return 0;
  }
  if (!target) {
    return SDL_SubmitGPUCommandBuffer(commands) ? 1 : 0;
  }
  SDL_GPUColorTargetInfo color = {0};
  color.texture = target;
  color.clear_color = (SDL_FColor){red, green, blue, alpha};
  color.load_op = SDL_GPU_LOADOP_CLEAR;
  color.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &color, 1, NULL);
  if (!pass) {
    SDL_SubmitGPUCommandBuffer(commands);
    return 0;
  }
  SDL_BindGPUGraphicsPipeline(pass, context->pipeline);
  SDL_DrawGPUPrimitives(pass, vertex_count, 1, 0, 0);
  SDL_EndGPURenderPass(pass);
  return SDL_SubmitGPUCommandBuffer(commands) ? 1 : 0;
}

int kstd_vulkan_window_poll(KstdVulkanWindow *context) {
  if (!context) {
    return 0;
  }
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT ||
        event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
      return 0;
    }
  }
  return 1;
}

const char *kstd_vulkan_window_error(void) { return SDL_GetError(); }
