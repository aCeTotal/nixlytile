#include <vulkan/vulkan.h>
#include "platform/vulkan/pipeline/pipeline.h"

static const VkVertexInputBindingDescription instance_binding = {
	.binding = 0,
	.stride = NX_VK_INSTANCE_SIZE,
	.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE,
};

static const VkVertexInputAttributeDescription instance_attr = {
	.location = 0,
	.binding = 0,
	.format = VK_FORMAT_R32G32B32A32_SFLOAT,
	.offset = 0,
};

static const VkPipelineVertexInputStateCreateInfo vertex_input = {
	.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	.vertexBindingDescriptionCount = 1,
	.pVertexBindingDescriptions = &instance_binding,
	.vertexAttributeDescriptionCount = 1,
	.pVertexAttributeDescriptions = &instance_attr,
};

static const VkPipelineInputAssemblyStateCreateInfo assembly = {
	.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
};

static const VkPipelineRasterizationStateCreateInfo rasterization = {
	.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	.polygonMode = VK_POLYGON_MODE_FILL,
	.cullMode = VK_CULL_MODE_NONE,
	.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	.lineWidth = 1.f,
};

static const VkPipelineMultisampleStateCreateInfo multisample = {
	.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
};

static const VkPipelineViewportStateCreateInfo viewport = {
	.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	.viewportCount = 1,
	.scissorCount = 1,
};

static const VkDynamicState dyn_states[] = {
	VK_DYNAMIC_STATE_VIEWPORT,
	VK_DYNAMIC_STATE_SCISSOR,
};

static const VkPipelineDynamicStateCreateInfo dynamic = {
	.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	.pDynamicStates = dyn_states,
	.dynamicStateCount = sizeof(dyn_states) / sizeof(dyn_states[0]),
};

// Instanced quads, dynamic viewport.
void vulkan_pipeline_set_fixed_state(VkGraphicsPipelineCreateInfo *info) {
	info->pVertexInputState = &vertex_input;
	info->pInputAssemblyState = &assembly;
	info->pRasterizationState = &rasterization;
	info->pMultisampleState = &multisample;
	info->pViewportState = &viewport;
	info->pDynamicState = &dynamic;
}
